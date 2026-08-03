#pragma once

#include "../PluginProcessor.h"

namespace keepsake
{
    /**
        The hero control: the source waveform with the capture bracket drawn over it.

        Spec §3 - "selecting 15ms out of a 3-minute file needs good zoom UX", so this
        view keeps its own zoom/scroll window over the source and drives the Place and
        Keep Length parameters through the APVTS (never through hidden state).

        Interaction:
          - drag the middle of the bracket          -> Place
          - drag either bracket edge                -> Keep Length (anchored on the far edge)
          - click anywhere in the waveform          -> move the bracket there
          - mouse wheel                             -> zoom about the pointer
          - shift + wheel, or drag outside bracket  -> scroll
          - double click                            -> zoom to fit the bracket
    */
    class WaveformView : public juce::Component,
                         private juce::ChangeListener,
                         private juce::Timer
    {
    public:
        explicit WaveformView (KeepsakeProcessor& processor);
        ~WaveformView() override;

        void paint (juce::Graphics& g) override;
        void resized() override;

        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;
        void mouseMove (const juce::MouseEvent& e) override;
        void mouseDoubleClick (const juce::MouseEvent& e) override;
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

        /** Discards the cached peaks; call when the source is replaced. */
        void refresh();

    private:
        enum class Drag { none, move, leftEdge, rightEdge, scroll };

        void changeListenerCallback (juce::ChangeBroadcaster*) override;
        void timerCallback() override;

        void rebuildPeaksIfNeeded();
        void zoomToFitBracket();
        void setViewRange (double start, double length);

        double sampleToX (double sample) const;
        double xToSample (double x) const;
        juce::Range<double> getBracketSamples() const;
        Drag hitTest (juce::Point<float> p) const;

        void setPlaceFromStartSample (double startSample);
        void setLengthFromSamples (double lengthSamples, bool anchorRightEdge);

        KeepsakeProcessor& proc;

        // The visible window over the source, in source samples.
        double viewStart = 0.0;
        double viewLength = 0.0;

        // Cached min/max peaks, one pair per pixel column of the last drawn view.
        std::vector<juce::Range<float>> peaks;
        double peaksViewStart = -1.0;
        double peaksViewLength = -1.0;
        int peaksWidth = 0;

        Drag currentDrag = Drag::none;
        double dragAnchorSample = 0.0;
        double dragAnchorViewStart = 0.0;
        double playheadNormalised = -1.0;
        juce::Range<double> lastBracket { 0.0, 0.0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
    };
}
