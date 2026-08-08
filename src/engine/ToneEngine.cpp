#include "ToneEngine.h"
#include "GrainEngine.h" // hermite()

namespace keepsake
{
    namespace
    {
        constexpr int kFrameSize = WavetableSet::kFrameSize;
        constexpr int kMaxMip = WavetableSet::kNumMipLevels - 1;

        float readTable (const float* table, double tablePhase) noexcept
        {
            const auto i = (int) tablePhase;
            const auto frac = (float) (tablePhase - (double) i);

            // Circular: a wavetable frame is periodic by construction.
            const auto xm1 = table[(i - 1) & (kFrameSize - 1)];
            const auto x0  = table[i & (kFrameSize - 1)];
            const auto x1  = table[(i + 1) & (kFrameSize - 1)];
            const auto x2  = table[(i + 2) & (kFrameSize - 1)];

            return hermite (xm1, x0, x1, x2, frac);
        }
    }

    void ToneEngine::prepare (double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        fadeLength = juce::jmax (1.0, kSwapFadeSeconds * sampleRate);
        framePosSmoothed.reset (sampleRate, kFrameSmoothingSeconds);

        // Host restart: drop every held pointer so a stale set can be collected.
        currentSet.store (nullptr, std::memory_order_release);
        fadingSet.store (nullptr, std::memory_order_release);
        pendingSet.store (nullptr, std::memory_order_release);
        fadeRemaining = 0.0;
        fadingFromSilence = false;
        phase = 0.0;
    }

    void ToneEngine::noteOn (const WavetableSet* set) noexcept
    {
        currentSet.store (set, std::memory_order_release);
        fadingSet.store (nullptr, std::memory_order_release);
        pendingSet.store (nullptr, std::memory_order_release);
        fadeRemaining = 0.0;
        fadingFromSilence = false;
        phase = 0.0; // deterministic renders need a fixed phase at note start
        framePosSmoothed.setCurrentAndTargetValue (framePosSmoothed.getTargetValue());
    }

    float ToneEngine::readSample (const WavetableSet& set, double tablePhase,
                                  double levelF, double framePosF) const noexcept
    {
        const auto level0 = (int) levelF;
        const auto level1 = juce::jmin (level0 + 1, kMaxMip);
        const auto levelW = (float) (levelF - (double) level0);

        const auto frameScaled = framePosF * (double) (set.numFrames - 1);
        const auto frame0 = juce::jlimit (0, set.numFrames - 1, (int) frameScaled);
        const auto frame1 = juce::jmin (frame0 + 1, set.numFrames - 1);
        const auto frameW = (float) (frameScaled - (double) frame0);

        // Bilinear over {frame0,frame1} x {level0,level1}: 4 Hermite reads.
        const auto a = readTable (set.getTable (frame0, level0), tablePhase);
        const auto b = readTable (set.getTable (frame0, level1), tablePhase);
        const auto c = readTable (set.getTable (frame1, level0), tablePhase);
        const auto d = readTable (set.getTable (frame1, level1), tablePhase);

        const auto f0 = a + (b - a) * levelW;
        const auto f1 = c + (d - c) * levelW;

        return f0 + (f1 - f0) * frameW;
    }

    void ToneEngine::process (float* out, int numSamples,
                              const WavetableSet* latest,
                              double frequency,
                              double framePos) noexcept
    {
        auto* current = currentSet.load (std::memory_order_relaxed);
        auto* fading = fadingSet.load (std::memory_order_relaxed);

        // A new publication reaches a running voice as a crossfade. If a fade is
        // already running we can't retarget (a blend of two tables isn't a table),
        // so the newest set parks in pending and starts when the fade ends.
        if (latest != current && latest != nullptr)
        {
            if (fadeRemaining <= 0.0)
            {
                if (current == nullptr)
                {
                    // First set arriving mid-note: explicit fade-in from silence -
                    // a gain ramp, not a blend against a null read.
                    fadingFromSilence = true;
                    fading = nullptr;
                }
                else
                {
                    fadingFromSilence = false;
                    fading = current;
                }

                current = latest;
                fadeRemaining = fadeLength;
                currentSet.store (current, std::memory_order_release);
                fadingSet.store (fading, std::memory_order_release);
                pendingSet.store (nullptr, std::memory_order_release);
            }
            else
            {
                pendingSet.store (latest, std::memory_order_release);
            }
        }

        if (current == nullptr)
        {
            std::fill (out, out + numSamples, 0.0f);
            return;
        }

        framePosSmoothed.setTargetValue ((float) juce::jlimit (0.0, 1.0, framePos));

        const auto inc = frequency * (double) kFrameSize / sampleRate;

        // Safe-side level selection: a table keeping H = 1024>>L harmonics played
        // at increment inc has its top partial at H*f, alias-free only when
        // 2^L >= inc. log2(inc) alone blends a FLOOR level that keeps twice the
        // safe harmonic count - audibly aliased at the top of the keyboard - so
        // the continuous level is log2(inc) + 1, which keeps BOTH blended levels
        // inside the limit at every frequency (the +1 costs one octave of top-end
        // that only sub-23Hz fundamentals could have used anyway).
        const auto levelF = juce::jlimit (0.0, (double) kMaxMip,
                                          inc > 0.5 ? std::log2 (inc) + 1.0 : 0.0);
        const auto level = forcedMipLevel >= 0
                             ? (double) juce::jmin (forcedMipLevel, kMaxMip)
                             : levelF;

        for (int n = 0; n < numSamples; ++n)
        {
            const auto fpos = (double) framePosSmoothed.getNextValue();
            auto sample = readSample (*current, phase, level, fpos);

            if (fadeRemaining > 0.0)
            {
                // Linear fade: extraction fundamental-aligns the sets, so old and
                // new are correlated and linear sums identical material to unity.
                const auto newGain = (float) (1.0 - fadeRemaining / fadeLength);

                if (fadingFromSilence)
                    sample *= newGain;
                else if (fading != nullptr)
                    sample = sample * newGain
                             + readSample (*fading, phase, level, fpos) * (1.0f - newGain);

                fadeRemaining -= 1.0;

                if (fadeRemaining <= 0.0)
                {
                    // A set parked in pending is picked up by the latest!=current
                    // check at the top of the next process() call; the pending slot
                    // exists so the GC keeps that set alive until then.
                    fading = nullptr;
                    fadingFromSilence = false;
                    fadingSet.store (nullptr, std::memory_order_release);
                }
            }

            out[n] = sample;

            phase += inc;

            if (phase >= (double) kFrameSize)
                phase -= (double) kFrameSize;
        }
    }

    void ToneEngine::getPinnedSets (std::vector<const WavetableSet*>& outPinned) const
    {
        if (auto* p = currentSet.load (std::memory_order_acquire)) outPinned.push_back (p);
        if (auto* p = fadingSet.load (std::memory_order_acquire)) outPinned.push_back (p);
        if (auto* p = pendingSet.load (std::memory_order_acquire)) outPinned.push_back (p);
    }
}
