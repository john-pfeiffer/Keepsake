#include "CaptureBuffer.h"

#include <algorithm>

namespace keepsake
{
    // =========================================================================
    // SourceStore
    // =========================================================================

    void SourceStore::publish (SourceAudio::Ptr newSource)
    {
        // Must not be called from the audio thread - it takes a lock and may free.
        // Hosts do call setStateInformation off the message thread, so this is
        // deliberately not a message-thread assertion.
        const juce::ScopedLock sl (messageThreadLock);

        if (live != nullptr)
            retired.push_back ({ live, juce::Time::getMillisecondCounterHiRes() * 0.001 });

        live = newSource;
        current.store (live.get(), std::memory_order_release);
    }

    void SourceStore::collectGarbage()
    {
        const juce::ScopedLock sl (messageThreadLock);

        const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;

        for (int i = (int) retired.size(); --i >= 0;)
            if (now - retired[(size_t) i].retiredAtSeconds > kRetainSeconds)
                retired.erase (retired.begin() + i);
    }

    SourceAudio::Ptr SourceStore::getForMessageThread() const
    {
        const juce::ScopedLock sl (messageThreadLock);
        return live;
    }

    // =========================================================================
    // WavetableStore
    // =========================================================================

    void WavetableStore::publish (WavetableSet::Ptr newSet)
    {
        // Callable from the extraction worker as well as the message thread -
        // anything except the audio thread (takes a lock, may free).
        const juce::ScopedLock sl (messageThreadLock);

        if (live != nullptr)
            retired.push_back ({ live, juce::Time::getMillisecondCounterHiRes() * 0.001 });

        live = newSet;
        current.store (live.get(), std::memory_order_release);
    }

    void WavetableStore::collectGarbage (const std::vector<const WavetableSet*>& pinnedByVoices)
    {
        const juce::ScopedLock sl (messageThreadLock);

        const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;

        for (int i = (int) retired.size(); --i >= 0;)
        {
            const auto& r = retired[(size_t) i];

            if (now - r.retiredAtSeconds <= kRetainSeconds)
                continue;

            // The extension over SourceStore: a set held by a voice (mid-crossfade,
            // or as a note's current table) stays alive however old it is. Without
            // this, a host suspending processing mid-note lets wall-clock time pass
            // with no audio callbacks, and the free races the eventual resume.
            if (std::find (pinnedByVoices.begin(), pinnedByVoices.end(), r.set.get())
                != pinnedByVoices.end())
                continue;

            retired.erase (retired.begin() + i);
        }
    }

    WavetableSet::Ptr WavetableStore::getForMessageThread() const
    {
        const juce::ScopedLock sl (messageThreadLock);
        return live;
    }

    // =========================================================================
    // CaptureIO
    // =========================================================================

    CaptureIO::CaptureIO()
    {
        formatManager.registerBasicFormats(); // WAV, AIFF, FLAC, Ogg, MP3
    }

    namespace
    {
        /** Reads a whole reader into a stereo buffer at the reader's own rate. */
        bool readAll (juce::AudioFormatReader& reader, juce::AudioBuffer<float>& dest)
        {
            const auto numSamples = (int) juce::jmin ((juce::int64) std::numeric_limits<int>::max(),
                                                      reader.lengthInSamples);

            if (numSamples <= 0)
                return false;

            juce::AudioBuffer<float> raw ((int) juce::jmax (1u, reader.numChannels), numSamples);
            reader.read (&raw, 0, numSamples, 0, true, true);

            dest.setSize (2, numSamples, false, true, false);

            if (raw.getNumChannels() == 1)
            {
                dest.copyFrom (0, 0, raw, 0, 0, numSamples);
                dest.copyFrom (1, 0, raw, 0, 0, numSamples);
            }
            else
            {
                dest.copyFrom (0, 0, raw, 0, 0, numSamples);
                dest.copyFrom (1, 0, raw, 1, 0, numSamples);
            }

            return true;
        }

