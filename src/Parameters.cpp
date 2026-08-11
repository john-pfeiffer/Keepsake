#include "Parameters.h"

namespace keepsake::params
{
    namespace
    {
        using Range = juce::NormalisableRange<float>;

        /** value-to-text that reads cleanly in a DAW automation lane. */
        std::function<juce::String (float, int)> fmt (const char* unit, int decimals)
        {
            juce::String u (unit);
            return [u, decimals] (float v, int) { return juce::String (v, decimals) + u; };
        }

        juce::String noteToText (float v, int)
        {
            return juce::MidiMessage::getMidiNoteName (juce::roundToInt (v), true, true, 3);
        }

        float textToNote (const juce::String& text)
        {
            // Accept either a raw number ("60") or a note name ("C3", "F#2").
            const auto trimmed = text.trim();

            if (trimmed.containsOnly ("0123456789"))
                return trimmed.getFloatValue();

            static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

            for (int semitone = 11; semitone >= 0; --semitone)
            {
                const juce::String name (names[semitone]);

                if (trimmed.startsWithIgnoreCase (name))
                {
                    const auto octave = trimmed.substring (name.length()).getIntValue();
                    return (float) juce::jlimit (0, 127, (octave + 2) * 12 + semitone);
                }
            }

            return 60.0f;
        }

        /** Log-ish taper: a skew that puts the given value at the centre of the knob. */
        Range skewed (float lo, float hi, float centre, float interval = 0.0f)
        {
            Range r (lo, hi, interval);
            r.setSkewForCentre (centre);
            return r;
        }

