#include "PluginEditor.h"

namespace keepsake
{
    // =========================================================================
    // Content - laid out once, at design size
    // =========================================================================

    KeepsakeEditor::Content::Content (KeepsakeProcessor& p)
        : waveform (p), capture (p)
    {
        addAndMakeVisible (waveform);
        addAndMakeVisible (capture);

        auto& state = p.getState();

        auto knob = [&state] (const char* id, const char* name, const char* tip)
        {
            return std::make_unique<ParamKnob> (state, id, name, tip);
        };

        // --- Keepsake (capture) ---------------------------------------------
        rootPanel.setColumns (3);
        rootPanel.addKnob (knob (params::place, "Place",
                                 "Place - capture position through the source file. Automatable; "
                                 "small mod depths give subtle movement, large sweeps scan the whole file."));
        rootPanel.addKnob (knob (params::captureLength, "Keep",
                                 "Keep Length - length of the captured window, 10-500 ms."));
        rootPanel.addKnob (knob (params::rootNote, "Root",
                                 "Root - the pitch the source material is assumed to be. "
                                 "Everything tracks from here."));
        rootPanel.addKnob (knob (params::rootCents, "Fine",
                                 "Root Fine - root offset in cents."));
        addAndMakeVisible (rootPanel);

        // --- Cloud ----------------------------------------------------------
        cloudPanel.setColumns (3);
        cloudPanel.addKnob (knob (params::grainSize, "Size",
                                  "Grain Size - 5-250 ms, clamped to the kept window."));
        cloudPanel.addKnob (knob (params::grainDensity, "Density",
                                  "Density - grains per second."));
        cloudPanel.addKnob (knob (params::grainDrift, "Drift",
                                  "Position jitter - randomises each grain's start within the kept window."));
        cloudPanel.addKnob (knob (params::grainShimmer, "Shimmer",
                                  "Pitch jitter - per-grain random detune in cents; "
                                  "high settings also add octave-up grains."));
        cloudPanel.addKnob (knob (params::grainWindow, "Window",
                                  "Grain window shape - Hann -> Tukey -> Expodec."));
        cloudPanel.addKnob (knob (params::grainSpread, "Spread",
                                  "Stereo spread - per-grain pan randomisation."));
        addAndMakeVisible (cloudPanel);

        // --- Tone -----------------------------------------------------------
        tonePanel.setColumns (4);
        tonePanel.addKnob (knob (params::focus, "Focus",
                                 "Focus - morphs between the Cloud (granular) and Tone "
                                 "(wavetable) engines. Equal-power blend in this version; "
                                 "engine coupling arrives in M4."));
        tonePanel.addKnob (knob (params::toneFrame, "Frame",
                                 "Frame - scans across the wavetable frames extracted from "
                                 "the kept moment."));
        tonePanel.addKnob (knob (params::toneFrames, "Frames",
                                 "Frames - how many cycles are extracted across the kept "
                                 "window: 2/4/8/16. Changing it re-extracts."));
        tonePanel.addKnob (knob (params::toneFrameWrap, "Wrap",
                                 "Frame Wrap - Loop or Ping-Pong when Frame is scanned or "
                                 "modulated."));
        addAndMakeVisible (tonePanel);

        // --- Amp envelope ---------------------------------------------------
        ampPanel.setColumns (4);
        ampPanel.addKnob (knob (params::ampAttack, "A", "ENV1 Attack (hardwired to amp)."));
        ampPanel.addKnob (knob (params::ampDecay, "D", "ENV1 Decay."));
        ampPanel.addKnob (knob (params::ampSustain, "S", "ENV1 Sustain."));
        ampPanel.addKnob (knob (params::ampRelease, "R", "ENV1 Release."));
        addAndMakeVisible (ampPanel);

        // --- Output ---------------------------------------------------------
        outputPanel.setColumns (1);
        outputPanel.addKnob (knob (params::masterGain, "Level",
                                   "Master output level. Excluded from Randomize."));
        addAndMakeVisible (outputPanel);

        // Honest about what is not built yet, rather than showing dead controls.
        placeholder.setText ("Filter, LFOs, mod matrix and FX arrive in M4-M5.",
                             juce::dontSendNotification);
        placeholder.setJustificationType (juce::Justification::centred);
        placeholder.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.30f));
        placeholder.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (placeholder);
    }

    void KeepsakeEditor::Content::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour (0xff1c1f24));
    }

    void KeepsakeEditor::Content::resized()
    {
        auto r = getLocalBounds();

        // Top half: the waveform is the hero (spec §3).
        waveform.setBounds (r.removeFromTop (r.getHeight() / 2).reduced (8, 8));
        capture.setBounds (r.removeFromTop (32).reduced (8, 0));

        r.reduce (8, 8);

        // Bottom half, left: capture + Cloud. Right: envelope, output, and the space
        // that Focus/Tone will occupy from M3.
        auto left = r.removeFromLeft ((int) (r.getWidth() * 0.55));
        r.removeFromLeft (8);

        rootPanel.setBounds (left.removeFromTop (left.getHeight() / 2).reduced (0, 0));
        left.removeFromTop (8);
        cloudPanel.setBounds (left);

        // Right column: Tone on top (Focus lives with it - controls sit on the
        // side they affect, and Tone is the right-hand engine per spec layout),
        // then the envelope, then output + the remaining-milestones note.
        tonePanel.setBounds (r.removeFromTop ((int) (r.getHeight() * 0.45)));
        r.removeFromTop (8);
        ampPanel.setBounds (r.removeFromTop ((int) (r.getHeight() * 0.55)));
        r.removeFromTop (8);
        outputPanel.setBounds (r.removeFromLeft (r.getWidth() / 3));
        r.removeFromLeft (8);
        placeholder.setBounds (r);
    }

    // =========================================================================
    // Editor
    // =========================================================================

    KeepsakeEditor::KeepsakeEditor (KeepsakeProcessor& p)
        : juce::AudioProcessorEditor (&p), proc (p), content (p)
    {
        addAndMakeVisible (content);
        content.setSize (kDesignWidth, kDesignHeight);

        // Resizable from v1, fixed aspect ratio (spec §3 / §8).
        setResizable (true, true);

        if (auto* bounds = getConstrainer())
        {
            bounds->setFixedAspectRatio ((double) kDesignWidth / (double) kDesignHeight);
            bounds->setSizeLimits (kDesignWidth * 2 / 3, kDesignHeight * 2 / 3,
                                        kDesignWidth * 2, kDesignHeight * 2);
        }

        setSize (kDesignWidth, kDesignHeight);
    }

    KeepsakeEditor::~KeepsakeEditor() = default;

    void KeepsakeEditor::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour (0xff1c1f24));

        if (fileDragActive)
        {
            g.setColour (juce::Colour (0xffe0a458));
            g.drawRect (getLocalBounds(), 3);
        }
    }

    void KeepsakeEditor::resized()
    {
        // One transform scales the whole design-size layout to the current window.
        const auto scale = (float) getWidth() / (float) kDesignWidth;

        content.setTransform (juce::AffineTransform::scale (scale));
        content.setBounds (0, 0, kDesignWidth, kDesignHeight);
    }

    // =========================================================================
    // Drag and drop
    // =========================================================================

    bool KeepsakeEditor::isInterestedInFileDrag (const juce::StringArray& files)
    {
        for (const auto& f : files)
            if (juce::File (f).hasFileExtension ("wav;aiff;aif;flac;mp3;ogg"))
                return true;

        return false;
    }

    void KeepsakeEditor::fileDragEnter (const juce::StringArray&, int, int)
    {
        fileDragActive = true;
        repaint();
    }

    void KeepsakeEditor::fileDragExit (const juce::StringArray&)
    {
        fileDragActive = false;
        repaint();
    }

    void KeepsakeEditor::filesDropped (const juce::StringArray& files, int, int)
    {
        fileDragActive = false;
        repaint();

        for (const auto& f : files)
        {
            const juce::File file (f);

            if (! file.hasFileExtension ("wav;aiff;aif;flac;mp3;ogg"))
                continue;

            const auto result = proc.importFile (file);
            content.capture.setStatus (result.message);
            break; // one keepsake per preset in v1 (spec §8)
        }
    }
}
