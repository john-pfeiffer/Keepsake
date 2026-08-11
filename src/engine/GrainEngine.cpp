#include "GrainEngine.h"

#include <algorithm>

namespace keepsake
{
    // =========================================================================
    // Window shapes
    // =========================================================================

    float GrainWindow::hann (float t) noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * t);
    }

    float GrainWindow::tukey (float t, float plateau) noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        const auto taper = juce::jlimit (0.001f, 1.0f, 1.0f - plateau); // total taper fraction
        const auto half = taper * 0.5f;

        if (t < half)
            return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * t / half);

        if (t > 1.0f - half)
            return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (1.0f - t) / half);

        return 1.0f;
    }

    float GrainWindow::expodec (float t) noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t);

        // Fast rise (~3% of the grain) then exponential decay: the classic "expodec"
        // percussive grain. The rise is windowed too, so the grain still starts at 0.
        constexpr float rise = 0.03f;

        const auto attack = t < rise ? 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * t / rise)
                                     : 1.0f;
        const auto decay = std::exp (-5.0f * juce::jmax (0.0f, t - rise) / (1.0f - rise));

        // Force the tail to exactly zero so a grain can never end on a step.
        const auto tail = 1.0f - juce::jlimit (0.0f, 1.0f, (t - 0.9f) * 10.0f);

        return attack * decay * tail;
    }

    float GrainWindow::shape (float t, float morph) noexcept
    {
        morph = juce::jlimit (0.0f, 1.0f, morph);

        if (morph <= 0.5f)
        {
            const auto x = morph * 2.0f;
            return hann (t) * (1.0f - x) + tukey (t) * x;
        }

        const auto x = (morph - 0.5f) * 2.0f;
        return tukey (t) * (1.0f - x) + expodec (t) * x;
    }

    // =========================================================================
    // Interpolation
    // =========================================================================

    float hermite (float xm1, float x0, float x1, float x2, float frac) noexcept
    {
        const auto c = (x1 - xm1) * 0.5f;
        const auto v = x0 - x1;
        const auto w = c + v;
        const auto a = w + v + (x2 - x0) * 0.5f;
        const auto b = w + a;

        return ((((a * frac) - b) * frac + c) * frac + x0);
    }

    // =========================================================================
    // GrainEngine
    // =========================================================================

    void GrainEngine::prepare (double sampleRate)
    {
        currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
    }

    void GrainEngine::reset() noexcept
    {
        for (auto& g : grains)
            g.active = false;

        samplesUntilNextGrain = 0.0;
        warpPhase = 0.0; // the sweep is note-anchored, like the sync grid
        nextGrainSlot = 0;
        normaliseInitialised = false; // next note snaps to the right level immediately
    }

    int GrainEngine::getActiveGrainCount() const noexcept
    {
        int count = 0;

        for (const auto& g : grains)
            if (g.active)
                ++count;

        return count;
    }

    float GrainEngine::readSource (const SourceAudio& source, int channel, double position) const noexcept
    {
        const auto total = source.getNumSamples();

        if (total <= 0)
            return 0.0f;

        const auto i = (int) position;

        if (i < 0 || i >= total)
            return 0.0f;

        const auto frac = (float) (position - (double) i);
        const auto* data = source.buffer.getReadPointer (juce::jmin (channel, source.getNumChannels() - 1));

        // Clamp the Hermite taps at the buffer edges rather than wrapping, so the very
        // start and end of a file read as silence-adjacent instead of splicing.
        const auto xm1 = data[juce::jlimit (0, total - 1, i - 1)];
        const auto x0  = data[juce::jlimit (0, total - 1, i)];
        const auto x1  = data[juce::jlimit (0, total - 1, i + 1)];
        const auto x2  = data[juce::jlimit (0, total - 1, i + 2)];

        return hermite (xm1, x0, x1, x2, frac);
    }

    void GrainEngine::spawnGrain (const SourceAudio& source,
                                  const Settings& s,
                                  int windowStart,
                                  int windowLength,
                                  double lateSamples) noexcept
    {
        // Round-robin the pool. When all 32 are busy the oldest slot is simply
        // recycled - that is the documented cap, not an error.
        auto& g = grains[(size_t) nextGrainSlot];
        nextGrainSlot = (nextGrainSlot + 1) % kMaxGrains;

        // Grain size is clamped to the capture length (spec §2.2). In formant
        // mode it is also capped at four pitch periods: overlap beyond that
        // adds nothing audible but would exhaust the 32-grain pool on high
        // notes (a period at C8 is ~11 samples).
        auto lengthSamples = s.grainSizeMs * 0.001 * currentSampleRate;
        lengthSamples = juce::jmin (lengthSamples, (double) windowLength);

        if (s.formantIntervalSamples > 0.0)
            lengthSamples = juce::jmin (lengthSamples, 4.0 * s.formantIntervalSamples);

        lengthSamples = juce::jmax (2.0, lengthSamples);

        const auto positionSpan = (double) juce::jmax (0, windowLength - (int) lengthSamples);
        double readStart;

        if (s.warpPhaseIncrement > 0.0)
        {
            // Warp: the start follows the note-anchored sweep through the
            // window; Drift becomes symmetric jitter around that moving point.
            const auto base = (double) windowStart + warpPhase * positionSpan;
            const auto jitter = (rng.nextDouble() * 2.0 - 1.0) * s.drift * 0.5 * (double) windowLength;
            readStart = juce::jlimit ((double) windowStart, (double) windowStart + positionSpan, base + jitter);
        }
        else
        {
            // Drift jitters the grain start within the capture window.
            readStart = (double) windowStart + s.drift * positionSpan * rng.nextDouble();
        }

        if (s.snapToTransients && ! source.transients.empty())
        {
            // Quantise to the detected hits inside the window. Drift doubles
            // as the probability of shuffling to a random hit instead of the
            // nearest - at full Drift, Snap is a slice shuffler.
            const auto& hits = source.transients;
            const auto first = std::lower_bound (hits.begin(), hits.end(), windowStart);
            const auto last = std::lower_bound (first, hits.end(), windowStart + windowLength);

            if (first != last)
            {
                const auto count = (int) std::distance (first, last);

                if (count > 1 && rng.nextDouble() < s.drift)
                {
                    readStart = (double) *(first + rng.nextInt (count));
                }
                else
                {
                    auto it = std::lower_bound (first, last, (int) readStart);

                    if (it == last)
                        --it;
                    else if (it != first && readStart - (double) *(it - 1) < (double) *it - readStart)
                        --it;

                    readStart = (double) *it;
                }
            }
        }

        // Formant mode leaves the source unrepitched - the emission clock in
        // process() supplies the played pitch instead.
        double rate = s.formantIntervalSamples > 0.0 ? 1.0 : s.playbackRatio;

        if (s.shimmerCents > 0.0)
        {
            const auto cents = (rng.nextDouble() * 2.0 - 1.0) * s.shimmerCents;
            rate *= std::pow (2.0, cents / 1200.0);

            // At high Shimmer settings some grains jump an octave (spec §2.2).
            const auto octaveProbability = juce::jmax (0.0, (s.shimmerCents - 50.0) / 50.0) * 0.25;

            if (rng.nextDouble() < octaveProbability)
                rate *= 2.0;
        }

        g.active = true;
        g.readPos = readStart + lateSamples * rate;
        g.rate = rate;
        g.age = lateSamples;
        g.length = lengthSamples;
        g.windowMorph = (float) s.windowMorph;

        // Equal-power pan; Spread scales how far from centre a grain may land.
        const auto pan = (rng.nextDouble() * 2.0 - 1.0) * s.spread;
        const auto angle = (pan * 0.5 + 0.5) * juce::MathConstants<double>::halfPi;
        g.gainL = (float) std::cos (angle);
        g.gainR = (float) std::sin (angle);
    }

    void GrainEngine::process (juce::AudioBuffer<float>& output,
                               int startSample,
                               int numSamples,
                               const SourceAudio* source,
                               const Settings& settings) noexcept
    {
        if (source == nullptr || source->getNumSamples() <= 0 || numSamples <= 0)
            return;

        const auto window = CaptureWindow::resolve (*source,
                                                    settings.place,
                                                    settings.captureLengthMs,
                                                    currentSampleRate);

        if (window.numSamples <= 0)
            return;

        const auto density = juce::jlimit (0.5, 500.0, settings.densityPerSecond);
        // Tempo sync replaces the density-derived interval outright, and the
        // overlap compensation below follows it - level must track the REAL
        // emission rate, not the ignored Density knob. Formant mode outranks
        // both: its emission clock IS the played pitch, so neither Density
        // nor the tempo grid may touch it.
        const auto formant = settings.formantIntervalSamples > 0.0;
        const auto synced = ! formant && settings.syncIntervalSamples > 0.0;
        const auto samplesPerGrain = formant ? juce::jmax (2.0, settings.formantIntervalSamples)
                                   : synced  ? settings.syncIntervalSamples
                                             : currentSampleRate / density;

        // Grains are mutually uncorrelated, so their powers sum: compensating by
        // 1/sqrt(overlap) keeps perceived level roughly constant while Density and
        // Grain Size sweep. Without this, Density 2/s vs 200/s is a ~20dB jump.
        const auto grainLengthSamples =
            juce::jmax (2.0, juce::jmin (settings.grainSizeMs * 0.001 * currentSampleRate,
                                         (double) window.numSamples));
        const auto overlap = juce::jmax (1.0, grainLengthSamples / samplesPerGrain);
        const auto targetNormalise = (float) (1.0 / std::sqrt (overlap));

        if (! normaliseInitialised)
        {
            normaliseGain = targetNormalise;
            normaliseInitialised = true;
        }

        // One-pole smoothing (~20ms) so automating Density does not zipper.
        const auto smoothingCoeff = (float) std::exp (-1.0 / (0.02 * currentSampleRate));

        auto* left = output.getWritePointer (0, startSample);
        auto* right = output.getNumChannels() > 1 ? output.getWritePointer (1, startSample) : left;

        for (int n = 0; n < numSamples; ++n)
        {
            normaliseGain = targetNormalise + (normaliseGain - targetNormalise) * smoothingCoeff;

            if (samplesUntilNextGrain <= 0.0)
            {
                // Formant mode needs sub-sample spawn alignment: the pitch
                // period is rarely an integer number of samples, and grains
                // restart their carrier from the same source position, so a
                // spawn grid quantised to whole samples collapses the played
                // pitch into a subharmonic comb (440Hz at 48k -> a 40Hz
                // buzz). Pre-advancing each grain by its quantisation error
                // keeps the emission exactly periodic. Other modes stay at
                // 0 so their output is bit-identical to previous builds.
                const auto late = formant ? -samplesUntilNextGrain : 0.0;

                spawnGrain (*source, settings, window.startSample, window.numSamples, late);

                // Free mode jitters the interval slightly so low densities do
                // not buzz periodically. Sync mode is EXACT - the musical grid
                // is the point, and reset() at note-on anchors it to the note.
                // Formant mode is exact too: interval jitter there is pitch
                // jitter.
                const auto jitter = (synced || formant) ? 1.0 : 0.9 + 0.2 * rng.nextDouble();
                samplesUntilNextGrain += samplesPerGrain * jitter;
            }

            samplesUntilNextGrain -= 1.0;

            // Advanced every sample (not just at spawns) so chunked process()
            // calls accumulate identically - block-size independence.
            warpPhase += settings.warpPhaseIncrement;

            if (warpPhase >= 1.0)
                warpPhase -= std::floor (warpPhase);

            float sumL = 0.0f;
            float sumR = 0.0f;

            for (auto& g : grains)
            {
                if (! g.active)
                    continue;

                const auto t = (float) (g.age / g.length);

                if (t >= 1.0f)
                {
                    g.active = false;
                    continue;
                }

                const auto env = GrainWindow::shape (t, g.windowMorph);
                const auto l = readSource (*source, 0, g.readPos);
                const auto r = readSource (*source, 1, g.readPos);

                sumL += l * env * g.gainL;
                sumR += r * env * g.gainR;

                g.readPos += g.rate;
                g.age += 1.0;
            }

            left[n] += sumL * normaliseGain;
            right[n] += sumR * normaliseGain;
        }
    }
}
