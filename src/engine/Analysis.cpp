#include "Analysis.h"
#include "GrainEngine.h" // hermite()

namespace keepsake::analysis
{
    namespace
    {
        constexpr int kFrameSize = WavetableSet::kFrameSize;
        constexpr int kNumMipLevels = WavetableSet::kNumMipLevels;
        constexpr int kFFTOrder = 11; // 2^11 == kFrameSize
        constexpr int kWrapFadeSamples = 64; // ~3% of a frame

        float readClamped (const float* data, int numSamples, int i) noexcept
        {
            return data[juce::jlimit (0, numSamples - 1, i)];
        }

        /** Hermite read at a fractional position, edge-clamped. */
        float readFractional (const float* data, int numSamples, double position) noexcept
        {
            const auto i = (int) std::floor (position);
            const auto frac = (float) (position - (double) i);

            return hermite (readClamped (data, numSamples, i - 1),
                            readClamped (data, numSamples, i),
                            readClamped (data, numSamples, i + 1),
                            readClamped (data, numSamples, i + 2),
                            frac);
        }
    }

    double rootFrequencyHz (double rootNoteMidi, double rootCents) noexcept
    {
        return noteFrequencyHz (rootNoteMidi + rootCents * 0.01);
    }

    double noteFrequencyHz (double midiNote) noexcept
    {
        return 440.0 * std::pow (2.0, (midiNote - 69.0) / 12.0);
    }

    bool fundamentalPhaseAt (const float* mono, int numSamples,
                             double centre, double f0, double sampleRate,
                             double& phaseOut) noexcept
    {
        if (numSamples <= 0 || f0 <= 0.0 || sampleRate <= 0.0)
            return false;

        const auto period = sampleRate / f0;
        const auto halfSpan = juce::jmax (4.0, 1.5 * period);
        const auto from = (int) std::floor (centre - halfSpan);
        const auto to = (int) std::ceil (centre + halfSpan);

        const auto omega = juce::MathConstants<double>::twoPi * f0 / sampleRate;
        const auto windowScale = juce::MathConstants<double>::pi / halfSpan;

        double re = 0.0, im = 0.0, windowedPower = 0.0;

        for (int n = from; n <= to; ++n)
        {
            const auto t = (double) n - centre;
            const auto hann = 0.5 + 0.5 * std::cos (t * windowScale); // 1 at centre, 0 at edges
            const auto x = (double) readClamped (mono, numSamples, n) * hann;

            re += x * std::cos (omega * t);
            im -= x * std::sin (omega * t);
            windowedPower += x * x;
        }

        const auto magnitude = std::sqrt (re * re + im * im);

        // Silence guard: no meaningful fundamental energy -> caller slices at the
        // anchor verbatim. Threshold is generous; the projection is well-defined on
        // anything that is not effectively silent.
        if (magnitude < 1.0e-6 || windowedPower < 1.0e-10)
            return false;

        phaseOut = std::atan2 (im, re);
        return true;
    }

