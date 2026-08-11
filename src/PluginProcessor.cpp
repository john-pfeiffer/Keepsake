#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "engine/Analysis.h"

namespace keepsake
{
    namespace ids
    {
        inline constexpr auto root       = "KEEPSAKE";
        inline constexpr auto audioData  = "audioData";
        inline constexpr auto audioName  = "audioName";
        inline constexpr auto audioRate  = "audioRate";
        inline constexpr auto audioTrimmed = "audioTrimmed";
        inline constexpr auto presetName   = "presetName";
    }

    // =========================================================================
    // KeepsakeSynth: mono/legato note handling
    // =========================================================================

    void KeepsakeProcessor::KeepsakeSynth::noteOn (int midiChannel, int midiNoteNumber,
                                                   float velocity)
    {
        const auto mode = (int) std::lround (handles.voiceMode->load());

        if (mode == 0) // Poly
        {
            juce::Synthesiser::noteOn (midiChannel, midiNoteNumber, velocity);
            return;
        }

        const juce::ScopedLock sl (lock);

        // Push (unique) onto the held stack; newest is the sounding note.
        for (int i = 0; i < heldCount; ++i)
        {
            if (held[(size_t) i].note == midiNoteNumber)
            {
                for (int j = i; j < heldCount - 1; ++j)
                    held[(size_t) j] = held[(size_t) j + 1];
                --heldCount;
                break;
            }
        }

        if (heldCount < (int) held.size())
            held[(size_t) heldCount++] = { midiNoteNumber, velocity };

        auto* voice = monoVoice();

        if (voice == nullptr)
            return;

        if (! voice->isVoiceActive())
        {
            if (auto* sound = getSound (0).get())
                startVoice (voice, sound, midiChannel, midiNoteNumber, velocity);
        }
        else
        {
            // Mono retriggers the envelopes; Legato lets them keep running.
            voice->changeNote (midiNoteNumber, velocity, mode == 1);
        }
    }

    void KeepsakeProcessor::KeepsakeSynth::noteOff (int midiChannel, int midiNoteNumber,
                                                    float velocity, bool allowTailOff)
    {
        const auto mode = (int) std::lround (handles.voiceMode->load());

        if (mode == 0) // Poly
        {
            juce::Synthesiser::noteOff (midiChannel, midiNoteNumber, velocity, allowTailOff);
            return;
        }

        const juce::ScopedLock sl (lock);

        const auto wasSounding = heldCount > 0
                                 && held[(size_t) (heldCount - 1)].note == midiNoteNumber;

        for (int i = 0; i < heldCount; ++i)
        {
            if (held[(size_t) i].note == midiNoteNumber)
            {
                for (int j = i; j < heldCount - 1; ++j)
                    held[(size_t) j] = held[(size_t) j + 1];
                --heldCount;
                break;
            }
        }

        auto* voice = monoVoice();

        if (voice == nullptr || ! wasSounding)
            return;

        if (heldCount > 0)
        {
            // Return to the most recent still-held note. Classic mono retriggers
            // on the way back down too; Legato glides without retrigger.
            const auto& previous = held[(size_t) (heldCount - 1)];
            voice->changeNote (previous.note, previous.velocity, mode == 1);
        }
        else
        {
            voice->stopNote (velocity, allowTailOff);
        }
    }

