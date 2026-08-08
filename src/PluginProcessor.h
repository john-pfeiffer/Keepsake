#pragma once

#include "Parameters.h"
#include "engine/CaptureBuffer.h"
#include "engine/Voice.h"

#include <optional>

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
        const WavetableStore& getWavetableStore() const noexcept { return wavetableStore; }

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

        /** Runs wavetable extraction synchronously on the calling thread and
            publishes the result. The async worker runs exactly the same code; this
            seam exists so tests (where no Timer ever fires) drive extraction
            deterministically. Safe from any non-audio thread. */
        void extractNow();

        KeepsakeVoice* getVoiceForTests (int index) noexcept
        {
            return dynamic_cast<KeepsakeVoice*> (synth.getVoice (index));
        }

    private:
        /** What one extraction run needs, snapshotted so the worker never touches
            live parameters mid-build. */
        struct ExtractionRequest
        {
            SourceAudio::Ptr source;
            double place = 0.0;
            double captureLengthMs = 0.0;
            double f0 = 0.0;
            int numFrames = 8;
        };

        /**
            The background extraction worker (spec §2.1: re-extraction "can't run
            per-sample"; §2.3: "never audio thread").

            One mutex-protected latest-request slot + a WaitableEvent: requesters
            overwrite the slot and signal; the worker copies it, builds, publishes,
            and re-checks. Coalescing is automatic - only the newest request ever
            runs. A dedicated thread rather than the message thread because hosts
            freeze the message thread (saves, UI storms), and rather than a
            ThreadPool because there is exactly one job type per instance.
        */
        class ExtractionWorker : public juce::Thread
        {
        public:
            explicit ExtractionWorker (KeepsakeProcessor& p)
                : juce::Thread ("Keepsake Extraction"), owner (p) {}

            void enqueue (ExtractionRequest&& request);
            void run() override;

        private:
            KeepsakeProcessor& owner;
            juce::CriticalSection slotLock;
            std::optional<ExtractionRequest> slot;
        };

        /** 50Hz poller: watches the extraction-relevant values and fires the worker
            with a trailing debounce (~80ms of quiet) PLUS a throttle (~200ms) so
            sustained host automation of Place still re-extracts periodically - a
            pure debounce would never fire under an LFO and Tone would freeze
            stale. Separate from the 4Hz GC timer, which is far too coarse. */
        class ExtractionPoller : public juce::Timer
        {
        public:
            explicit ExtractionPoller (KeepsakeProcessor& p) : owner (p) {}
            void timerCallback() override;

        private:
            KeepsakeProcessor& owner;
            double lastValues[5] { -1.0, -1.0, -1.0, -1.0, -1.0 };
            juce::uint64 lastSourceGeneration = 0;
            double lastChangeMs = 0.0;
            double lastRequestMs = 0.0;
            bool dirty = false;
            bool first = true;
        };

        std::optional<ExtractionRequest> makeExtractionRequest();
        void runExtraction (const ExtractionRequest& request);
        void timerCallback() override;
        void renderAudition (juce::AudioBuffer<float>& buffer);
        void publishSource (SourceAudio::Ptr source, const juce::String& sourceName);

        juce::AudioProcessorValueTreeState apvts;
        params::Handles handles;

        CaptureIO captureIO;
        SourceStore sourceStore;
        WavetableStore wavetableStore;
        std::atomic<juce::uint64> sourceGeneration { 0 };
        std::vector<const WavetableSet*> pinnedScratch; // message-thread GC scratch

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

        // Declared LAST so they are destroyed FIRST: the worker must be joined and
        // the poller stopped before the stores they touch are torn down.
        ExtractionPoller extractionPoller { *this };
        ExtractionWorker extractionWorker { *this };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeepsakeProcessor)
    };
}
