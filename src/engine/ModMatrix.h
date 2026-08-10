#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace keepsake
{
    /**
        The per-voice modulation snapshot: one plain value per destination,
        evaluated at every control tick (Voice::kControlTickSamples, anchored to
        the voice-local sample clock - never to host block boundaries, which is
        what keeps renders bit-identical across block sizes).

        Defaults are the identity: a default snapshot modulates nothing, which is
        exactly what step 1 of the M4 build ships. The ModMatrix (step 4) fills
        these from the slot parameters.

        Values are offsets in the destination parameter's NORMALIZED domain
        (combine rule: convertTo0to1(base) + offset, clamp, convertFrom0to1),
        except pitchSemitones (additive semitones - Pitch has no underlying
        parameter) and reverbSend (an absolute per-voice wet-send level 0..1,
        the contract M5's FX chain implements).
    */
    /** Frozen ABI lists (host automation stores index/(N-1); see Parameters.cpp). */
    namespace mod
    {
        enum Source { srcNone = 0, srcEnv2, srcLfo1, srcLfo2, srcVelocity, srcModWheel, srcAftertouch };
        enum Dest { destNone = 0, destFocus, destPlace, destCutoff, destGrainSize,
                    destDensity, destDrift, destShimmer, destFrame, destPitch,
                    destSpread, destReverbMix };
        inline constexpr int kNumSlots = 6;
    }

    struct ModSnapshot
    {
        float focus = 0.0f;
        float place = 0.0f;
        float cutoff = 0.0f;
        float grainSize = 0.0f;
        float density = 0.0f;
        float drift = 0.0f;
        float shimmer = 0.0f;
        float frame = 0.0f;
        float spread = 0.0f;
        float pitchSemitones = 0.0f;
        float reverbSend = 0.0f;
    };

    /**
        Turns the six slot parameters plus the per-voice source values into a
        snapshot of normalized-domain offsets. Pure; called per control tick.

        Source polarity: LFOs are bipolar -1..+1; ENV2, Velocity, ModWheel and
        Aftertouch are unipolar 0..1. Depth is -1..+1 (the -100..+100% param).
        Pitch accumulates in semitones (full depth = +-12 st per the plan);
        Reverb Mix accumulates into an absolute wet-send level (base 0).
    */
    struct ModSources
    {
        float env2 = 0.0f;
        float lfo1 = 0.0f;
        float lfo2 = 0.0f;
        float velocity = 0.0f;
        float modWheel = 0.0f;
        float aftertouch = 0.0f;
    };

    struct ModSlotValues
    {
        int source = 0;
        int dest = 0;
        float depth = 0.0f; // -1..+1
    };

    inline ModSnapshot evaluateModMatrix (const ModSlotValues (&slots)[mod::kNumSlots],
                                          const ModSources& sources) noexcept
    {
        ModSnapshot out;

        for (const auto& slot : slots)
        {
            // exactlyEqual: the zero test is intentionally exact - it gates the
            // bit-identity fast paths, not a tolerance decision.
            if (slot.dest == mod::destNone || slot.source == mod::srcNone
                || juce::exactlyEqual (slot.depth, 0.0f))
                continue;

            float value = 0.0f;

            switch (slot.source)
            {
                case mod::srcEnv2:       value = sources.env2; break;
                case mod::srcLfo1:       value = sources.lfo1; break;
                case mod::srcLfo2:       value = sources.lfo2; break;
                case mod::srcVelocity:   value = sources.velocity; break;
                case mod::srcModWheel:   value = sources.modWheel; break;
                case mod::srcAftertouch: value = sources.aftertouch; break;
                default: break;
            }

            const auto amount = slot.depth * value;

            switch (slot.dest)
            {
                case mod::destFocus:     out.focus += amount; break;
                case mod::destPlace:     out.place += amount; break;
                case mod::destCutoff:    out.cutoff += amount; break;
                case mod::destGrainSize: out.grainSize += amount; break;
                case mod::destDensity:   out.density += amount; break;
                case mod::destDrift:     out.drift += amount; break;
                case mod::destShimmer:   out.shimmer += amount; break;
                case mod::destFrame:     out.frame += amount; break;
                case mod::destPitch:     out.pitchSemitones += amount * 12.0f; break;
                case mod::destSpread:    out.spread += amount; break;
                case mod::destReverbMix: out.reverbSend += amount; break;
                default: break;
            }
        }

        out.reverbSend = juce::jlimit (0.0f, 1.0f, out.reverbSend);
        return out;
    }

    /** The normalized-domain combine (the M4 binding rule). offset==0 returns
        base untouched - the skewed convertTo/From round trip is not bit-exact,
        and the static path must stay bit-identical. */
    inline float applyNormalizedMod (const juce::NormalisableRange<float>& range,
                                     float base, float offset) noexcept
    {
        if (juce::exactlyEqual (offset, 0.0f)) // intentional: gates the bit-identical path
            return base;

        const auto n = juce::jlimit (0.0f, 1.0f, range.convertTo0to1 (base) + offset);
        return range.convertFrom0to1 (n);
    }
}
