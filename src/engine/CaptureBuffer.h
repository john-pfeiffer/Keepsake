#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace keepsake
{
    /**
        One immutable, host-sample-rate copy of an imported source file.

        Reference counted so that the audio thread can hold a raw pointer to it while
        the message thread swaps in a replacement. See SourceStore for the handover
        rules - nothing here is mutated after publication.
    */
    class SourceAudio : public juce::ReferenceCountedObject
    {
    public:
        using Ptr = juce::ReferenceCountedObjectPtr<SourceAudio>;

        juce::AudioBuffer<float> buffer;
        double sampleRate = 44100.0;
        juce::String name;

        int getNumSamples() const noexcept  { return buffer.getNumSamples(); }
        int getNumChannels() const noexcept { return buffer.getNumChannels(); }
        double getLengthSeconds() const noexcept
        {
            return sampleRate > 0.0 ? (double) buffer.getNumSamples() / sampleRate : 0.0;
        }
    };

    /**
        Publishes a SourceAudio to the audio thread without locks or allocation on the
        audio side.

        The audio thread reads a plain atomic pointer (getForAudioThread()). Replaced
        sources are parked in a retain list and only destroyed once they have been
        unpublished for longer than kRetainSeconds, which is many orders of magnitude
        longer than an audio callback can take. That deferral is what makes the raw
        pointer safe; do not shorten it to "free as soon as the refcount drops".
    */
    class SourceStore
    {
    public:
        static constexpr double kRetainSeconds = 2.0;

        /** Message thread only. Publishes a new source and retires the previous one. */
        void publish (SourceAudio::Ptr newSource);

        /** Message thread only. Frees sources that have been retired long enough. */
        void collectGarbage();

        /** Audio thread. May return nullptr; the pointer stays valid for this callback. */
        SourceAudio* getForAudioThread() const noexcept { return current.load (std::memory_order_acquire); }

        /** Message thread. Reference-counted handle for UI drawing. */
        SourceAudio::Ptr getForMessageThread() const;

        bool hasSource() const noexcept { return current.load (std::memory_order_acquire) != nullptr; }

    private:
        struct Retired
        {
            SourceAudio::Ptr source;
            double retiredAtSeconds = 0.0;
        };

        std::atomic<SourceAudio*> current { nullptr };
        SourceAudio::Ptr live;               // keeps *current alive
        std::vector<Retired> retired;
        juce::CriticalSection messageThreadLock; // guards live/retired, never taken on audio thread
    };

    /**
        One immutable set of mipmapped wavetable frames extracted from a capture
        window. Built by Analysis on a background thread; never mutated after
        publication (numFrames lives in here so a Frames-count change is just
        another set swap, with no special case in the oscillator).
    */
    class WavetableSet : public juce::ReferenceCountedObject
    {
    public:
        using Ptr = juce::ReferenceCountedObjectPtr<WavetableSet>;

        static constexpr int kFrameSize = 2048;
        /** Levels 0..10 keep 1024 >> L harmonics; level 10 is a pure fundamental. */
        static constexpr int kNumMipLevels = 11;

        int numFrames = 0;
        double sampleRate = 44100.0;
        double rootHz = 0.0;

        /** Layout: [frame][mipLevel][sample], contiguous. */
        std::vector<float> data;

        const float* getTable (int frame, int mipLevel) const noexcept
        {
            return data.data()
                   + ((size_t) frame * kNumMipLevels + (size_t) mipLevel) * kFrameSize;
        }
    };

    /**
        Publishes a WavetableSet to the audio thread. Mirrors SourceStore's
        atomic-pointer + deferred-release pattern, with one deliberate extension:

        Voices hold raw set pointers ACROSS blocks - for the ~30ms swap crossfade and
        for the whole lifetime of a note - not just within one callback. If the host
        suspends processing (transport stop, bypass) mid-note, wall-clock retention
        alone would free a set a voice still holds. collectGarbage() therefore takes
        the list of pointers currently pinned by voices and skips them regardless of
        age. Callers must gather that list every time; passing an empty list when
        voices exist reintroduces the use-after-free.
    */
    class WavetableStore
    {
    public:
        static constexpr double kRetainSeconds = 2.0;

        /** Any non-audio thread. Publishes a new set (may be nullptr = no keepsake). */
        void publish (WavetableSet::Ptr newSet);

        /** Message thread. Frees retired sets that are old enough AND not pinned. */
        void collectGarbage (const std::vector<const WavetableSet*>& pinnedByVoices);

        /** Audio thread. May return nullptr. See class comment for lifetime rules. */
        WavetableSet* getForAudioThread() const noexcept
        {
            return current.load (std::memory_order_acquire);
        }

        WavetableSet::Ptr getForMessageThread() const;

    private:
        struct Retired
        {
            WavetableSet::Ptr set;
            double retiredAtSeconds = 0.0;
        };

        std::atomic<WavetableSet*> current { nullptr };
        WavetableSet::Ptr live;
        std::vector<Retired> retired;
        juce::CriticalSection messageThreadLock;
    };

    /**
        File import, and serialisation of the imported audio into plugin state.

        Spec §2.1: the full source file is embedded in the preset so a keepsake survives
        the source file moving, capped at 60s stereo. Past the cap we keep a 60s region
        centred on the current capture point and flag that the file was trimmed.
    */
    class CaptureIO
    {
    public:
        static constexpr double kMaxEmbeddedSeconds = 60.0;

        struct LoadResult
        {
            SourceAudio::Ptr source;
            bool trimmed = false;
            juce::String error;

            bool ok() const noexcept { return source != nullptr && error.isEmpty(); }
        };

        CaptureIO();

        /** Message/background thread. Reads, mono-or-stereo, resamples to targetSampleRate. */
        LoadResult loadFile (const juce::File& file, double targetSampleRate, double placeNormalised = 0.0);

        LoadResult loadStream (std::unique_ptr<juce::InputStream> stream,
                               const juce::String& name,
                               double targetSampleRate,
                               double placeNormalised = 0.0);

        /** Encodes a source as FLAC, base64'd, for storage in a ValueTree property. */
        static juce::String encodeToBase64 (const SourceAudio& source);

        /** Inverse of encodeToBase64. Returns nullptr if the blob is unusable. */
        SourceAudio::Ptr decodeFromBase64 (const juce::String& base64,
                                           const juce::String& name,
                                           double storedSampleRate,
                                           double targetSampleRate);

        juce::AudioFormatManager& getFormatManager() noexcept { return formatManager; }

    private:
        juce::AudioFormatManager formatManager;
    };

    /** Resolves the capture window (in source samples) from Place and length. */
    struct CaptureWindow
    {
        int startSample = 0;
        int numSamples = 0;

        int endSample() const noexcept { return startSample + numSamples; }

        /** @param place       0..1 through the file
            @param lengthMs    requested window length
            The window is clamped so it always lies inside the source. */
        static CaptureWindow resolve (const SourceAudio& source,
                                      double place,
                                      double lengthMs,
                                      double sampleRate) noexcept;
    };
}
