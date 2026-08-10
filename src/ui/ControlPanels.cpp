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
        label.setBounds (r.removeFromTop (14));

        // The rotary gets ALL remaining height. The value box is dropped
        // entirely in cells too short to fit both - overlapping text (the
        // rc.1 layout) is worse than no box, and the tooltip still carries
        // the exact value name.
        const auto showBox = r.getHeight() >= 52;
        slider.setTextBoxStyle (showBox ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox,
                                false, juce::jmin (70, getWidth() - 6), 15);
        slider.setBounds (r);
    }

    // =========================================================================
    // ParamChoice
    // =========================================================================

    ParamChoice::ParamChoice (juce::AudioProcessorValueTreeState& state,
                              const juce::String& parameterID,
                              const juce::String& humanName,
                              const juce::String& tooltip)
    {
        // Items first, then attach: the attachment maps item indices to the
        // parameter but does not populate the list itself.
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (parameterID)))
            box.addItemList (choice->choices, 1);

        box.setTooltip (tooltip);
        addAndMakeVisible (box);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            state, parameterID, box);

        label.setText (humanName, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);
        label.setTooltip (tooltip);
        addAndMakeVisible (label);
    }

    void ParamChoice::resized()
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (14));

        auto row = r.withSizeKeepingCentre (juce::jmin (getWidth() - 4, 110),
                                            juce::jmin (24, r.getHeight()));
        box.setBounds (row);
    }

    // =========================================================================
    // ParamToggle
    // =========================================================================

    ParamToggle::ParamToggle (juce::AudioProcessorValueTreeState& state,
                              const juce::String& parameterID,
                              const juce::String& humanName,
                              const juce::String& tooltip)
        : attachment (state, parameterID, button)
    {
        button.setTooltip (tooltip);
        addAndMakeVisible (button);

        label.setText (humanName, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);
        label.setTooltip (tooltip);
        addAndMakeVisible (label);
    }

    void ParamToggle::resized()
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (14));
        button.setBounds (r.withSizeKeepingCentre (juce::jmin (getWidth() - 4, 28),
                                                   juce::jmin (24, r.getHeight())));
    }

    // =========================================================================
    // KnobPanel
    // =========================================================================

    KnobPanel::KnobPanel (juce::String title) : panelTitle (std::move (title)) {}

    void KnobPanel::addKnob (std::unique_ptr<juce::Component> control)
    {
        addAndMakeVisible (*control);
        knobs.push_back (std::move (control));
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
