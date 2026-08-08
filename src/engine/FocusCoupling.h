#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace keepsake::coupling
{
    /**
        The Focus coupling curves (spec §2.4): "not just a crossfade" - Focus also
        scales engine parameters so the morph feels designed. All pure functions
        of the post-modulation focus value f in 0..1.

        Two properties are load-bearing and asserted by unit tests - do not tune
        one side of either without the other:

        1. densityMultiplier(f) * grainSizeMultiplier(f) == 1 for all f.
           Grain overlap = length x density, and Cloud's output level rides
           1/sqrt(overlap); the reciprocal pair keeps overlap - and therefore
           level - constant across the whole sweep. Texture changes, level does
           not. That invariant is most of what makes the knob feel like an
           instrument instead of a mixer.

        2. Every curve is the identity at f == 0 (multipliers exactly 1, tone
           gain exactly 0), which is what keeps a focus-0 render bit-identical
           to the pre-Tone engine (a regression test asserts it).
    */

    /** Cloud "condenses" toward Tone: x1 -> x2. */
    inline float densityMultiplier (float f) noexcept { return std::exp2 (f); }

    /** ...while grains shrink: x1 -> x0.5 (reciprocal of density, see above). */
    inline float grainSizeMultiplier (float f) noexcept { return std::exp2 (-f); }

    /** Shimmer stays lively at mid-focus, cleans up fully at Tone: x1 -> x0. */
    inline float shimmerMultiplier (float f) noexcept { return 1.0f - f * f; }

    /** Tone ducks earlier than linear toward Cloud (spec): ~-2.5dB below
        equal-power at f=0.5, exactly 0 at f=0, exactly 1 at f=1. */
    inline float toneGain (float f) noexcept
    {
        return std::sin (juce::MathConstants<float>::halfPi * std::pow (f, 1.5f));
    }

    /** Engine loudness match, applied to Tone at the blend (NOT part of
        toneGain - the curve's 0..1 endpoints are load-bearing). Hann-windowed
        grains carry sqrt(3/8) ~ 0.61x the RMS of a raw wavetable playing the
        same material, so without this trim a Focus sweep tilts ~+4dB toward
        Tone - an equal-power blend of unequal signals always pumps. The RMS
        corridor test owns this constant. */
    inline constexpr float kToneEngineTrim = 0.612f;

    /** Unchanged from the M3 equal-power blend. */
    inline float cloudGain (float f) noexcept
    {
        return std::cos (juce::MathConstants<float>::halfPi * f);
    }

    /** A small detune spreads Tone toward Cloud: 7ct at f=0, 0 at f=1.
        Sign-alternated by voice index so chords spread symmetrically; a single
        note is just d cents off, inaudible at <=7ct. (A true 2-osc unison is the
        richer reading of "spread" - deferred, this ships.) */
    inline float toneDetuneCents (float f, int voiceIndex) noexcept
    {
        const auto d = 7.0f * (1.0f - f);
        return (voiceIndex & 1) != 0 ? -d : d;
    }
}
