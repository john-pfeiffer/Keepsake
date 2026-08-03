#pragma once

#include "Parameters.h"
#include "engine/CaptureBuffer.h"
#include "engine/Voice.h"

namespace keepsake
{
    class KeepsakeProcessor : public juce::AudioProcessor,
                              private juce::Timer
    {
    public:
        static constexpr int kNumVoices = 12; // spec §2.5

        /** 'KPSK' - guards setStateInformation against arbitrary bytes. */
        static constexpr juce::uint32 kStateMagic = 0x4b50534b;
        static constexpr int kStateVersion = 1;

        KeepsakeProcessor();
        ~KeepsakeProcessor() override;

        // --- AudioProcessor -------------------------------------------------
        void prepareToPlay (double sampleRate, int samplesPerBlock) override;
        void releaseResources() override;
        bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

        juce::AudioProcessorEditor* createEditor() override;
        bool hasEditor() const override { return true; }

        const juce::String getName() const override { return JucePlugin_Name; }
        bool acceptsMidi() const override { return true; }
        bool producesMidi() const override { return false; }
        bool isMidiEffect() const override { return false; }
        double getTailLengthSeconds() const override;

        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return "Default"; }
        void changeProgramName (int, const juce::String&) override {}

        void getStateInformation (juce::MemoryBlock& destData) override;
        void setStateInformation (const void* data, int sizeInBytes) override;

        // --- Keepsake -------------------------------------------------------
        juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }
        const SourceStore& getSourceStore() const noexcept { return sourceStore; }

        struct ImportResult
        {
            bool ok = false;
            bool trimmed = false;
            juce::String message;
        };

        /** Message thread. Imports a file and publishes it to the audio thread. */
        ImportResult importFile (const juce::File& file);

        /** Broadcasts when the loaded source changes (import, or state restore). */
        juce::ChangeBroadcaster sourceChanged;

        /** Audition transport - a momentary action, not an automatable control. */
        enum class Audition { off, source, window };

        void startAudition (Audition mode);
        void stopAudition();
        Audition getAuditionMode() const noexcept { return auditionMode.load(); }

        /** 0..1 through the file, or -1 when not auditioning. For the playhead. */
        double getAuditionPositionNormalised() const noexcept;

        int getActiveVoiceCount() const noexcept;

    private:
        void timerCallback() override;
        void renderAudition (juce::AudioBuffer<float>& buffer);
        void publishSource (SourceAudio::Ptr source, const juce::String& sourceName);

        juce::AudioProcessorValueTreeState apvts;
        params::Handles handles;

        CaptureIO captureIO;
        SourceStore sourceStore;

        juce::Synthesiser synth;
        juce::LinearSmoothedValue<float> outputGain;

        // Audition state, all touched from both threads via atomics only.
        std::atomic<Audition> auditionMode { Audition::off };
        std::atomic<int64_t> auditionPosition { 0 };
        std::atomic<float> auditionFade { 0.0f };

        // Serialised alongside the parameters so a preset carries its keepsake.
        juce::String embeddedAudioBase64;
        juce::String embeddedAudioName;
        double embeddedAudioRate = 0.0;
        bool embeddedAudioTrimmed = false;

        double currentSampleRate = 44100.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeepsakeProcessor)
    };
}
