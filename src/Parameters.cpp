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
        ampAttack     = state.getRawParameterValue (params::ampAttack);
        ampDecay      = state.getRawParameterValue (params::ampDecay);
        ampSustain    = state.getRawParameterValue (params::ampSustain);
        ampRelease    = state.getRawParameterValue (params::ampRelease);
        masterGain    = state.getRawParameterValue (params::masterGain);
    }
}