        /** Offline, quality-first resample. Import is not real time, so cost is fine. */
        void resample (juce::AudioBuffer<float>& buffer, double fromRate, double toRate)
        {
            if (fromRate <= 0.0 || toRate <= 0.0 || std::abs (fromRate - toRate) < 1.0e-6)
                return;

            const auto ratio = fromRate / toRate;
            const auto outLength = (int) std::ceil ((double) buffer.getNumSamples() / ratio);

            juce::AudioBuffer<float> out (buffer.getNumChannels(), outLength);
            out.clear();

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                juce::LagrangeInterpolator interpolator;
                interpolator.reset();
                interpolator.process (ratio,
                                      buffer.getReadPointer (ch),
                                      out.getWritePointer (ch),
                                      outLength,
                                      buffer.getNumSamples(),
                                      0);
            }

            buffer = std::move (out);
        }

        /** Spec §2.1: keep a 60s region centred on the capture point, and say so. */
        bool trimToCap (juce::AudioBuffer<float>& buffer, double sampleRate, double placeNormalised)
        {
            const auto maxSamples = (int) (CaptureIO::kMaxEmbeddedSeconds * sampleRate);

            if (buffer.getNumSamples() <= maxSamples)
                return false;

            const auto centre = (int) (juce::jlimit (0.0, 1.0, placeNormalised)
                                       * (double) buffer.getNumSamples());
            auto start = juce::jlimit (0, buffer.getNumSamples() - maxSamples, centre - maxSamples / 2);

            juce::AudioBuffer<float> trimmed (buffer.getNumChannels(), maxSamples);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                trimmed.copyFrom (ch, 0, buffer, ch, start, maxSamples);

            buffer = std::move (trimmed);
            return true;
        }
    }

    CaptureIO::LoadResult CaptureIO::loadFile (const juce::File& file,
                                               double targetSampleRate,
                                               double placeNormalised)
    {
        LoadResult result;

        if (! file.existsAsFile())
        {
            result.error = "File not found: " + file.getFullPathName();
            return result;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

        if (reader == nullptr)
        {
            result.error = "Unsupported or unreadable audio file: " + file.getFileName();
            return result;
        }

        auto source = new SourceAudio();
        source->name = file.getFileName();

        if (! readAll (*reader, source->buffer))
        {
            delete source;
            result.error = "Audio file is empty: " + file.getFileName();
            return result;
        }

        resample (source->buffer, reader->sampleRate, targetSampleRate);
        source->sampleRate = targetSampleRate;
        result.trimmed = trimToCap (source->buffer, targetSampleRate, placeNormalised);
        source->transients = detectTransients (source->buffer, targetSampleRate);
        result.source = source;

        return result;
    }

    CaptureIO::LoadResult CaptureIO::loadStream (std::unique_ptr<juce::InputStream> stream,
                                                 const juce::String& name,
                                                 double targetSampleRate,
                                                 double placeNormalised)
    {
        LoadResult result;

        if (stream == nullptr)
        {
            result.error = "No data to read";
            return result;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (
            formatManager.createReaderFor (std::move (stream)));

        if (reader == nullptr)
        {
            result.error = "Unsupported or unreadable audio data";
            return result;
        }

        auto source = new SourceAudio();
        source->name = name;

        if (! readAll (*reader, source->buffer))
        {
            delete source;
            result.error = "Audio data is empty";
            return result;
        }

        resample (source->buffer, reader->sampleRate, targetSampleRate);
        source->sampleRate = targetSampleRate;
        result.trimmed = trimToCap (source->buffer, targetSampleRate, placeNormalised);
        source->transients = detectTransients (source->buffer, targetSampleRate);
        result.source = source;

        return result;
    }

    juce::String CaptureIO::encodeToBase64 (const SourceAudio& source)
    {
        if (source.getNumSamples() <= 0)
            return {};

        juce::MemoryBlock block;

        {
            juce::FlacAudioFormat flac;
            auto stream = std::make_unique<juce::MemoryOutputStream> (block, false);

            std::unique_ptr<juce::AudioFormatWriter> writer (
                flac.createWriterFor (stream.get(),
                                      source.sampleRate,
                                      (unsigned int) source.getNumChannels(),
                                      24,
                                      {},
                                      5));

            if (writer == nullptr)
                return {};

            stream.release(); // the writer owns it now
            writer->writeFromAudioSampleBuffer (source.buffer, 0, source.getNumSamples());
        }

        return block.toBase64Encoding();
    }

    SourceAudio::Ptr CaptureIO::decodeFromBase64 (const juce::String& base64,
                                                  const juce::String& name,
                                                  double storedSampleRate,
                                                  double targetSampleRate)
    {
        if (base64.isEmpty())
            return {};

        juce::MemoryBlock block;

        if (! block.fromBase64Encoding (base64) || block.getSize() == 0)
            return {};

        auto stream = std::make_unique<juce::MemoryInputStream> (block, true);
        std::unique_ptr<juce::AudioFormatReader> reader (
            formatManager.createReaderFor (std::move (stream)));

        if (reader == nullptr)
            return {};

        SourceAudio::Ptr source (new SourceAudio());
        source->name = name;

        if (! readAll (*reader, source->buffer))
            return {};

        // The blob carries its own rate; trust the reader but fall back to the
        // stored value if the codec did not report one.
        const auto sourceRate = reader->sampleRate > 0.0 ? reader->sampleRate : storedSampleRate;
        resample (source->buffer, sourceRate, targetSampleRate);
        source->sampleRate = targetSampleRate;
        source->transients = detectTransients (source->buffer, targetSampleRate);

        return source;
    }

    // =========================================================================
    // detectTransients
    // =========================================================================

    std::vector<int> detectTransients (const juce::AudioBuffer<float>& buffer,
                                       double sampleRate)
    {
        std::vector<int> onsets;

        const auto numSamples = buffer.getNumSamples();
        const auto numChannels = buffer.getNumChannels();

        if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0)
            return onsets;

        // Per-hop mono energy. 256 samples is ~5ms at 48k - fine enough that a
        // drum hit lands within one grain-attack of its true position.
        constexpr int kHop = 256;
        const auto numHops = (numSamples + kHop - 1) / kHop;

        std::vector<double> energy ((size_t) numHops, 0.0);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* data = buffer.getReadPointer (ch);

            for (int i = 0; i < numSamples; ++i)
                energy[(size_t) (i / kHop)] += (double) data[i] * (double) data[i];
        }

        double peakEnergy = 0.0;

        for (auto e : energy)
            peakEnergy = juce::jmax (peakEnergy, e);

        if (peakEnergy <= 0.0)
            return onsets;

        // One-pole local average (~100ms) as the reference level; an onset is a
        // hop that jumps well above it. The relative floor keeps room-noise
        // wobble in near-silence from registering as hits.
        const auto averageCoeff = std::exp (-(double) kHop / (0.1 * sampleRate));
        const auto refractoryHops = juce::jmax (1, (int) std::ceil (0.04 * sampleRate / kHop));
        constexpr double kRiseRatio = 2.5;
        const auto floorEnergy = 1.0e-4 * peakEnergy;

        auto localAverage = energy[0];
        auto hopsSinceOnset = refractoryHops; // the very first hop may fire

        for (int h = 0; h < numHops; ++h)
        {
            const auto e = energy[(size_t) h];
            const auto isOnset = hopsSinceOnset >= refractoryHops
                              && e > floorEnergy
                              && e > kRiseRatio * juce::jmax (localAverage, 1.0e-12);

            if (isOnset)
            {
                onsets.push_back (h * kHop);
                hopsSinceOnset = 0;
            }
            else
            {
                ++hopsSinceOnset;
            }

            localAverage = averageCoeff * localAverage + (1.0 - averageCoeff) * e;
        }

        return onsets;
    }

    // =========================================================================
    // CaptureWindow
    // =========================================================================

    CaptureWindow CaptureWindow::resolve (const SourceAudio& source,
                                          double place,
                                          double lengthMs,
                                          double sampleRate) noexcept
    {
        CaptureWindow w;

        const auto total = source.getNumSamples();

        if (total <= 0 || sampleRate <= 0.0)
            return w;

        auto length = (int) std::round (lengthMs * 0.001 * sampleRate);
        length = juce::jlimit (1, total, length);

        // Place walks the *start* of the window from 0 to (end of file - window),
        // so the frame never runs off the end however long the window is.
        const auto maxStart = total - length;
        auto start = (int) std::round (juce::jlimit (0.0, 1.0, place) * (double) maxStart);

        w.startSample = juce::jlimit (0, maxStart, start);
        w.numSamples = length;

        return w;
    }
}
