#include "Voice.h"

namespace keepsake
{
    namespace
    {
        constexpr int kMaxBlockSize = 8192; // voice scratch buffer capacity
    }

    KeepsakeVoice::KeepsakeVoice (const params::Handles& handles, const SourceStore& store, int voiceIndex)
        : params (handles), sources (store)
    {
        // Each voice gets a distinct RNG stream, otherwise stacked notes would spawn
        // identical grain patterns and sum coherently into a comb filter.
        cloud.setSeed (0x5EED + voiceIndex * 7919);
        voiceBuffer.setSize (2, kMaxBlockSize);
    }

    void KeepsakeVoice::setCurrentPlaybackSampleRate (double newRate)
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);

        if (newRate > 0.0)
        {
            cloud.prepare (newRate);
            adsr.setSampleRate (newRate);
        }
    }

    void KeepsakeVoice::updateEnvelopeParameters()
    {
        adsrParams.attack  = juce::jmax (0.001f, params.ampAttack->load()  * 0.001f);
        adsrParams.decay   = juce::jmax (0.001f, params.ampDecay->load()   * 0.001f);
        adsrParams.sustain = juce::jlimit (0.0f, 1.0f, params.ampSustain->load());
        adsrParams.release = juce::jmax (0.001f, params.ampRelease->load() * 0.001f);
        adsr.setParameters (adsrParams);
    }

    double KeepsakeVoice::computePlaybackRatio() const
    {
        // Spec §2.2: grains are repitched by playback-rate scaling relative to Root.
        const auto root = (double) params.rootNote->load() + (double) params.rootCents->load() * 0.01;
        const auto semitones = ((double) noteNumber + pitchWheelSemitones) - root;

        return std::pow (2.0, semitones / 12.0);
    }

    void KeepsakeVoice::startNote (int midiNoteNumber, float velocity,
                                   juce::SynthesiserSound*, int currentPitchWheelPosition)
    {
        noteNumber = midiNoteNumber;
        noteVelocity = juce::jlimit (0.0f, 1.0f, velocity);
        pitchWheelMoved (currentPitchWheelPosition);

        cloud.reset();
        updateEnvelopeParameters();
        adsr.noteOn();
    }

    void KeepsakeVoice::stopNote (float, bool allowTailOff)
    {
        if (allowTailOff)
        {
            adsr.noteOff();
        }
        else
        {
            // Voice stealing path (spec §2.5: "quick fade-out"). The Synthesiser calls
            // this with allowTailOff=false; clearing immediately would click, so we
            // hand off to a very short release instead of cutting the voice dead.
            auto stealParams = adsrParams;
            stealParams.release = 0.005f;
            adsr.setParameters (stealParams);
            adsr.noteOff();
        }
    }

    void KeepsakeVoice::pitchWheelMoved (int newValue)
    {
        // +/- 2 semitones, the conventional default range.
        pitchWheelSemitones = ((double) newValue - 8192.0) / 8192.0 * 2.0;
    }

    void KeepsakeVoice::renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                                         int startSample, int numSamples)
    {
        if (! adsr.isActive() || numSamples <= 0)
            return;

        const auto* source = sources.getForAudioThread();

        if (source == nullptr)
        {
            // No keepsake loaded: still advance the envelope so that a released note
            // frees its voice instead of hanging forever waiting for audio that will
            // never arrive.
            for (int i = 0; i < numSamples; ++i)
                adsr.getNextSample();

            if (! adsr.isActive())
                clearCurrentNote();

            return;
        }

        updateEnvelopeParameters();

        GrainEngine::Settings s;
        s.place           = (double) params.place->load();
        s.captureLengthMs = (double) params.captureLength->load();
        s.grainSizeMs     = (double) params.grainSize->load();
        s.densityPerSecond = (double) params.grainDensity->load();
        s.drift           = (double) params.grainDrift->load() * 0.01;
        s.shimmerCents    = (double) params.grainShimmer->load();
        s.windowMorph     = (double) params.grainWindow->load();
        s.spread          = (double) params.grainSpread->load() * 0.01;
        s.playbackRatio   = computePlaybackRatio();

        // Render in chunks so an unusually large host block can never overrun the
        // pre-allocated scratch buffer (allocating here would break RT safety).
        int offset = 0;

        while (offset < numSamples)
        {
            const auto chunk = juce::jmin (numSamples - offset, voiceBuffer.getNumSamples());

            voiceBuffer.clear (0, chunk);
            cloud.process (voiceBuffer, 0, chunk, source, s);

            adsr.applyEnvelopeToBuffer (voiceBuffer, 0, chunk);

            const auto velocityGain = 0.3f + 0.7f * noteVelocity;

            for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
                outputBuffer.addFrom (ch, startSample + offset,
                                      voiceBuffer, juce::jmin (ch, 1), 0,
                                      chunk, velocityGain);

            offset += chunk;
        }

        if (! adsr.isActive())
            clearCurrentNote();
    }
}
