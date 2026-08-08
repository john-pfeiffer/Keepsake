#pragma once

#include "GrainEngine.h"
#include "ToneEngine.h"
#include "../Parameters.h"

namespace keepsake
{
    /** Trivial sound: Keepsake responds on all notes and channels. */
    struct KeepsakeSound : public juce::SynthesiserSound
    {
        bool appliesToNote (int) override    { return true; }
        bool appliesToChannel (int) override { return true; }
    };

    /**
        One polyphonic voice.

        Owns one Cloud (granular) unit, one Tone (wavetable) unit and the amp
        envelope. Cloud renders into the stereo voice buffer, Tone into a mono
        scratch; the two are blended by the Focus parameter (equal-power - the
        engines are uncorrelated) before the envelope. The SVF filter and ENV2
        land here in M4.
    */
    class KeepsakeVoice : public juce::SynthesiserVoice
    {
    public:
        KeepsakeVoice (const params::Handles& handles, const SourceStore& store,
                       const WavetableStore& wavetables, int voiceIndex);

        bool canPlaySound (juce::SynthesiserSound* sound) override
        {
            return dynamic_cast<KeepsakeSound*> (sound) != nullptr;
        }

        void startNote (int midiNoteNumber, float velocity,
                        juce::SynthesiserSound*, int currentPitchWheelPosition) override;
        void stopNote (float velocity, bool allowTailOff) override;
        void pitchWheelMoved (int newValue) override;
        void controllerMoved (int, int) override {}

        void setCurrentPlaybackSampleRate (double newRate) override;
        void renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                              int startSample, int numSamples) override;

        bool isVoiceActive() const override { return adsr.isActive(); }

        int getActiveGrainCount() const noexcept { return cloud.getActiveGrainCount(); }

        /** Message thread. Appends the wavetable sets this voice still holds, for
            WavetableStore::collectGarbage's pin list. */
        void getPinnedWavetableSets (std::vector<const WavetableSet*>& out) const
        {
            tone.getPinnedSets (out);
        }

        ToneEngine& getToneEngineForTests() noexcept { return tone; }

    private:
        void updateEnvelopeParameters();
        double computePlaybackRatio() const;

        const params::Handles& params;
        const SourceStore& sources;
        const WavetableStore& wavetables;

        GrainEngine cloud;
        ToneEngine tone;
        juce::ADSR adsr;
        juce::ADSR::Parameters adsrParams;

        juce::AudioBuffer<float> voiceBuffer; // pre-allocated in setCurrentPlaybackSampleRate
        juce::AudioBuffer<float> toneBuffer;  // mono Tone scratch, same capacity
        juce::LinearSmoothedValue<float> cloudGain, toneGain; // equal-power focus blend

        int noteNumber = 60;
        float noteVelocity = 1.0f;
        double pitchWheelSemitones = 0.0;
    };
}
