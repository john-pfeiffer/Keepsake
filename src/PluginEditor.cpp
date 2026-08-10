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

        // --- Amp env + output ------------------------------------------------
        ampPanel.setColumns (5);
        ampPanel.addKnob (knob (params::ampAttack, "A", "ENV1 Attack (hardwired to amp)."));
        ampPanel.addKnob (knob (params::ampDecay, "D", "ENV1 Decay."));
        ampPanel.addKnob (knob (params::ampSustain, "S", "ENV1 Sustain."));
        ampPanel.addKnob (knob (params::ampRelease, "R", "ENV1 Release."));
        ampPanel.addKnob (knob (params::masterGain, "Level",
                                "Master output level. Excluded from Randomize."));

        // --- Filter ----------------------------------------------------------
        filterPanel.setColumns (4);
        filterPanel.addKnob (knob (params::filterType, "Type", "Filter type: LP / BP / HP."));
        filterPanel.addKnob (knob (params::filterCutoff, "Cutoff",
                                   "Filter cutoff. Keytrack and modulation apply on top."));
        filterPanel.addKnob (knob (params::filterResonance, "Res", "Filter resonance (Q)."));
        filterPanel.addKnob (knob (params::filterKeytrack, "Keytrk",
                                   "Keytrack - how much cutoff follows the played note (C3 reference)."));

        // --- ENV2 + voice ----------------------------------------------------
        env2Panel.setColumns (4);
        env2Panel.addKnob (knob (params::env2Attack, "A", "ENV2 Attack (assignable envelope)."));
        env2Panel.addKnob (knob (params::env2Decay, "D", "ENV2 Decay."));
        env2Panel.addKnob (knob (params::env2Sustain, "S", "ENV2 Sustain."));
        env2Panel.addKnob (knob (params::env2Release, "R", "ENV2 Release."));

        voicePanel.setColumns (2);
        voicePanel.addKnob (knob (params::voiceMode, "Mode",
                                  "Voice mode: Poly, Mono (retriggers), Legato (doesn't)."));
        voicePanel.addKnob (knob (params::glideTime, "Glide",
                                  "Glide time between notes in Mono/Legato, constant-time."));

        // --- LFOs ------------------------------------------------------------
        lfoPanel.setColumns (5);

        for (auto ids : { std::array<const char*, 5> { params::lfo1Shape, params::lfo1Rate,
                                                       params::lfo1Sync, params::lfo1Division,
                                                       params::lfo1Retrig },
                          std::array<const char*, 5> { params::lfo2Shape, params::lfo2Rate,
                                                       params::lfo2Sync, params::lfo2Division,
                                                       params::lfo2Retrig } })
        {
            lfoPanel.addKnob (knob (ids[0], "Shape", "LFO shape: Sine / Triangle / Saw / S&H."));
            lfoPanel.addKnob (knob (ids[1], "Rate", "LFO rate in Hz (Free mode)."));
            lfoPanel.addKnob (knob (ids[2], "Sync", "Free-running Hz or tempo-synced."));
            lfoPanel.addKnob (knob (ids[3], "Div", "Tempo division when synced."));
            lfoPanel.addKnob (knob (ids[4], "Retrig",
                                    "On: phase restarts at every note. Off: free-running."));
        }

        // --- Mod matrix ------------------------------------------------------
        modPanel.setColumns (6);

        for (int slot = 0; slot < 6; ++slot)
        {
            const auto n = juce::String (slot + 1);
            modPanel.addKnob (std::make_unique<ParamKnob> (state, params::slotId ("Source", slot),
                                                           "Src " + n, "Mod slot " + n + " source."));
            modPanel.addKnob (std::make_unique<ParamKnob> (state, params::slotId ("Dest", slot),
                                                           "Dst " + n, "Mod slot " + n + " destination."));
            modPanel.addKnob (std::make_unique<ParamKnob> (state, params::slotId ("Depth", slot),
                                                           "Amt " + n, "Mod slot " + n + " depth."));
        }

        // Two-panel container tabs need a host component each; KnobPanels are
        // members, so tabs must not delete them.
        tabs.addTab ("Amp", juce::Colour (0xff23262c), &ampPanel, false);
        tabs.addTab ("Filter", juce::Colour (0xff23262c), &filterPanel, false);
        tabs.addTab ("Env2", juce::Colour (0xff23262c), &env2Panel, false);
        tabs.addTab ("LFOs", juce::Colour (0xff23262c), &lfoPanel, false);
        tabs.addTab ("Voice", juce::Colour (0xff23262c), &voicePanel, false);
        tabs.addTab ("Mod", juce::Colour (0xff23262c), &modPanel, false);
        addAndMakeVisible (tabs);
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
        // side they affect), then the M4 tab strip below.
        tonePanel.setBounds (r.removeFromTop ((int) (r.getHeight() * 0.42)));
        r.removeFromTop (8);
        tabs.setBounds (r);
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
