#pragma once

#include "GrainEngine.h"
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

        For M2 this owns a Cloud (granular) unit and the amp envelope. The Tone
        wavetable unit, the SVF filter and ENV2 land here in M3/M4 - the render path
        is deliberately shaped so that Focus can crossfade two engine outputs into the
        same voice buffer without restructuring this class.
    */
    class KeepsakeVoice : public juce::SynthesiserVoice
    {
    public:
        KeepsakeVoice (const params::Handles& handles, const SourceStore& store, int voiceIndex);

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

    private:
        void updateEnvelopeParameters();
        double computePlaybackRatio() const;

        const params::Handles& params;
        const SourceStore& sources;

        GrainEngine cloud;
        juce::ADSR adsr;
        juce::ADSR::Parameters adsrParams;

        juce::AudioBuffer<float> voiceBuffer; // pre-allocated in setCurrentPlaybackSampleRate

        int noteNumber = 60;
        float noteVelocity = 1.0f;
        double pitchWheelSemitones = 0.0;
    };
}
