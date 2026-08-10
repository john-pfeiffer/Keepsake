#include "PluginEditor.h"

namespace keepsake
{
    // =========================================================================
    // PresetBar
    // =========================================================================

    KeepsakeEditor::PresetBar::PresetBar (KeepsakeProcessor& p) : proc (p)
    {
        name.setEditableText (true);
        name.setTextWhenNothingSelected ("(unsaved)");
        name.setTooltip ("Preset name - type one and press Save, or pick one to load.");
        addAndMakeVisible (name);

        for (auto* b : { &prev, &next, &save, &random, &back })
            addAndMakeVisible (*b);

        prev.setTooltip ("Previous preset");
        next.setTooltip ("Next preset");
        save.setTooltip ("Save this sound - keepsake included - as a preset");
        random.setTooltip ("Randomize every control except the output level and the loaded audio");
        back.setTooltip ("Undo the last Randomize");
        back.setEnabled (false);

        prev.onClick = [this] { proc.getPresetManager().loadPrevious(); refresh(); };
        next.onClick = [this] { proc.getPresetManager().loadNext(); refresh(); };

        save.onClick = [this]
        {
            auto entered = name.getText().trim();

            if (entered.isEmpty())
                entered = "Preset " + juce::String (proc.getPresetManager().getPresetNames().size() + 1);

            proc.getPresetManager().savePreset (entered);
            refresh();
        };

        random.onClick = [this]
        {
            proc.randomizeParameters (juce::Random::getSystemRandom().nextInt64());
            back.setEnabled (true);
        };

        back.onClick = [this]
        {
            proc.undoRandomize();
            back.setEnabled (false); // single-level stash (spec §3)
        };

        name.onChange = [this]
        {
            // Fires for real selections only; refresh() repopulates silently.
            const auto selected = name.getSelectedItemIndex();

            if (selected >= 0)
            {
                proc.getPresetManager().loadPreset (selected);
                refresh();
            }
        };

        refresh();
    }

    void KeepsakeEditor::PresetBar::refresh()
    {
        auto& presets = proc.getPresetManager();

        name.clear (juce::dontSendNotification);

        int id = 1;
        for (const auto& presetName : presets.getPresetNames())
            name.addItem (presetName, id++);

        name.setText (proc.getPresetDisplayName(), juce::dontSendNotification);
    }

    void KeepsakeEditor::PresetBar::resized()
    {
        auto r = getLocalBounds();

        prev.setBounds (r.removeFromLeft (26));
        r.removeFromLeft (2);
        next.setBounds (r.removeFromLeft (26));
        r.removeFromLeft (6);

        back.setBounds (r.removeFromRight (52));
        r.removeFromRight (4);
        random.setBounds (r.removeFromRight (68));
        r.removeFromRight (10);
        save.setBounds (r.removeFromRight (52));
        r.removeFromRight (6);

        name.setBounds (r);
    }

    // =========================================================================
    // Content - laid out once, at design size
    // =========================================================================

    KeepsakeEditor::Content::Content (KeepsakeProcessor& p)
        : waveform (p), capture (p), presetBar (p)
    {
        addAndMakeVisible (presetBar);
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
                                 "Focus - the one-knob morph from Cloud to Tone. Coupled: "
                                 "the cloud condenses toward Tone, and Tone ducks and "
                                 "detunes toward Cloud (spec 2.4)."));
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

        // --- FX (spec §2.6: deliberately small) ------------------------------
        fxPanel.setColumns (4);
        fxPanel.addKnob (knob (params::warmthAmount, "Warmth",
                               "Soft saturation (tanh), drive-compensated - color, not volume."));
        fxPanel.addKnob (knob (params::chorusAmount, "Chorus",
                               "One-knob chorus macro - rate, depth and mix move together."));
        fxPanel.addKnob (knob (params::airSize, "Size", "Air - reverb room size."));
        fxPanel.addKnob (knob (params::airMix, "Air",
                               "Air send level. The dry signal is never ducked; per-voice "
                               "Reverb Mix modulation adds on top, audible even at 0."));

        // Two-panel container tabs need a host component each; KnobPanels are
        // members, so tabs must not delete them.
        tabs.addTab ("Amp", juce::Colour (0xff23262c), &ampPanel, false);
        tabs.addTab ("Filter", juce::Colour (0xff23262c), &filterPanel, false);
        tabs.addTab ("Env2", juce::Colour (0xff23262c), &env2Panel, false);
        tabs.addTab ("LFOs", juce::Colour (0xff23262c), &lfoPanel, false);
        tabs.addTab ("Voice", juce::Colour (0xff23262c), &voicePanel, false);
        tabs.addTab ("Mod", juce::Colour (0xff23262c), &modPanel, false);
        tabs.addTab ("FX", juce::Colour (0xff23262c), &fxPanel, false);
        addAndMakeVisible (tabs);
    }

    void KeepsakeEditor::Content::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour (0xff1c1f24));
    }

    void KeepsakeEditor::Content::resized()
    {
        auto r = getLocalBounds();

        presetBar.setBounds (r.removeFromTop (30).reduced (8, 3));

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

        // Resizable from v1 (spec §3 / §8). Deliberately NO fixed-aspect
        // constrainer: a constrainer that "snaps back" a size the host just
        // imposed is a known trigger for Logic/GarageBand's macOS Tahoe
        // out-of-process AU bug, where the container drops mouse events in
        // whole regions of the view (KPT-1/KPT-3). We accept ANY size and
        // letterbox the content instead - the design stays proportional, and
        // nothing ever disagrees with the host about geometry.
        setResizable (true, true);

        if (auto* bounds = getConstrainer())
            bounds->setSizeLimits (kDesignWidth * 2 / 3, kDesignHeight * 2 / 3,
                                   kDesignWidth * 2, kDesignHeight * 2);

        setSize (kDesignWidth, kDesignHeight);

        // The same Logic container bug can leave its clickable region stale
        // for the size the editor opened at; a 1px resize shortly after
        // attach forces it to recompute (established JUCE-forum workaround).
        // Harmless everywhere else.
        startTimer (50);
    }

    KeepsakeEditor::~KeepsakeEditor() = default;

    void KeepsakeEditor::timerCallback()
    {
        stopTimer();

        // Downward, so the constrainer's maximum can never clamp this into a
        // no-op; the minimum is well below the default size.
        const auto w = getWidth();
        const auto h = getHeight();
        setSize (w - 1, h);
        setSize (w, h);
    }

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
        // One transform scales the whole design-size layout to the current
        // window, letterboxed: whatever aspect ratio the host imposes, the
        // content keeps its proportions and centers, and the editor never
        // pushes a "corrected" size back at the host (see the constructor).
        const auto scale = juce::jmin ((float) getWidth() / (float) kDesignWidth,
                                       (float) getHeight() / (float) kDesignHeight);
        const auto offsetX = ((float) getWidth() - kDesignWidth * scale) * 0.5f;
        const auto offsetY = ((float) getHeight() - kDesignHeight * scale) * 0.5f;

        content.setTransform (juce::AffineTransform::scale (scale)
                                  .translated (offsetX, offsetY));
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
