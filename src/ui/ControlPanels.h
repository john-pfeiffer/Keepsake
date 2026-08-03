#pragma once

#include "../PluginProcessor.h"

namespace keepsake
{
    /**
        A labelled rotary bound to one APVTS parameter.

        Spec §3: "stock JUCE components with light cleanup only ... No custom
        LookAndFeel artwork, no themes." This is exactly that - a Slider, a Label, and
        the tooltip carrying the technical name.
    */
    class ParamKnob : public juce::Component
    {
    public:
        ParamKnob (juce::AudioProcessorValueTreeState& state,
                   const juce::String& parameterID,
                   const juce::String& humanName,
                   const juce::String& tooltip);

        void resized() override;

    private:
        juce::Slider slider;
        juce::Label label;
        juce::AudioProcessorValueTreeState::SliderAttachment attachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamKnob)
    };

    /** A titled box that lays its children out in a simple grid of knobs. */
    class KnobPanel : public juce::Component
    {
    public:
        explicit KnobPanel (juce::String title);

        void addKnob (std::unique_ptr<ParamKnob> knob);
        void setColumns (int n) { columns = juce::jmax (1, n); }

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        juce::String panelTitle;
        std::vector<std::unique_ptr<ParamKnob>> knobs;
        int columns = 3;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobPanel)
    };

    /** File load / audition transport row that sits under the waveform. */
    class CapturePanel : public juce::Component,
                         private juce::Timer
    {
    public:
        explicit CapturePanel (KeepsakeProcessor& processor);
        ~CapturePanel() override;

        void resized() override;
        void setStatus (const juce::String& text);

    private:
        void timerCallback() override;
        void chooseFile();

        KeepsakeProcessor& proc;

        juce::TextButton loadButton { "Load..." };
        juce::TextButton playSourceButton { "Play file" };
        juce::TextButton playWindowButton { "Play keep" };
        juce::TextButton stopButton { "Stop" };
        juce::Label statusLabel;
        std::unique_ptr<juce::FileChooser> chooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CapturePanel)
    };
}