        std::unique_ptr<juce::AudioParameterFloat> makeFloat (juce::String id,
                                                              juce::String name,
                                                              Range range,
                                                              float defaultValue,
                                                              const char* unit,
                                                              int decimals)
        {
            return std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { id, 1 },
                name,
                range,
                defaultValue,
                juce::AudioParameterFloatAttributes()
                    .withLabel (juce::String (unit).trim())
                    .withStringFromValueFunction (fmt (unit, decimals)));
        }
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        // --- Capture --------------------------------------------------------
        // Place is normalised (0..1) rather than seconds so that automation written
        // against one source file still means "the same relative moment" if the file
        // is swapped. Displayed as a percentage through the file.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { place, 1 },
            "Place",
            Range (0.0f, 1.0f),
            0.0f,
            juce::AudioParameterFloatAttributes()
                .withLabel ("%")
                .withStringFromValueFunction ([] (float v, int)
                                              { return juce::String (v * 100.0f, 1) + " %"; })
                .withValueFromStringFunction ([] (const juce::String& t)
                                              { return t.getFloatValue() * 0.01f; })));
        layout.add (makeFloat (captureLength, "Keep Length",
                               skewed (10.0f, 500.0f, 100.0f), 120.0f, " ms", 1));

        // --- Root -----------------------------------------------------------
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { rootNote, 1 },
            "Root",
            Range (0.0f, 127.0f, 1.0f),
            60.0f, // C3 with middle-C-is-octave-3 naming (Ableton/Logic convention)
            juce::AudioParameterFloatAttributes()
                .withStringFromValueFunction (noteToText)
                .withValueFromStringFunction (textToNote)));

        layout.add (makeFloat (rootCents, "Root Fine", Range (-50.0f, 50.0f), 0.0f, " ct", 1));

        // --- Cloud ----------------------------------------------------------
        layout.add (makeFloat (grainSize, "Grain Size",
                               skewed (5.0f, 250.0f, 60.0f), 60.0f, " ms", 1));
        layout.add (makeFloat (grainDensity, "Density",
                               skewed (2.0f, 200.0f, 24.0f), 24.0f, " /s", 1));
        layout.add (makeFloat (grainDrift, "Drift", Range (0.0f, 100.0f), 15.0f, " %", 1));
        layout.add (makeFloat (grainShimmer, "Shimmer", Range (0.0f, 100.0f), 0.0f, " ct", 1));
        layout.add (makeFloat (grainWindow, "Window", Range (0.0f, 1.0f), 0.0f, "", 2));
        layout.add (makeFloat (grainSpread, "Spread", Range (0.0f, 100.0f), 40.0f, " %", 1));

        // Tempo-synced grain emission: grains pulse at a musical division,
        // anchored to note-on (the division list is the LFOs' frozen 12-entry
        // list - shared ABI, shared math).
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { grainSync, 1 },
            "Grain Sync",
            juce::StringArray { "Free", "Sync" },
            0));
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { grainDivision, 1 },
            "Grain Division",
            juce::StringArray { "1/1", "1/2", "1/2T", "1/4.", "1/4", "1/4T",
                                "1/8.", "1/8", "1/8T", "1/16", "1/16T", "1/32" },
            7)); // 1/8

        // --- Tone -----------------------------------------------------------
        // Focus defaults to full Cloud so the plugin sounds exactly like M2 out of
        // the box. In M3 this is a plain equal-power blend; M4 adds the coupling.
        layout.add (makeFloat (focus, "Focus", Range (0.0f, 1.0f), 0.0f, "", 2));
        layout.add (makeFloat (toneFrame, "Frame", Range (0.0f, 1.0f), 0.0f, "", 2));

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { toneFrames, 1 },
            "Frames",
            juce::StringArray { "2", "4", "8", "16" },
            2)); // default 8

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { toneFrameWrap, 1 },
            "Frame Wrap",
            juce::StringArray { "Loop", "Ping-Pong" },
            0));

        // --- Filter ---------------------------------------------------------
        // NOTE (frozen ABI): every AudioParameterChoice list in this plugin is
        // final once shipped - host automation stores index/(numChoices-1), so
        // changing a list's length silently remaps recorded automation.
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { filterType, 1 },
            "Filter Type",
            juce::StringArray { "Low-pass", "Band-pass", "High-pass" },
            0));
        layout.add (makeFloat (filterCutoff, "Cutoff",
                               skewed (20.0f, 20000.0f, 640.0f), 20000.0f, " Hz", 0));
        layout.add (makeFloat (filterResonance, "Resonance",
                               skewed (0.5f, 10.0f, 1.5f), 0.707f, "", 2));
        layout.add (makeFloat (filterKeytrack, "Keytrack", Range (0.0f, 100.0f), 0.0f, " %", 0));

        // --- ENV2 -----------------------------------------------------------
        layout.add (makeFloat (env2Attack, "Env2 Attack",
                               skewed (1.0f, 5000.0f, 100.0f), 20.0f, " ms", 1));
        layout.add (makeFloat (env2Decay, "Env2 Decay",
                               skewed (1.0f, 5000.0f, 300.0f), 400.0f, " ms", 1));
        layout.add (makeFloat (env2Sustain, "Env2 Sustain", Range (0.0f, 1.0f), 0.8f, "", 2));
        layout.add (makeFloat (env2Release, "Env2 Release",
                               skewed (1.0f, 10000.0f, 500.0f), 600.0f, " ms", 1));

        // --- LFOs (all lists frozen ABI) ------------------------------------
        const juce::StringArray lfoShapes { "Sine", "Triangle", "Saw", "S&H" };
        const juce::StringArray lfoDivisions { "1/1", "1/2", "1/2T", "1/4.", "1/4", "1/4T",
                                               "1/8.", "1/8", "1/8T", "1/16", "1/16T", "1/32" };

        auto addLfo = [&] (const char* shapeId, const char* rateId, const char* syncId,
                           const char* divId, const char* retrigId, const char* prefix)
        {
            const juce::String name (prefix);
            layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { shapeId, 1 }, name + " Shape", lfoShapes, 0));
            layout.add (makeFloat (rateId, name + " Rate",
                                   skewed (0.02f, 20.0f, 2.0f), 1.0f, " Hz", 2));
            layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { syncId, 1 }, name + " Sync",
                juce::StringArray { "Free", "Sync" }, 0));
            layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { divId, 1 }, name + " Division", lfoDivisions, 4));
            layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { retrigId, 1 }, name + " Retrig",
                juce::StringArray { "Off", "On" }, 1));
        };

        addLfo (lfo1Shape, lfo1Rate, lfo1Sync, lfo1Division, lfo1Retrig, "LFO1");
        addLfo (lfo2Shape, lfo2Rate, lfo2Sync, lfo2Division, lfo2Retrig, "LFO2");

        // --- Mod matrix (6 slots; both choice lists frozen ABI) --------------
        const juce::StringArray modSources { "None", "Env2", "LFO1", "LFO2",
                                             "Velocity", "Mod Wheel", "Aftertouch" };
        const juce::StringArray modDests { "None", "Focus", "Place", "Cutoff",
                                           "Grain Size", "Density", "Drift", "Shimmer",
                                           "Frame", "Pitch", "Spread", "Reverb Mix" };

        for (int slot = 0; slot < 6; ++slot)
        {
            const auto n = juce::String (slot + 1);
            layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { slotId ("Source", slot), 1 },
                "Mod " + n + " Source", modSources, 0));
            layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { slotId ("Dest", slot), 1 },
                "Mod " + n + " Dest", modDests, 0));
            layout.add (makeFloat (slotId ("Depth", slot), "Mod " + n + " Amount",
                                   Range (-100.0f, 100.0f), 0.0f, " %", 0));
        }

        // --- Voice architecture ---------------------------------------------
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { voiceMode, 1 },
            "Voice Mode",
            juce::StringArray { "Poly", "Mono", "Legato" },
            0));
        layout.add (makeFloat (glideTime, "Glide",
                               skewed (0.0f, 2000.0f, 100.0f), 50.0f, " ms", 0));

        // --- FX chain (spec §2.6) -------------------------------------------
        // All except Air Size default to 0 (fully dry): the bit-identity
        // regression tests (block-size independence, focus-0-equals-M2) render
        // at defaults, and a dry default keeps the whole FX section on its
        // exact-bypass path there.
        layout.add (makeFloat (warmthAmount, "Warmth", Range (0.0f, 100.0f), 0.0f, " %", 0));
        layout.add (makeFloat (chorusAmount, "Chorus", Range (0.0f, 100.0f), 0.0f, " %", 0));
        layout.add (makeFloat (airSize, "Air Size", Range (0.0f, 100.0f), 50.0f, " %", 0));
        layout.add (makeFloat (airMix, "Air Mix", Range (0.0f, 100.0f), 0.0f, " %", 0));

        // --- Amp envelope ---------------------------------------------------
        layout.add (makeFloat (ampAttack, "Attack",
                               skewed (1.0f, 5000.0f, 100.0f), 20.0f, " ms", 1));
        layout.add (makeFloat (ampDecay, "Decay",
                               skewed (1.0f, 5000.0f, 300.0f), 400.0f, " ms", 1));
        layout.add (makeFloat (ampSustain, "Sustain", Range (0.0f, 1.0f), 0.8f, "", 2));
        layout.add (makeFloat (ampRelease, "Release",
                               skewed (1.0f, 10000.0f, 500.0f), 600.0f, " ms", 1));

        // --- Output ---------------------------------------------------------
        layout.add (makeFloat (masterGain, "Output",
                               Range (-60.0f, 6.0f), -6.0f, " dB", 1));

        return layout;
    }

    void Handles::attach (juce::AudioProcessorValueTreeState& state)
    {
        place         = state.getRawParameterValue (params::place);
        captureLength = state.getRawParameterValue (params::captureLength);
        rootNote      = state.getRawParameterValue (params::rootNote);
        rootCents     = state.getRawParameterValue (params::rootCents);
        grainSize     = state.getRawParameterValue (params::grainSize);
        grainDensity  = state.getRawParameterValue (params::grainDensity);
        grainDrift    = state.getRawParameterValue (params::grainDrift);
        grainShimmer  = state.getRawParameterValue (params::grainShimmer);
        grainWindow   = state.getRawParameterValue (params::grainWindow);
        grainSpread   = state.getRawParameterValue (params::grainSpread);
        grainSync     = state.getRawParameterValue (params::grainSync);
        grainDivision = state.getRawParameterValue (params::grainDivision);
        focus         = state.getRawParameterValue (params::focus);
        toneFrame     = state.getRawParameterValue (params::toneFrame);
        toneFrames    = state.getRawParameterValue (params::toneFrames);
        toneFrameWrap = state.getRawParameterValue (params::toneFrameWrap);
        filterType      = state.getRawParameterValue (params::filterType);
        filterCutoff    = state.getRawParameterValue (params::filterCutoff);
        filterResonance = state.getRawParameterValue (params::filterResonance);
        filterKeytrack  = state.getRawParameterValue (params::filterKeytrack);
        env2Attack  = state.getRawParameterValue (params::env2Attack);
        env2Decay   = state.getRawParameterValue (params::env2Decay);
        env2Sustain = state.getRawParameterValue (params::env2Sustain);
        env2Release = state.getRawParameterValue (params::env2Release);

        const char* lfoIds[2][5] = {
            { params::lfo1Shape, params::lfo1Rate, params::lfo1Sync, params::lfo1Division, params::lfo1Retrig },
            { params::lfo2Shape, params::lfo2Rate, params::lfo2Sync, params::lfo2Division, params::lfo2Retrig },
        };

        for (int i = 0; i < 2; ++i)
        {
            lfoShape[i]    = state.getRawParameterValue (lfoIds[i][0]);
            lfoRate[i]     = state.getRawParameterValue (lfoIds[i][1]);
            lfoSync[i]     = state.getRawParameterValue (lfoIds[i][2]);
            lfoDivision[i] = state.getRawParameterValue (lfoIds[i][3]);
            lfoRetrig[i]   = state.getRawParameterValue (lfoIds[i][4]);
        }

        voiceMode = state.getRawParameterValue (params::voiceMode);
        glideTime = state.getRawParameterValue (params::glideTime);

        for (int slot = 0; slot < 6; ++slot)
        {
            modSource[slot] = state.getRawParameterValue (params::slotId ("Source", slot));
            modDest[slot]   = state.getRawParameterValue (params::slotId ("Dest", slot));
            modDepth[slot]  = state.getRawParameterValue (params::slotId ("Depth", slot));
        }

        // Destination ranges for the normalized-domain combine. Indices follow
        // mod::Dest; None/Pitch/ReverbMix have no underlying parameter.
        auto rangeOf = [&state] (const char* id) -> const juce::NormalisableRange<float>*
        {
            if (auto* p = state.getParameter (id))
                return &p->getNormalisableRange();

            jassertfalse;
            return nullptr;
        };

        destRange[1]  = rangeOf (params::focus);
        destRange[2]  = rangeOf (params::place);
        destRange[3]  = rangeOf (params::filterCutoff);
        destRange[4]  = rangeOf (params::grainSize);
        destRange[5]  = rangeOf (params::grainDensity);
        destRange[6]  = rangeOf (params::grainDrift);
        destRange[7]  = rangeOf (params::grainShimmer);
        destRange[8]  = rangeOf (params::toneFrame);
        destRange[10] = rangeOf (params::grainSpread);
        warmthAmount  = state.getRawParameterValue (params::warmthAmount);
        chorusAmount  = state.getRawParameterValue (params::chorusAmount);
        airSize       = state.getRawParameterValue (params::airSize);
        airMix        = state.getRawParameterValue (params::airMix);
        ampAttack     = state.getRawParameterValue (params::ampAttack);
        ampDecay      = state.getRawParameterValue (params::ampDecay);
        ampSustain    = state.getRawParameterValue (params::ampSustain);
        ampRelease    = state.getRawParameterValue (params::ampRelease);
        masterGain    = state.getRawParameterValue (params::masterGain);
    }
}
