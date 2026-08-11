#include "WaveformView.h"

namespace keepsake
{
    namespace
    {
        constexpr float kEdgeGrabPixels = 6.0f;
        constexpr double kMinViewSamples = 64.0;
    }

    WaveformView::WaveformView (KeepsakeProcessor& processor) : proc (processor)
    {
        proc.sourceChanged.addChangeListener (this);
        setWantsKeyboardFocus (false);
        startTimerHz (30); // playhead + parameter-driven bracket updates

        // Hosts destroy and recreate the editor on window close/reopen; a
        // fresh view must pull the already-loaded source rather than sit
        // blank waiting for the NEXT sourceChanged broadcast (KPT-2).
        refresh();
    }

    WaveformView::~WaveformView()
    {
        proc.sourceChanged.removeChangeListener (this);
    }

    void WaveformView::changeListenerCallback (juce::ChangeBroadcaster*)
    {
        refresh();
    }

    void WaveformView::refresh()
    {
        const auto source = proc.getSourceStore().getForMessageThread();

        viewStart = 0.0;
        viewLength = source != nullptr ? (double) source->getNumSamples() : 0.0;
        peaksViewStart = -1.0; // force a peak rebuild

        repaint();
    }

    void WaveformView::timerCallback()
    {
        // The bracket is parameter-driven, so host automation of Place has to move it
        // even when nothing here was clicked. Repaint only on an actual change -
        // an unconditional 30Hz repaint of a full-width waveform is not free.
        const auto playhead = proc.getAuditionPositionNormalised();
        const auto bracket = getBracketSamples();

        const auto changed = std::abs (playhead - playheadNormalised) > 1.0e-4
                             || std::abs (bracket.getStart() - lastBracket.getStart()) > 0.5
                             || std::abs (bracket.getEnd() - lastBracket.getEnd()) > 0.5;

        if (! changed)
            return;

        playheadNormalised = playhead;
        lastBracket = bracket;
        repaint();
    }

    void WaveformView::resized()
    {
        peaksViewStart = -1.0;
    }

    // =========================================================================
    // View <-> sample mapping
    // =========================================================================

    void WaveformView::setViewRange (double start, double length)
    {
        const auto source = proc.getSourceStore().getForMessageThread();

        if (source == nullptr)
            return;

        const auto total = (double) source->getNumSamples();

        viewLength = juce::jlimit (juce::jmin (kMinViewSamples, total), total, length);
        viewStart = juce::jlimit (0.0, juce::jmax (0.0, total - viewLength), start);
    }

    double WaveformView::sampleToX (double sample) const
    {
        if (viewLength <= 0.0)
            return 0.0;

        return (sample - viewStart) / viewLength * (double) getWidth();
    }

    double WaveformView::xToSample (double x) const
    {
        if (getWidth() <= 0)
            return viewStart;

        return viewStart + x / (double) getWidth() * viewLength;
    }

    juce::Range<double> WaveformView::getBracketSamples() const
    {
        const auto source = proc.getSourceStore().getForMessageThread();

        if (source == nullptr)
            return { 0.0, 0.0 };

        auto& state = proc.getState();
        const auto place = (double) state.getRawParameterValue (params::place)->load();
        const auto lengthMs = (double) state.getRawParameterValue (params::captureLength)->load();

        const auto w = CaptureWindow::resolve (*source, place, lengthMs, source->sampleRate);

        return { (double) w.startSample, (double) w.endSample() };
    }

    // =========================================================================
    // Parameter writes (gesture-wrapped so hosts record clean automation)
    // =========================================================================

    void WaveformView::setPlaceFromStartSample (double startSample)
    {
        const auto source = proc.getSourceStore().getForMessageThread();

        if (source == nullptr)
            return;

        auto& state = proc.getState();
        const auto lengthMs = (double) state.getRawParameterValue (params::captureLength)->load();
        const auto lengthSamples = juce::jmax (1.0, lengthMs * 0.001 * source->sampleRate);
        const auto maxStart = juce::jmax (1.0, (double) source->getNumSamples() - lengthSamples);

        if (auto* p = state.getParameter (params::place))
            p->setValueNotifyingHost ((float) juce::jlimit (0.0, 1.0, startSample / maxStart));
    }