    KeepsakeProcessor::KeepsakeProcessor()
        : juce::AudioProcessor (BusesProperties()
                                    .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMETERS", params::createParameterLayout())
    {
        handles.attach (apvts);

        synth.addSound (new KeepsakeSound());

        for (int i = 0; i < kNumVoices; ++i)
            synth.addVoice (new KeepsakeVoice (handles, sourceStore, wavetableStore, blockContext, i));

        // The AudioBuffer object's address is stable across setSize, so the
        // voices can hold the pointer from construction onward.
        for (int i = 0; i < synth.getNumVoices(); ++i)
            if (auto* voice = dynamic_cast<KeepsakeVoice*> (synth.getVoice (i)))
                voice->setWetSendBus (&wetSendBus);

        // Spec §2.5: oldest-note stealing.
        synth.setNoteStealingEnabled (true);

        pinnedScratch.reserve (kNumVoices * 3);

        startTimerHz (4);                 // deferred release of retired buffers
        extractionPoller.startTimerHz (50); // debounce needs ~20ms granularity, 4Hz can't
        extractionWorker.startThread();
    }

    KeepsakeProcessor::~KeepsakeProcessor()
    {
        // Order matters: stop everything that can touch the stores before the
        // stores (declared earlier, destroyed later) go away.
        extractionPoller.stopTimer();
        stopTimer();
        extractionWorker.stopThread (2000); // signals exit + notifies the wait()
    }

    void KeepsakeProcessor::timerCallback()
    {
        sourceStore.collectGarbage();

        // Wavetable GC must know which sets voices still hold (mid-crossfade or as
        // a note's current table): a host that suspends processing mid-note lets
        // wall-clock retention expire while the pointer is still live.
        pinnedScratch.clear();

        for (int i = 0; i < synth.getNumVoices(); ++i)
            if (auto* voice = dynamic_cast<KeepsakeVoice*> (synth.getVoice (i)))
                voice->getPinnedWavetableSets (pinnedScratch);

        wavetableStore.collectGarbage (pinnedScratch);
    }

    // =========================================================================
    // Wavetable extraction
    // =========================================================================

    std::optional<KeepsakeProcessor::ExtractionRequest> KeepsakeProcessor::makeExtractionRequest()
    {
        ExtractionRequest request;
        request.source = sourceStore.getForMessageThread();

        if (request.source == nullptr)
            return std::nullopt;

        request.place = (double) handles.place->load();
        request.captureLengthMs = (double) handles.captureLength->load();
        request.f0 = analysis::rootFrequencyHz ((double) handles.rootNote->load(),
                                                (double) handles.rootCents->load());
        request.numFrames = params::frameCountForChoice (
            (int) std::lround (handles.toneFrames->load()));

        return request;
    }

    void KeepsakeProcessor::runExtraction (const ExtractionRequest& request)
    {
        const auto window = CaptureWindow::resolve (*request.source,
                                                    request.place,
                                                    request.captureLengthMs,
                                                    request.source->sampleRate);

        wavetableStore.publish (analysis::buildWavetableSet (*request.source, window,
                                                             request.f0,
                                                             request.numFrames));
    }

    void KeepsakeProcessor::extractNow()
    {
        if (auto request = makeExtractionRequest())
            runExtraction (*request);
        else
            wavetableStore.publish (nullptr); // no source (e.g. failed restore) -> Tone silent
    }

    void KeepsakeProcessor::ExtractionWorker::enqueue (ExtractionRequest&& request)
    {
        {
            const juce::ScopedLock sl (slotLock);
            slot = std::move (request); // latest wins; an unserviced older request is gone
        }

        // juce::Thread's built-in event, so stopThread() can wake the wait() too.
        notify();
    }

    void KeepsakeProcessor::ExtractionWorker::run()
    {
        while (! threadShouldExit())
        {
            wait (-1);

            // Drain until the slot stays empty: a request arriving mid-build is
            // picked up immediately after publish, so the store always converges
            // on the newest values.
            for (;;)
            {
                if (threadShouldExit())
                    return;

                std::optional<ExtractionRequest> request;

                {
                    const juce::ScopedLock sl (slotLock);
                    request.swap (slot);
                }

                if (! request.has_value())
                    break;

                owner.runExtraction (*request);
            }
        }
    }

    void KeepsakeProcessor::ExtractionPoller::timerCallback()
    {
        const auto nowMs = juce::Time::getMillisecondCounterHiRes();

        const double values[5] = {
            (double) owner.handles.place->load(),
            (double) owner.handles.captureLength->load(),
            (double) owner.handles.rootNote->load(),
            (double) owner.handles.rootCents->load(),
            (double) owner.handles.toneFrames->load(),
        };

        const auto generation = owner.sourceGeneration.load (std::memory_order_acquire);

        bool changed = generation != lastSourceGeneration;

        for (int i = 0; i < 5; ++i)
            changed = changed || std::abs (values[i] - lastValues[i]) > 1.0e-9;

        if (changed)
        {
            std::copy (std::begin (values), std::end (values), std::begin (lastValues));
            lastSourceGeneration = generation;
            lastChangeMs = nowMs;
            dirty = true;

            if (first)
            {
                // Startup baseline: don't extract just because the poller woke up
                // with defaults, only once a source actually exists.
                first = false;
                dirty = owner.sourceStore.hasSource();
            }
        }

        if (! dirty)
            return;

        // Trailing debounce (80ms of quiet) so a drag settles before the rebuild -
        // PLUS a throttle (200ms) so sustained automation still re-extracts: a pure
        // debounce never fires under a host LFO and Tone would freeze stale. The
        // throttle exceeds the 30ms swap fade, so fades always complete. This is
        // the spec's "granular-forward, catching up in steps".
        const auto quietMs = nowMs - lastChangeMs;
        const auto sinceRequestMs = nowMs - lastRequestMs;

        if (quietMs >= 80.0 || sinceRequestMs >= 200.0)
        {
            if (auto request = owner.makeExtractionRequest())
            {
                owner.extractionWorker.enqueue (std::move (*request));
                lastRequestMs = nowMs;
            }

            dirty = false;
        }
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

        // The send bus and FX are sized to at least the voice-scratch maximum,
        // not just the host's declared block size - hosts treat that hint as
        // advisory, and this codebase already distrusts it.
        {
            const auto maxBlock = juce::jmax (FxChain::kMaxBlockSamples, samplesPerBlock);
            const auto channels = juce::jlimit (1, 2, getTotalNumOutputChannels());

            wetSendBus.setSize (channels, maxBlock);
            wetSendBus.clear();
            fx.prepare (sampleRate, maxBlock, channels,
                        handles.warmthAmount->load() * 0.01f,
                        handles.airSize->load() * 0.01f);
        }

        outputGain.reset (sampleRate, 0.02);
        outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (handles.masterGain->load()));
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
        // Longest possible amp release, so hosts do not cut the tail on the last
        // note - plus the Air tail on top when the reverb can be running.
        const auto reverbInPlay = handles.airMix != nullptr && handles.airMix->load() > 0.0f;
        return reverbInPlay ? 20.0 : 10.0;
    }

