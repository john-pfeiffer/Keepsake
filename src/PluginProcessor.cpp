#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace keepsake
{
    namespace ids
    {
        inline constexpr auto root       = "KEEPSAKE";
        inline constexpr auto audioData  = "audioData";
        inline constexpr auto audioName  = "audioName";
        inline constexpr auto audioRate  = "audioRate";
        inline constexpr auto audioTrimmed = "audioTrimmed";
    }

    KeepsakeProcessor::KeepsakeProcessor()
        : juce::AudioProcessor (BusesProperties()
                                    .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMETERS", params::createParameterLayout())
    {
        handles.attach (apvts);

        synth.addSound (new KeepsakeSound());

        for (int i = 0; i < kNumVoices; ++i)
            synth.addVoice (new KeepsakeVoice (handles, sourceStore, i));

        // Spec §2.5: oldest-note stealing.
        synth.setNoteStealingEnabled (true);

        startTimerHz (4); // deferred release of retired source buffers
    }

    KeepsakeProcessor::~KeepsakeProcessor()
    {
        stopTimer();
    }

    void KeepsakeProcessor::timerCallback()
    {
        sourceStore.collectGarbage();
    }

    // =========================================================================
    // Audio
    // =========================================================================

    void KeepsakeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
    {
        const auto rateChanged = std::abs (sampleRate - currentSampleRate) > 1.0e-6;
        currentSampleRate = sampleRate;

        // State can be restored before prepareToPlay, in which case the source was
        // decoded against a guessed rate. Re-decode so the source always sits at the
        // host rate and grain playback ratios stay honest.
        if (rateChanged && embeddedAudioBase64.isNotEmpty())
        {
            const auto existing = sourceStore.getForMessageThread();

            if (existing == nullptr || std::abs (existing->sampleRate - sampleRate) > 1.0e-6)
                publishSource (captureIO.decodeFromBase64 (embeddedAudioBase64,
                                                           embeddedAudioName,
                                                           embeddedAudioRate,
                                                           sampleRate),
                               embeddedAudioName);
        }

        synth.setCurrentPlaybackSampleRate (sampleRate);

        outputGain.reset (sampleRate, 0.02);
        outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (handles.masterGain->load()));

        juce::ignoreUnused (samplesPerBlock);
    }

    void KeepsakeProcessor::releaseResources()
    {
        synth.allNotesOff (0, false);
    }

    bool KeepsakeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
    {
        const auto& out = layouts.getMainOutputChannelSet();
        return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
    }

    double KeepsakeProcessor::getTailLengthSeconds() const
    {
        // Longest possible amp release, so hosts do not cut the tail on the last note.
        return 10.0;
    }

    void KeepsakeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        juce::ScopedNoDenormals noDenormals;

        buffer.clear();

        synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());

        renderAudition (buffer);

        outputGain.setTargetValue (juce::Decibels::decibelsToGain (handles.masterGain->load()));
        outputGain.applyGain (buffer, buffer.getNumSamples());

       #if JUCE_DEBUG
        // NaN scrubbing in debug builds (spec §6). A single NaN otherwise poisons the
        // whole host graph and makes the real cause impossible to find.
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                jassert (std::isfinite (d[i]));

                if (! std::isfinite (d[i]))
                    d[i] = 0.0f;
            }
        }
       #endif
    }

    void KeepsakeProcessor::renderAudition (juce::AudioBuffer<float>& buffer)
    {
        const auto mode = auditionMode.load();
        auto fade = auditionFade.load();

        if (mode == Audition::off && fade <= 0.0f)
            return;

        const auto* source = sourceStore.getForAudioThread();

        if (source == nullptr || source->getNumSamples() <= 0)
        {
            auditionFade.store (0.0f);
            return;
        }

        int regionStart = 0;
        int regionLength = source->getNumSamples();

        if (mode == Audition::window)
        {
            const auto w = CaptureWindow::resolve (*source,
                                                   (double) handles.place->load(),
                                                   (double) handles.captureLength->load(),
                                                   currentSampleRate);
            regionStart = w.startSample;
            regionLength = juce::jmax (1, w.numSamples);
        }

        auto pos = (int) auditionPosition.load();

        if (pos < regionStart || pos >= regionStart + regionLength)
            pos = regionStart;

        // ~5ms fade in/out so starting and stopping audition never clicks.
        const auto fadeStep = (float) (1.0 / (0.005 * currentSampleRate));
        const auto target = mode == Audition::off ? 0.0f : 1.0f;

        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            fade = target > fade ? juce::jmin (target, fade + fadeStep)
                                 : juce::jmax (target, fade - fadeStep);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto* src = source->buffer.getReadPointer (juce::jmin (ch, source->getNumChannels() - 1));
                buffer.getWritePointer (ch)[n] += src[pos] * fade * 0.8f;
            }

            if (++pos >= regionStart + regionLength)
                pos = regionStart; // audition loops its region
        }

        auditionPosition.store (pos);
        auditionFade.store (fade);

        if (mode == Audition::off && fade <= 0.0f)
            auditionPosition.store (0);
    }

    void KeepsakeProcessor::startAudition (Audition mode)
    {
        auditionPosition.store (0);
        auditionMode.store (mode);
    }

    void KeepsakeProcessor::stopAudition()
    {
        auditionMode.store (Audition::off);
    }

    double KeepsakeProcessor::getAuditionPositionNormalised() const noexcept
    {
        if (auditionMode.load() == Audition::off)
            return -1.0;

        const auto source = sourceStore.getForMessageThread();

        if (source == nullptr || source->getNumSamples() <= 0)
            return -1.0;

        return juce::jlimit (0.0, 1.0,
                             (double) auditionPosition.load() / (double) source->getNumSamples());
    }

    int KeepsakeProcessor::getActiveVoiceCount() const noexcept
    {
        int count = 0;

        for (int i = 0; i < synth.getNumVoices(); ++i)
            if (synth.getVoice (i)->isVoiceActive())
                ++count;

        return count;
    }

    // =========================================================================
    // Import & state
    // =========================================================================

    void KeepsakeProcessor::publishSource (SourceAudio::Ptr source, const juce::String& sourceName)
    {
        embeddedAudioName = sourceName;
        embeddedAudioRate = source != nullptr ? source->sampleRate : 0.0;
        sourceStore.publish (source);
        sourceChanged.sendChangeMessage();
    }

    KeepsakeProcessor::ImportResult KeepsakeProcessor::importFile (const juce::File& file)
    {
        ImportResult result;

        const auto place = (double) handles.place->load();
        auto loaded = captureIO.loadFile (file, currentSampleRate, place);

        if (! loaded.ok())
        {
            result.message = loaded.error;
            return result;
        }

        // Encode once, here, rather than on every getStateInformation call - hosts
        // poll state far more often than the user imports a file.
        embeddedAudioBase64 = CaptureIO::encodeToBase64 (*loaded.source);
        embeddedAudioTrimmed = loaded.trimmed;

        publishSource (loaded.source, file.getFileName());

        result.ok = true;
        result.trimmed = loaded.trimmed;
        result.message = loaded.trimmed
                           ? "Loaded (trimmed to 60s around the capture point)"
                           : "Loaded " + file.getFileName();

        return result;
    }

    void KeepsakeProcessor::getStateInformation (juce::MemoryBlock& destData)
    {
        auto state = apvts.copyState();

        // Spec §2.1: the keepsake travels with the preset, so the user never loses it
        // to a moved source file.
        state.setProperty (ids::audioData, embeddedAudioBase64, nullptr);
        state.setProperty (ids::audioName, embeddedAudioName, nullptr);
        state.setProperty (ids::audioRate, embeddedAudioRate, nullptr);
        state.setProperty (ids::audioTrimmed, embeddedAudioTrimmed, nullptr);

        // Binary rather than XML: the embedded audio is a multi-megabyte base64 blob,
        // and XML-escaping it on every host state poll is pure overhead. Binary also
        // keeps setStateInformation off the String-from-8-bit-data path, which asserts
        // when a host (or pluginval) hands us arbitrary bytes.
        juce::MemoryOutputStream stream (destData, false);

        // Header: magic + version + payload length. ValueTree::readFromData asserts
        // (loudly, and it breaks under a debugger) on corrupt or truncated input, so
        // we validate before handing it anything. The version field is also what lets
        // a future format change reject or migrate old presets instead of guessing.
        stream.writeInt ((int) kStateMagic);
        stream.writeInt (kStateVersion);

        const auto lengthPosition = stream.getPosition();
        stream.writeInt (0); // placeholder, patched below

        const auto payloadStart = stream.getPosition();
        state.writeToStream (stream);
        const auto payloadLength = (int) (stream.getPosition() - payloadStart);

        stream.setPosition (lengthPosition);
        stream.writeInt (payloadLength);
        stream.setPosition ((juce::int64) stream.getDataSize());
    }

    void KeepsakeProcessor::setStateInformation (const void* data, int sizeInBytes)
    {
        constexpr int headerBytes = 12;

        if (data == nullptr || sizeInBytes <= headerBytes)
            return;

        juce::MemoryInputStream header (data, (size_t) sizeInBytes, false);

        if ((juce::uint32) header.readInt() != kStateMagic)
            return;

        if (header.readInt() != kStateVersion)
            return; // a newer preset than this build understands

        const auto payloadLength = header.readInt();

        // Truncated (or overlong) payload: refuse rather than parse a partial tree.
        if (payloadLength <= 0 || payloadLength != sizeInBytes - headerBytes)
            return;

        auto tree = juce::ValueTree::readFromData (
            static_cast<const char*> (data) + headerBytes, (size_t) payloadLength);

        if (! tree.isValid() || tree.getType() != apvts.state.getType())
            return;

        embeddedAudioBase64  = tree.getProperty (ids::audioData, juce::String()).toString();
        embeddedAudioName    = tree.getProperty (ids::audioName, juce::String()).toString();
        embeddedAudioRate    = (double) tree.getProperty (ids::audioRate, 0.0);
        embeddedAudioTrimmed = (bool) tree.getProperty (ids::audioTrimmed, false);

        // The audio blob is not a parameter; strip it before handing the tree back to
        // the APVTS so it does not end up in the parameter state.
        tree.removeProperty (ids::audioData, nullptr);
        tree.removeProperty (ids::audioName, nullptr);
        tree.removeProperty (ids::audioRate, nullptr);
        tree.removeProperty (ids::audioTrimmed, nullptr);

        apvts.replaceState (tree);

        auto restored = captureIO.decodeFromBase64 (embeddedAudioBase64,
                                                    embeddedAudioName,
                                                    embeddedAudioRate,
                                                    currentSampleRate);

        publishSource (restored, embeddedAudioName);
    }

    juce::AudioProcessorEditor* KeepsakeProcessor::createEditor()
    {
        return new KeepsakeEditor (*this);
    }
}

// The host's entry point.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new keepsake::KeepsakeProcessor();
}
