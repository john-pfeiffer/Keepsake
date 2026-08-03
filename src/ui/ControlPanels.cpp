#include "ControlPanels.h"

namespace keepsake
{
    // =========================================================================
    // ParamKnob
    // =========================================================================

    ParamKnob::ParamKnob (juce::AudioProcessorValueTreeState& state,
                          const juce::String& parameterID,
                          const juce::String& humanName,
                          const juce::String& tooltip)
        : attachment (state, parameterID, slider)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
        slider.setTooltip (tooltip);
        addAndMakeVisible (slider);

        // Spec §1: human control names on the face, technical names in tooltips.
        label.setText (humanName, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);
        label.setTooltip (tooltip);
        addAndMakeVisible (label);
    }

    void ParamKnob::resized()
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (16));
        slider.setBounds (r);
    }

    // =========================================================================
    // KnobPanel
    // =========================================================================

    KnobPanel::KnobPanel (juce::String title) : panelTitle (std::move (title)) {}

    void KnobPanel::addKnob (std::unique_ptr<ParamKnob> knob)
    {
        addAndMakeVisible (*knob);
        knobs.push_back (std::move (knob));
        resized();
    }

    void KnobPanel::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();

        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.fillRoundedRectangle (r, 4.0f);

        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (panelTitle, getLocalBounds().reduced (8, 4),
                    juce::Justification::topLeft, false);
    }

    void KnobPanel::resized()
    {
        if (knobs.empty())
            return;

        auto r = getLocalBounds().reduced (6);
        r.removeFromTop (16); // title

        const auto rows = (int) std::ceil ((double) knobs.size() / (double) columns);
        const auto cellW = r.getWidth() / columns;
        const auto cellH = juce::jmax (1, r.getHeight() / juce::jmax (1, rows));

        for (size_t i = 0; i < knobs.size(); ++i)
        {
            const auto col = (int) i % columns;
            const auto row = (int) i / columns;

            knobs[i]->setBounds (r.getX() + col * cellW,
                                 r.getY() + row * cellH,
                                 cellW, cellH);
        }
    }

    // =========================================================================
    // CapturePanel
    // =========================================================================

    CapturePanel::CapturePanel (KeepsakeProcessor& processor) : proc (processor)
    {
        loadButton.onClick = [this] { chooseFile(); };
        playSourceButton.onClick = [this] { proc.startAudition (KeepsakeProcessor::Audition::source); };
        playWindowButton.onClick = [this] { proc.startAudition (KeepsakeProcessor::Audition::window); };
        stopButton.onClick = [this] { proc.stopAudition(); };

        for (auto* b : { &loadButton, &playSourceButton, &playWindowButton, &stopButton })
            addAndMakeVisible (*b);

        statusLabel.setJustificationType (juce::Justification::centredLeft);
        statusLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.65f));
        statusLabel.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (statusLabel);

        startTimerHz (10);
        timerCallback();
    }

    CapturePanel::~CapturePanel() = default;

    void CapturePanel::timerCallback()
    {
        const auto hasSource = proc.getSourceStore().hasSource();

        playSourceButton.setEnabled (hasSource);
        playWindowButton.setEnabled (hasSource);
        stopButton.setEnabled (proc.getAuditionMode() != KeepsakeProcessor::Audition::off);
    }

    void CapturePanel::setStatus (const juce::String& text)
    {
        statusLabel.setText (text, juce::dontSendNotification);
    }

    void CapturePanel::chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> (
            "Choose a sound to keep a moment of...",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory),
            "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.ogg");

        const auto chooserFlags = juce::FileBrowserComponent::openMode
                           | juce::FileBrowserComponent::canSelectFiles;

        chooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();

            if (file == juce::File())
                return;

            const auto result = proc.importFile (file);
            setStatus (result.message);
        });
    }

    void CapturePanel::resized()
    {
        auto r = getLocalBounds().reduced (4);
        const auto buttonWidth = 84;
        const auto gap = 4;

        loadButton.setBounds (r.removeFromLeft (buttonWidth));
        r.removeFromLeft (gap * 3);
        playSourceButton.setBounds (r.removeFromLeft (buttonWidth));
        r.removeFromLeft (gap);
        playWindowButton.setBounds (r.removeFromLeft (buttonWidth));
        r.removeFromLeft (gap);
        stopButton.setBounds (r.removeFromLeft (60));
        r.removeFromLeft (gap * 3);
        statusLabel.setBounds (r);
    }
}
