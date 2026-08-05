#pragma once

#include "engine/CaptureBuffer.h"
#include <juce_dsp/juce_dsp.h>

namespace ktest
{
    /** A sine at a known frequency - the reference material for pitch-related tests. */
    inline keepsake::SourceAudio::Ptr makeSineSource (double frequency,
                                                      double seconds,
                                                      double sampleRate,
                                                      const juce::String& name = "sine")
    {
        keepsake::SourceAudio::Ptr s (new keepsake::SourceAudio());
        s->sampleRate = sampleRate;
        s->name = name;

        const auto numSamples = (int) (seconds * sampleRate);
        s->buffer.setSize (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto v = (float) std::sin (juce::MathConstants<double>::twoPi * frequency
                                             * (double) i / sampleRate);
            s->buffer.setSample (0, i, v);
            s->buffer.setSample (1, i, v);
        }

        return s;
    }

    /** Largest absolute sample-to-sample step: a blunt but effective click detector. */
    inline float maxDiscontinuity (const juce::AudioBuffer<float>& buffer)
    {
        float worst = 0.0f;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* d = buffer.getReadPointer (ch);

            for (int i = 1; i < buffer.getNumSamples(); ++i)
                worst = juce::jmax (worst, std::abs (d[i] - d[i - 1]));
        }

        return worst;
    }

    inline bool isFinite (const juce::AudioBuffer<float>& buffer)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* d = buffer.getReadPointer (ch);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (! std::isfinite (d[i]))
                    return false;
        }

        return true;
    }

    /** Deterministic "vocal-like" source: a fixed sum of harmonics with 1/n
        rolloff at a known f0. Rich enough to exercise the mip chain, exactly
        periodic so extraction with the correct Root is well-posed. */
    inline keepsake::SourceAudio::Ptr makeHarmonicSource (double f0,
                                                          double seconds,
                                                          double sampleRate,
                                                          int numHarmonics = 30)
    {
        keepsake::SourceAudio::Ptr s (new keepsake::SourceAudio());
        s->sampleRate = sampleRate;
        s->name = "harmonic";

        const auto numSamples = (int) (seconds * sampleRate);
        s->buffer.setSize (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            double v = 0.0;

            for (int n = 1; n <= numHarmonics; ++n)
            {
                const auto fn = f0 * n;

                if (fn >= sampleRate * 0.45)
                    break;

                v += std::sin (juce::MathConstants<double>::twoPi * fn * (double) i / sampleRate) / (double) n;
            }

            const auto sample = (float) (v * 0.4);
            s->buffer.setSample (0, i, sample);
            s->buffer.setSample (1, i, sample);
        }

        return s;
    }

    /** Deterministic drum-like source: a seeded, exponentially decaying, gently
        lowpassed noise burst. Aperiodic on purpose - the "arbitrary Root" half of
        the M3 exit test. */
    inline keepsake::SourceAudio::Ptr makeDrumSource (double seconds,
                                                      double sampleRate,
                                                      juce::int64 seed = 777)
    {
        keepsake::SourceAudio::Ptr s (new keepsake::SourceAudio());
        s->sampleRate = sampleRate;
        s->name = "drum";

        const auto numSamples = (int) (seconds * sampleRate);
        s->buffer.setSize (2, numSamples);

        juce::Random rng (seed);
        float lp = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto env = (float) std::exp (-6.0 * (double) i / (double) numSamples);
            const auto noise = rng.nextFloat() * 2.0f - 1.0f;
            lp += 0.35f * (noise - lp); // one-pole lowpass tames the top end

            const auto sample = lp * env * 0.8f;
            s->buffer.setSample (0, i, sample);
            s->buffer.setSample (1, i, sample);
        }

        return s;
    }

    /** Power spectrum in dB of one channel, 4-term Blackman-Harris windowed.
        BH4's -92dB sidelobes keep window leakage far below the -50dB alias
        threshold; a Hann window's -31dB skirts would eat the test's margin. */
    inline std::vector<float> powerSpectrumDb (const juce::AudioBuffer<float>& buffer,
                                               int startSample,
                                               int fftOrder = 15)
    {
        const auto fftSize = 1 << fftOrder;
        jassert (startSample + fftSize <= buffer.getNumSamples());

        std::vector<float> windowed ((size_t) fftSize * 2, 0.0f);
        const auto* data = buffer.getReadPointer (0, startSample);

        constexpr double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;

        for (int i = 0; i < fftSize; ++i)
        {
            const auto t = juce::MathConstants<double>::twoPi * (double) i / (double) (fftSize - 1);
            const auto w = a0 - a1 * std::cos (t) + a2 * std::cos (2.0 * t) - a3 * std::cos (3.0 * t);
            windowed[(size_t) i] = data[i] * (float) w;
        }

        juce::dsp::FFT fft (fftOrder);
        fft.performRealOnlyForwardTransform (windowed.data(), true);

        const auto* bins = reinterpret_cast<const std::complex<float>*> (windowed.data());
        std::vector<float> db ((size_t) fftSize / 2);

        for (size_t k = 0; k < db.size(); ++k)
            db[k] = 10.0f * std::log10 (std::norm (bins[k]) + 1.0e-30f);

        return db;
    }

    /**
        The alias detector. A wavetable at a static Frame is exactly periodic at
        the played frequency whatever the source material, so every legitimate
        line sits on the harmonic grid m*f; anything off-grid is aliasing or
        interpolation imaging. Returns (worst off-grid dB) - (peak harmonic dB):
        more negative is cleaner.
    */
    inline float worstOffGridDb (const std::vector<float>& spectrumDb,
                                 double playedHz, double sampleRate,
                                 int guardBins = 3)
    {
        const auto fftSize = (int) spectrumDb.size() * 2;
        const auto binHz = sampleRate / (double) fftSize;

        std::vector<bool> onGrid (spectrumDb.size(), false);

        // DC/near-DC leakage is not aliasing; exclude the first few bins too.
        for (int k = 0; k <= guardBins && k < (int) spectrumDb.size(); ++k)
            onGrid[(size_t) k] = true;

        for (int m = 1; m * playedHz < sampleRate * 0.5; ++m)
        {
            const auto centre = (int) std::round ((double) m * playedHz / binHz);

            for (int k = centre - guardBins; k <= centre + guardBins; ++k)
                if (k >= 0 && k < (int) spectrumDb.size())
                    onGrid[(size_t) k] = true;
        }

        float peakHarmonic = -300.0f, worstOffGrid = -300.0f;

        for (size_t k = 0; k < spectrumDb.size(); ++k)
        {
            if (onGrid[k])
                peakHarmonic = juce::jmax (peakHarmonic, spectrumDb[k]);
            else
                worstOffGrid = juce::jmax (worstOffGrid, spectrumDb[k]);
        }

        return worstOffGrid - peakHarmonic;
    }
}