    void WaveformView::setLengthFromSamples (double lengthSamples, bool anchorRightEdge)
    {
        const auto source = proc.getSourceStore().getForMessageThread();

        if (source == nullptr)
            return;

        auto& state = proc.getState();
        const auto oldBracket = getBracketSamples();

        const auto lengthMs = juce::jlimit ((double) params::kKeepLengthMinMs,
                                            (double) params::kKeepLengthMaxMs,
                                            lengthSamples / source->sampleRate * 1000.0);

        if (auto* p = state.getParameter (params::captureLength))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) lengthMs));

        // Dragging the left edge should keep the right edge still, which means Place
        // has to move too - otherwise the bracket appears to grow the wrong way.
        if (anchorRightEdge)
        {
            const auto newLengthSamples = lengthMs * 0.001 * source->sampleRate;
            setPlaceFromStartSample (oldBracket.getEnd() - newLengthSamples);
        }
    }

    // =========================================================================
    // Painting
    // =========================================================================

    void WaveformView::rebuildPeaksIfNeeded()
    {
        const auto source = proc.getSourceStore().getForMessageThread();
        const auto width = getWidth();

        if (source == nullptr || width <= 0 || viewLength <= 0.0)
        {
            peaks.clear();
            return;
        }

        if (peaksWidth == width
            && std::abs (peaksViewStart - viewStart) < 1.0e-6
            && std::abs (peaksViewLength - viewLength) < 1.0e-6)
            return;

        peaks.assign ((size_t) width, juce::Range<float> (0.0f, 0.0f));

        const auto total = source->getNumSamples();
        const auto samplesPerPixel = viewLength / (double) width;

        for (int x = 0; x < width; ++x)
        {
            const auto from = (int) std::floor (viewStart + (double) x * samplesPerPixel);
            const auto to = (int) std::ceil (viewStart + (double) (x + 1) * samplesPerPixel);

            const auto a = juce::jlimit (0, total, from);
            const auto b = juce::jlimit (a + 1, total, juce::jmax (to, a + 1));

            if (a >= total)
                break;

            auto range = source->buffer.findMinMax (0, a, b - a);

            if (source->getNumChannels() > 1)
                range = range.getUnionWith (source->buffer.findMinMax (1, a, b - a));

            peaks[(size_t) x] = range;
        }

        peaksWidth = width;
        peaksViewStart = viewStart;
        peaksViewLength = viewLength;
    }

    void WaveformView::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();

        g.setColour (juce::Colour (0xff14161a));
        g.fillRect (bounds);

        const auto source = proc.getSourceStore().getForMessageThread();

        if (source == nullptr || source->getNumSamples() <= 0)
        {
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.setFont (juce::FontOptions (16.0f));
            g.drawText ("Drop an audio file here, or use Load...",
                        getLocalBounds(), juce::Justification::centred);
            return;
        }

        rebuildPeaksIfNeeded();

        const auto midY = bounds.getCentreY();
        const auto halfHeight = bounds.getHeight() * 0.45f;

        // Waveform
        g.setColour (juce::Colour (0xff6f7d8c));

        for (size_t x = 0; x < peaks.size(); ++x)
        {
            const auto& r = peaks[x];
            const auto top = midY - r.getEnd() * halfHeight;
            const auto bottom = midY - r.getStart() * halfHeight;

            g.drawLine ((float) x + 0.5f, top, (float) x + 0.5f, juce::jmax (bottom, top + 1.0f), 1.0f);
        }

        // Zero line
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) midY, bounds.getX(), bounds.getRight());

        // Capture bracket - the warm "photo frame" of spec §3.
        const auto bracket = getBracketSamples();
        const auto x1 = (float) sampleToX (bracket.getStart());
        const auto x2 = (float) sampleToX (bracket.getEnd());

        const juce::Colour warm (0xffe0a458);

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect (bounds.withRight (juce::jmax (bounds.getX(), x1)));
        g.fillRect (bounds.withLeft (juce::jmin (bounds.getRight(), x2)));

        g.setColour (warm.withAlpha (0.14f));
        g.fillRect (juce::Rectangle<float> (x1, bounds.getY(), juce::jmax (1.0f, x2 - x1), bounds.getHeight()));

        g.setColour (warm);
        g.drawLine (x1, bounds.getY(), x1, bounds.getBottom(), 2.0f);
        g.drawLine (x2, bounds.getY(), x2, bounds.getBottom(), 2.0f);

        // Playhead
        if (playheadNormalised >= 0.0)
        {
            const auto px = (float) sampleToX (playheadNormalised * (double) source->getNumSamples());

            if (bounds.contains (px, midY))
            {
                g.setColour (juce::Colours::white.withAlpha (0.8f));
                g.drawLine (px, bounds.getY(), px, bounds.getBottom(), 1.0f);
            }
        }

        // Readout: file name, zoom level, bracket length
        const auto lengthMs = (bracket.getLength() / source->sampleRate) * 1000.0;
        const auto viewMs = (viewLength / source->sampleRate) * 1000.0;

        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (source->name, getLocalBounds().reduced (8, 6),
                    juce::Justification::topLeft, true);
        g.drawText (juce::String (lengthMs, 1) + " ms kept   -   view "
                        + (viewMs >= 1000.0 ? juce::String (viewMs / 1000.0, 2) + " s"
                                            : juce::String (viewMs, 0) + " ms"),
                    getLocalBounds().reduced (8, 6),
                    juce::Justification::topRight, true);
    }

    // =========================================================================
    // Interaction
    // =========================================================================

    WaveformView::Drag WaveformView::hitTest (juce::Point<float> p) const
    {
        const auto bracket = getBracketSamples();
        const auto x1 = (float) sampleToX (bracket.getStart());
        const auto x2 = (float) sampleToX (bracket.getEnd());

        if (std::abs (p.x - x1) <= kEdgeGrabPixels)
            return Drag::leftEdge;

        if (std::abs (p.x - x2) <= kEdgeGrabPixels)
            return Drag::rightEdge;

        if (p.x > x1 && p.x < x2)
            return Drag::move;

        return Drag::scroll;
    }

    void WaveformView::mouseMove (const juce::MouseEvent& e)
    {
        switch (hitTest (e.position))
        {
            case Drag::leftEdge:
            case Drag::rightEdge: setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
            case Drag::move:      setMouseCursor (juce::MouseCursor::DraggingHandCursor); break;
            case Drag::scroll:
            case Drag::none:      setMouseCursor (juce::MouseCursor::NormalCursor); break;
        }
    }

    void WaveformView::mouseDown (const juce::MouseEvent& e)
    {
        if (proc.getSourceStore().getForMessageThread() == nullptr)
            return;

        currentDrag = hitTest (e.position);
        dragAnchorViewStart = viewStart;

        auto& state = proc.getState();

        if (currentDrag == Drag::scroll)
        {
            // A plain click outside the bracket moves the bracket there rather than
            // making the user drag it across the whole file.
            if (! e.mods.isRightButtonDown())
            {
                const auto bracket = getBracketSamples();
                state.getParameter (params::place)->beginChangeGesture();
                setPlaceFromStartSample (xToSample (e.position.x) - bracket.getLength() * 0.5);
                currentDrag = Drag::move;
                dragAnchorSample = xToSample (e.position.x) - getBracketSamples().getStart();
                return;
            }

            dragAnchorSample = xToSample (e.position.x);
            return;
        }

        if (currentDrag == Drag::move)
        {
            state.getParameter (params::place)->beginChangeGesture();
            dragAnchorSample = xToSample (e.position.x) - getBracketSamples().getStart();
        }
        else
        {
            state.getParameter (params::captureLength)->beginChangeGesture();
            state.getParameter (params::place)->beginChangeGesture();
        }
    }

    void WaveformView::mouseDrag (const juce::MouseEvent& e)
    {
        const auto source = proc.getSourceStore().getForMessageThread();

        if (source == nullptr || currentDrag == Drag::none)
            return;

        const auto bracket = getBracketSamples();

        switch (currentDrag)
        {
            case Drag::move:
                setPlaceFromStartSample (xToSample (e.position.x) - dragAnchorSample);
                break;

            case Drag::leftEdge:
                setLengthFromSamples (bracket.getEnd() - xToSample (e.position.x), true);
                break;

            case Drag::rightEdge:
                setLengthFromSamples (xToSample (e.position.x) - bracket.getStart(), false);
                break;

            case Drag::scroll:
            {
                const auto deltaSamples = (double) -e.getDistanceFromDragStartX()
                                          / (double) juce::jmax (1, getWidth()) * viewLength;
                setViewRange (dragAnchorViewStart + deltaSamples, viewLength);
                break;
            }

            case Drag::none:
                break;
        }

        repaint();
    }

    void WaveformView::mouseUp (const juce::MouseEvent&)
    {
        auto& state = proc.getState();

        if (currentDrag == Drag::move)
        {
            state.getParameter (params::place)->endChangeGesture();
        }
        else if (currentDrag == Drag::leftEdge || currentDrag == Drag::rightEdge)
        {
            state.getParameter (params::captureLength)->endChangeGesture();
            state.getParameter (params::place)->endChangeGesture();
        }

        currentDrag = Drag::none;
    }

    void WaveformView::mouseDoubleClick (const juce::MouseEvent&)
    {
        zoomToFitBracket();
    }

    void WaveformView::zoomToFitBracket()
    {
        const auto bracket = getBracketSamples();

        if (bracket.getLength() <= 0.0)
            return;

        // Show the bracket with a comfortable margin either side.
        const auto length = bracket.getLength() * 3.0;
        setViewRange (bracket.getStart() - bracket.getLength(), length);
        repaint();
    }

    void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        const auto source = proc.getSourceStore().getForMessageThread();

        if (source == nullptr)
            return;

        if (e.mods.isShiftDown())
        {
            setViewRange (viewStart - wheel.deltaY * viewLength, viewLength);
        }
        else
        {
            // Zoom about the pointer, so the sample under the cursor stays put.
            const auto anchor = xToSample (e.position.x);
            const auto factor = std::pow (2.0, -wheel.deltaY * 1.5);
            const auto newLength = viewLength * factor;
            const auto anchorFraction = viewLength > 0.0 ? (anchor - viewStart) / viewLength : 0.5;

            setViewRange (anchor - anchorFraction * newLength, newLength);
        }

        repaint();
    }
}