    void KeepsakeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        juce::ScopedNoDenormals noDenormals;

        // Host tempo for synced LFOs, read once per block. No playhead / no tempo
        // (the test harness) deterministically falls back to 120 BPM.
        if (auto* head = getPlayHead())
        {
            if (auto position = head->getPosition())
                if (auto bpm = position->getBpm())
                    blockContext.bpm.store (*bpm, std::memory_order_relaxed);
        }

        buffer.clear();

        // Switching voice mode mid-performance releases everything through the
        // existing 5ms steal fade, so the mode change is graceful, and the mono
        // stack cannot inherit stale poly state.
        {
            const auto mode = (int) std::lround (handles.voiceMode->load());

            if (mode != lastVoiceMode)
            {
                lastVoiceMode = mode;
                synth.allNotesOff (0, true);
            }
        }

        // Collect this block's per-voice reverb sends, render, then run the
        // master FX (spec §2.6 flow: voices -> Saturation -> Chorus -> Reverb).
        wetSendBus.clear (0, juce::jmin (buffer.getNumSamples(), wetSendBus.getNumSamples()));

        synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());

        {
            // The reverb also runs whenever a mod slot routes to Reverb Mix -
            // the per-voice send must be audible at Mix 0 (that is its point).
            // Parameter-driven, not signal-driven: see FxChain's header.
            bool reverbRouted = false;

            for (int i = 0; i < mod::kNumSlots; ++i)
                reverbRouted = reverbRouted
                               || ((int) std::lround (handles.modDest[i]->load()) == mod::destReverbMix
                                   && (int) std::lround (handles.modSource[i]->load()) != mod::srcNone
                                   && ! juce::exactlyEqual (handles.modDepth[i]->load(), 0.0f));

            fx.process (buffer, wetSendBus,
                        handles.warmthAmount->load() * 0.01f,
                        handles.chorusAmount->load() * 0.01f,
                        handles.airSize->load() * 0.01f,
                        handles.airMix->load() * 0.01f,
                        reverbRouted);
        }

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

        // The poller watches this and schedules extraction for the new source
        // (or publishes a null set if the source went away).
        sourceGeneration.fetch_add (1, std::memory_order_release);
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
        state.setProperty (ids::presetName, presetDisplayName, nullptr);

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

        // A valid blob from an older build of the same state version (e.g. an
        // M4-era session loading into an M5 build) simply lacks the newer
        // parameters. replaceState handles that correctly in JUCE 8: a
        // parameter with no child in the incoming tree gets a fresh child
        // appended with no value property, and the childAdded listener then
        // sets the parameter to its DEFAULT (not its stale current value).
        // presets_missingParametersLoadAsDefaultsNotStaleValues pins that
        // library behavior - if a JUCE upgrade changes it, defaults must be
        // merged into the tree here before replaceState.
        embeddedAudioBase64  = tree.getProperty (ids::audioData, juce::String()).toString();
        embeddedAudioName    = tree.getProperty (ids::audioName, juce::String()).toString();
        embeddedAudioRate    = (double) tree.getProperty (ids::audioRate, 0.0);
        embeddedAudioTrimmed = (bool) tree.getProperty (ids::audioTrimmed, false);
        presetDisplayName    = tree.getProperty (ids::presetName, juce::String()).toString();

        // The audio blob is not a parameter; strip it before handing the tree back to
        // the APVTS so it does not end up in the parameter state.
        tree.removeProperty (ids::audioData, nullptr);
        tree.removeProperty (ids::audioName, nullptr);
        tree.removeProperty (ids::audioRate, nullptr);
        tree.removeProperty (ids::audioTrimmed, nullptr);
        tree.removeProperty (ids::presetName, nullptr);

        apvts.replaceState (tree);

        auto restored = captureIO.decodeFromBase64 (embeddedAudioBase64,
                                                    embeddedAudioName,
                                                    embeddedAudioRate,
                                                    currentSampleRate);

        publishSource (restored, embeddedAudioName);
    }

    void KeepsakeProcessor::randomizeParameters (juce::int64 seed)
    {
        juce::Random rng (seed);
        const auto& parameters = getParameters();

        randomizeStash.clear();
        randomizeStash.reserve ((size_t) parameters.size());

        for (auto* p : parameters)
            randomizeStash.push_back (p->getValue());

        for (auto* p : parameters)
        {
            // Master output is excluded for ear protection (spec §3), Root and
            // Fine because randomised tuning makes every roll sound wrong
            // regardless of texture; the loaded audio is not a parameter, so
            // it is excluded by construction. Everything else rolls.
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
                if (ranged->paramID == juce::String (params::masterGain)
                    || ranged->paramID == juce::String (params::rootNote)
                    || ranged->paramID == juce::String (params::rootCents))
                    continue;

            p->beginChangeGesture();
            p->setValueNotifyingHost (rng.nextFloat());
            p->endChangeGesture();
        }
    }

    bool KeepsakeProcessor::undoRandomize()
    {
        const auto& parameters = getParameters();

        if ((int) randomizeStash.size() != parameters.size())
            return false; // nothing stashed

        for (int i = 0; i < parameters.size(); ++i)
        {
            auto* p = parameters[i];
            p->beginChangeGesture();
            p->setValueNotifyingHost (randomizeStash[(size_t) i]);
            p->endChangeGesture();
        }

        randomizeStash.clear();
        return true;
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
