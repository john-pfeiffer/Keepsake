#pragma once

#include "PluginProcessor.h"
#include "ui/WaveformView.h"
#include "ui/ControlPanels.h"

namespace keepsake
{
    /**
        Single resizable window (spec §3).

        Everything is laid out once at the design size and then scaled by an
        AffineTransform, so the layout code never has to think about the current
        window size. That is the "scalable coordinate system from the start" the spec
        asks for, rather than a retrofit.
    */
    class KeepsakeEditor : public juce::AudioProcessorEditor,
                           public juce::FileDragAndDropTarget,
                           private juce::Timer
    {
    public:
        static constexpr int kDesignWidth = 900;
        static constexpr int kDesignHeight = 540;

        explicit KeepsakeEditor (KeepsakeProcessor&);
        ~KeepsakeEditor() override;

        void paint (juce::Graphics&) override;
        void resized() override;

        bool isInterestedInFileDrag (const juce::StringArray& files) override;
        void fileDragEnter (const juce::StringArray&, int, int) override;
        void fileDragExit (const juce::StringArray&) override;
        void filesDropped (const juce::StringArray& files, int x, int y) override;

    private:
        /** The spec §3 top bar: prev/next, name, save - plus Randomize and its
            single-level undo. Momentary actions, not parameters (audition
            precedent); the name itself travels inside the plugin state. */
        class PresetBar : public juce::Component
        {
        public:
            explicit PresetBar (KeepsakeProcessor&);
            void resized() override;

            /** Re-reads the preset list and the current display name. */
            void refresh();

        private:
            KeepsakeProcessor& proc;
            juce::TextButton prev { "<" }, next { ">" }, save { "Save" },
                             random { "Random" }, back { "Back" };
            juce::ComboBox name;
        };

        /** Holds the real UI at design size; the editor scales this one child. */
        class Content : public juce::Component
        {
        public:
            explicit Content (KeepsakeProcessor&);
            void paint (juce::Graphics&) override;
            void resized() override;

            WaveformView waveform;
            CapturePanel capture;

        private:
            PresetBar presetBar;
            KnobPanel cloudPanel { "CLOUD  -  granular" };
            KnobPanel tonePanel { "TONE  -  wavetable" };
            KnobPanel rootPanel { "KEEPSAKE" };

            // M4 controls live in tabs (the spec's "mod matrix behind a
            // drawer/tab"); stock TabbedComponent, stock knobs, no styling.
            juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
            KnobPanel ampPanel { "AMP + OUTPUT" };
            KnobPanel filterPanel { "FILTER" };
            KnobPanel env2Panel { "ENV2" };
            KnobPanel lfoPanel { "LFO 1 / LFO 2" };
            KnobPanel voicePanel { "VOICE" };
            KnobPanel modPanel { "MOD MATRIX" };
            KnobPanel fxPanel { "FX  -  Warmth / Chorus / Air" };
        };

        /** One-shot post-attach 1px resize nudge - forces Logic's AU
            container to recompute its clickable region (KPT-1 mitigation). */
        void timerCallback() override;

        KeepsakeProcessor& proc;
        Content content;
        juce::TooltipWindow tooltips { this, 500 };
        bool fileDragActive = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeepsakeEditor)
    };
}
