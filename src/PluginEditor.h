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
                           public juce::FileDragAndDropTarget
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
            KnobPanel cloudPanel { "CLOUD  -  granular" };
            KnobPanel tonePanel { "TONE  -  wavetable" };
            KnobPanel rootPanel { "KEEPSAKE" };
            KnobPanel ampPanel { "AMP ENVELOPE" };
            KnobPanel outputPanel { "OUTPUT" };
            juce::Label placeholder;
        };

        KeepsakeProcessor& proc;
        Content content;
        juce::TooltipWindow tooltips { this, 500 };
        bool fileDragActive = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeepsakeEditor)
    };
}
