#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace keepsake
{
    /**
        One per-voice LFO, sampled at control-tick rate (spec §2.5).

        Bipolar output -1..+1. Advanced by whole ticks on the voice-local clock,
        so its value sequence is identical at any host block size. Retrig=On
        resets phase at noteOn (the default - matches the codebase's determinism
        idiom); Retrig=Off lets phase persist across notes on this voice.

        S&H is seeded at construction from the voice index (same precedent as
        GrainEngine's per-voice RNG streams) and draws a new value on each phase
        wrap - deterministic for a given seed and rate sequence.
    */
    class LFO
    {
    public:
        enum Shape { sine = 0, triangle = 1, saw = 2, sampleAndHold = 3 };

        explicit LFO (juce::int64 seed) : constructionSeed (seed), rng (seed)
        {
            heldValue = rng.nextFloat() * 2.0f - 1.0f;
        }

        void noteOn (bool retrigger) noexcept
        {
            if (retrigger)
            {
                phase = 0.0;
                // A retriggered S&H starts each note with the same first value.
                rng.setSeed (constructionSeed);
                heldValue = rng.nextFloat() * 2.0f - 1.0f;
            }
        }

        /** Advances by numSamples at rateHz and returns the value at the NEW
            position. Call once per control tick. */
        float advance (int shape, double rateHz, int numSamples, double sampleRate) noexcept
        {
            const auto increment = rateHz * (double) numSamples / juce::jmax (1.0, sampleRate);
            phase += increment;

            if (phase >= 1.0)
            {
                phase -= std::floor (phase);

                if (shape == sampleAndHold)
                    heldValue = rng.nextFloat() * 2.0f - 1.0f;
            }

            switch (shape)
            {
                case sine:
                    return std::sin ((float) phase * juce::MathConstants<float>::twoPi);

                case triangle:
                    return phase < 0.5 ? (float) (4.0 * phase - 1.0)
                                       : (float) (3.0 - 4.0 * phase);

                case saw: // rising, -1 at phase 0
                    return (float) (2.0 * phase - 1.0);

                case sampleAndHold:
                default:
                    return heldValue;
            }
        }

        /** Beats per cycle for the frozen 12-entry division list (see
            Parameters.cpp; the list is host-automation ABI). Quarter note = 1. */
        static double beatsForDivision (int divisionIndex) noexcept
        {
            constexpr double beats[] = { 4.0,       // 1/1
                                         2.0,       // 1/2
                                         4.0 / 3.0, // 1/2T
                                         1.5,       // 1/4.
                                         1.0,       // 1/4
                                         2.0 / 3.0, // 1/4T
                                         0.75,      // 1/8.
                                         0.5,       // 1/8
                                         1.0 / 3.0, // 1/8T
                                         0.25,      // 1/16
                                         1.0 / 6.0, // 1/16T
                                         0.125 };   // 1/32
            return beats[juce::jlimit (0, 11, divisionIndex)];
        }

        static double syncedRateHz (double bpm, int divisionIndex) noexcept
        {
            return bpm / (60.0 * beatsForDivision (divisionIndex));
        }

    private:
        const juce::int64 constructionSeed;
        juce::Random rng;
        double phase = 0.0;
        float heldValue = 0.0f;
    };
}
