#include "Voice.h"
#include "Analysis.h"

namespace keepsake
{
    namespace
    {
        constexpr int kMaxBlockSize = 8192; // voice scratch buffer capacity
    }

    KeepsakeVoice::KeepsakeVoice (const params::Handles& handles, const SourceStore& store,
                                  const WavetableStore& wavetableStore, int voiceIndex)
        : params (handles), sources (store), wavetables (wavetableStore)
    {
        // Each voice gets a distinct RNG stream, otherwise stacked notes would spawn
        // identical grain patterns and sum coherently into a comb filter.
        cloud.setSeed (0x5EED + voiceIndex * 7919);
        voiceBuffer.setSize (2, kMaxBlockSize);
        toneBuffer.setSize (1, kMaxBlockSize);
    }

    void KeepsakeVoice::setCurrentPlaybackSampleRate (double newRate)
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);

        if (newRate > 0.0)
        {
            cloud.prepare (newRate);
            tone.prepare (newRate); // also drops held wavetable pointers (GC safety)
            adsr.setSampleRate (newRate);

            cloudGain.reset (newRate, 0.02);
            toneGain.reset (newRate, 0.02);
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
        tone.noteOn (wavetables.getForAudioThread()); // capture the set cold, no fade
        updateEnvelopeParameters();
        adsr.noteOn();

        // Snap the focus blend at note start so the attack is not a fade artefact.
        const auto focus = juce::jlimit (0.0f, 1.0f, params.focus->load());
        cloudGain.setCurrentAndTargetValue (std::cos (focus * juce::MathConstants<float>::halfPi));
        toneGain.setCurrentAndTargetValue (std::sin (focus * juce::MathConstants<float>::halfPi));
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

        // Equal-power Cloud/Tone blend (the engines are uncorrelated). Targets are
        // smoothed so Focus automation does not zipper; during a steady render the
        // gains are constant, which keeps block-size-independence renders identical.
        const auto focus = juce::jlimit (0.0f, 1.0f, params.focus->load());
        cloudGain.setTargetValue (std::cos (focus * juce::MathConstants<float>::halfPi));
        toneGain.setTargetValue (std::sin (focus * juce::MathConstants<float>::halfPi));

        const auto toneFrequency =
            analysis::noteFrequencyHz ((double) noteNumber + pitchWheelSemitones);
        const auto framePos = (double) params.toneFrame->load();
        const auto* wavetableSet = wavetables.getForAudioThread();

        // Render in chunks so an unusually large host block can never overrun the
        // pre-allocated scratch buffer (allocating here would break RT safety).
        int offset = 0;

        while (offset < numSamples)
        {
            const auto chunk = juce::jmin (numSamples - offset, voiceBuffer.getNumSamples());

            voiceBuffer.clear (0, chunk);

            // Exactly zero gain skips an engine outright. Beyond saving CPU this
            // guarantees Focus at 0 renders bit-identically to the pre-Tone (M2)
            // engine, which a regression test asserts.
            const auto cloudActive = cloudGain.getCurrentValue() > 0.0f
                                     || cloudGain.getTargetValue() > 0.0f;
            const auto toneActive = toneGain.getCurrentValue() > 0.0f
                                    || toneGain.getTargetValue() > 0.0f;

            if (cloudActive)
                cloud.process (voiceBuffer, 0, chunk, source, s);

            if (toneActive)
                tone.process (toneBuffer.getWritePointer (0), chunk,
                              wavetableSet, toneFrequency, framePos);

            {
                auto* left = voiceBuffer.getWritePointer (0);
                auto* right = voiceBuffer.getWritePointer (1);
                const auto* toneMono = toneBuffer.getReadPointer (0);

                for (int i = 0; i < chunk; ++i)
                {
                    const auto cg = cloudGain.getNextValue();
                    const auto tg = toneGain.getNextValue();
                    const auto t = toneActive ? toneMono[i] * tg : 0.0f;

                    left[i] = left[i] * cg + t;
                    right[i] = right[i] * cg + t;
                }
            }

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
