#pragma once

#include "engine/CaptureBuffer.h"

namespace ktest
{
    /** A sine at a known frequency - the reference material for pitch-related tests. */
    inline keepsake::SourceAudio::Ptr makeSineSource (double frequency,
                                                      double seconds,
                                                      double sampleRate,
                                                      const juce::String& name = "sine")
    {
        keepsake::SourceAudio::Ptr s (new keepsake::SourceAudio());
        s->sampleRate = sampleRate;
        s->name = name;

        const auto numSamples = (int) (seconds * sampleRate);
        s->buffer.setSize (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto v = (float) std::sin (juce::MathConstants<double>::twoPi * frequency
                                             * (double) i / sampleRate);
            s->buffer.setSample (0, i, v);
            s->buffer.setSample (1, i, v);
        }

        return s;
    }

    /** Largest absolute sample-to-sample step: a blunt but effective click detector. */
    inline float maxDiscontinuity (const juce::AudioBuffer<float>& buffer)
    {
        float worst = 0.0f;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* d = buffer.getReadPointer (ch);

            for (int i = 1; i < buffer.getNumSamples(); ++i)
                worst = juce::jmax (worst, std::abs (d[i] - d[i - 1]));
        }

        return worst;
    }

    inline bool isFinite (const juce::AudioBuffer<float>& buffer)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* d = buffer.getReadPointer (ch);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (! std::isfinite (d[i]))
                    return false;
        }

        return true;
    }
}