    void extractResampledCycle (const float* mono, int numSamples,
                                double start, double periodSamples,
                                float* outFrame)
    {
        jassert (periodSamples > 0.0);

        const auto step = periodSamples / (double) kFrameSize;

        for (int k = 0; k < kFrameSize; ++k)
            outFrame[k] = readFractional (mono, numSamples, start + (double) k * step);

        // Wrap crossfade: blend the continuation just past the cycle end into the
        // frame start, so a slice of aperiodic material still loops without a step.
        // (The FFT round trip would only convert a step into broadband harmonic
        // buzz - alias-safe but ugly. This tames it at the source.)
        for (int i = 0; i < kWrapFadeSamples; ++i)
        {
            const auto w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi
                                                   * ((float) i + 0.5f) / (float) kWrapFadeSamples);
            const auto continuation =
                readFractional (mono, numSamples,
                                start + (double) (kFrameSize + i) * step);

            outFrame[i] = w * outFrame[i] + (1.0f - w) * continuation;
        }
    }

    void buildMipChain (const float* frame, float* dest)
    {
        // juce::dsp::FFT's constructor allocates - which is exactly why all of this
        // lives here (worker/test thread) and never on the audio thread.
        juce::dsp::FFT fft (kFFTOrder);

        // Real-only forward transform: input in the first half, result is
        // kFrameSize/2 + 1 interleaved complex bins.
        std::vector<float> spectrum ((size_t) kFrameSize * 2, 0.0f);
        std::copy (frame, frame + kFrameSize, spectrum.begin());
        fft.performRealOnlyForwardTransform (spectrum.data(), true);

        auto* bins = reinterpret_cast<std::complex<float>*> (spectrum.data());
        constexpr int numBins = kFrameSize / 2; // bin indices 0..1024 usable

        // DC out (offset steps when scanning frames), Nyquist out (unrepresentable).
        bins[0] = { 0.0f, 0.0f };
        bins[numBins] = { 0.0f, 0.0f };

        // Rotate so bin 1 has zero phase: multiply bin k by e^{-i*k*phi1}. An exact
        // fractional-sample circular rotation, and it fundamental-aligns every
        // frame - the reason frame morphs and set-swap crossfades don't cancel.
        const auto phi1 = std::arg (bins[1]);

        for (int k = 1; k < numBins; ++k)
            bins[k] *= std::polar (1.0f, -phi1 * (float) k);

        // Per level: copy, zero everything above the level's harmonic limit, invert.
        std::vector<float> levelSpectrum ((size_t) kFrameSize * 2);

        for (int level = 0; level < kNumMipLevels; ++level)
        {
            std::copy (spectrum.begin(), spectrum.end(), levelSpectrum.begin());
            auto* levelBins = reinterpret_cast<std::complex<float>*> (levelSpectrum.data());

            const auto keep = numBins >> level; // 1024, 512, ..., 1

            for (int k = keep + 1; k <= numBins; ++k)
                levelBins[k] = { 0.0f, 0.0f };

            fft.performRealOnlyInverseTransform (levelSpectrum.data());

            std::copy (levelSpectrum.begin(), levelSpectrum.begin() + kFrameSize,
                       dest + (size_t) level * kFrameSize);
        }
    }

    WavetableSet::Ptr buildWavetableSet (const SourceAudio& source,
                                         const CaptureWindow& window,
                                         double f0,
                                         int numFrames)
    {
        const auto windowLength = window.numSamples;

        if (windowLength <= 0 || source.getNumSamples() <= 0 || f0 <= 0.0)
            return {};

        numFrames = juce::jlimit (1, 64, numFrames);
        const auto sampleRate = source.sampleRate;
        auto period = sampleRate / f0;

        // Mono copy of the window plus a guard past its end: a cycle sliced near
        // the window edge (or the wrap-fade continuation) reads a little beyond it,
        // and real continuation from the source beats clamped repetition. Clamped
        // at the source end regardless.
        const auto guard = (int) std::ceil (period) + kFrameSize / 8 + 8;
        const auto copyLength = juce::jmin (windowLength + guard,
                                            source.getNumSamples() - window.startSample);

        std::vector<float> mono ((size_t) copyLength);
        {
            const auto channels = source.getNumChannels();
            const auto gain = 1.0f / (float) channels;

            for (int i = 0; i < copyLength; ++i)
            {
                float sum = 0.0f;

                for (int ch = 0; ch < channels; ++ch)
                    sum += source.buffer.getSample (ch, window.startSample + i);

                mono[(size_t) i] = sum * gain;
            }
        }

        // Sub-period window (10ms window + low Root is reachable from the UI):
        // the whole window is the cycle. Defined behaviour, not an error.
        if ((double) windowLength < period)
            period = (double) windowLength;

        // How many distinct cycles actually fit; duplicates fill the rest.
        const auto distinct = juce::jlimit (1, numFrames,
                                            (int) std::floor ((double) windowLength / period));

        WavetableSet::Ptr set (new WavetableSet());
        set->numFrames = numFrames;
        set->sampleRate = sampleRate;
        set->rootHz = f0;
        set->data.assign ((size_t) numFrames * kNumMipLevels * kFrameSize, 0.0f);

        std::vector<float> cycle ((size_t) kFrameSize);

        const auto maxStart = juce::jmax (0.0, (double) windowLength - period);
        const auto omega = juce::MathConstants<double>::twoPi * f0 / sampleRate;

        for (int frame = 0; frame < distinct; ++frame)
        {
            const auto anchor = distinct > 1
                                  ? maxStart * (double) frame / (double) (distinct - 1)
                                  : 0.0;

            // Slice at the fundamental's positive-going zero crossing nearest the
            // anchor. The FFT bin-1 rotation later normalizes phase exactly; this
            // step's remaining job is to place the wrap seam at a low-energy point
            // of the fundamental.
            auto start = anchor;
            double phase = 0.0;

            if (fundamentalPhaseAt (mono.data(), copyLength, anchor, f0, sampleRate, phase))
            {
                // local fundamental ~ cos(omega*(t-anchor) + phase); positive-going
                // zero at omega*dt + phase = -pi/2 (mod 2*pi); pick dt nearest 0.
                auto dt = (-juce::MathConstants<double>::halfPi - phase) / omega;
                const auto periodAtF0 = juce::MathConstants<double>::twoPi / omega;
                dt -= periodAtF0 * std::round (dt / periodAtF0);
                start = juce::jlimit (0.0, (double) copyLength - 1.0, anchor + dt);
            }

            extractResampledCycle (mono.data(), copyLength, start, period, cycle.data());
            buildMipChain (cycle.data(),
                           set->data.data()
                               + (size_t) frame * kNumMipLevels * kFrameSize);
        }

        // Fill the remaining slots by duplicating the last distinct frame.
        for (int frame = distinct; frame < numFrames; ++frame)
            std::copy_n (set->getTable (distinct - 1, 0),
                         (size_t) kNumMipLevels * kFrameSize,
                         set->data.data() + (size_t) frame * kNumMipLevels * kFrameSize);

        return set;
    }
}
