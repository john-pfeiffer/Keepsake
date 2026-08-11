#include "TestHarness.h"
#include "TestUtils.h"
#include "PluginProcessor.h"
#include "engine/Analysis.h"
#include "engine/FocusCoupling.h"
#include "engine/LFO.h"

using namespace keepsake;

namespace
{
    /** Loads an arbitrary prebuilt source into a processor without touching
        the filesystem, via the real serialisation path. */
    void giveProcessorThisSource (KeepsakeProcessor& proc, double sampleRate,
                                  const SourceAudio& sourceToLoad)
    {
        const auto encoded = CaptureIO::encodeToBase64 (sourceToLoad);

        auto state = proc.getState().copyState();
        state.setProperty ("audioData", encoded, nullptr);
        state.setProperty ("audioName", sourceToLoad.name, nullptr);
        state.setProperty ("audioRate", sampleRate, nullptr);

        // Wrap in the same magic/version/length header the processor writes, so the
        // tests go through the real validation path rather than around it.
        juce::MemoryBlock block;
        {
            juce::MemoryOutputStream stream (block, false);
            stream.writeInt ((int) KeepsakeProcessor::kStateMagic);
            stream.writeInt (KeepsakeProcessor::kStateVersion);

            juce::MemoryBlock payload;
            {
                juce::MemoryOutputStream payloadStream (payload, false);
                state.writeToStream (payloadStream);
            }

            stream.writeInt ((int) payload.getSize());
            stream.write (payload.getData(), payload.getSize());
        }

        proc.setStateInformation (block.getData(), (int) block.getSize());
    }

    /** Loads a synthetic keepsake into a processor without touching the filesystem. */
    void giveProcessorASource (KeepsakeProcessor& proc, double sampleRate)
    {
        // Round-trip through the real serialisation path so the tests exercise the
        // same code a restored preset would.
        auto source = ktest::makeSineSource (220.0, 2.0, sampleRate, "test-tone");
        giveProcessorThisSource (proc, sampleRate, *source);
    }

    juce::AudioBuffer<float> renderMidi (KeepsakeProcessor& proc,
                                         const std::vector<int>& notes,
                                         int numSamples,
                                         int blockSize,
                                         double sampleRate)
    {
        proc.prepareToPlay (sampleRate, blockSize);

        juce::AudioBuffer<float> out (2, numSamples);
        out.clear();

        juce::AudioBuffer<float> block (2, blockSize);

        int pos = 0;
        bool notesSent = false;

        while (pos < numSamples)
        {
            const auto n = juce::jmin (blockSize, numSamples - pos);

            block.setSize (2, n, false, false, true);
            block.clear();

            juce::MidiBuffer midi;

            if (! notesSent)
            {
                for (auto note : notes)
                    midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);

                notesSent = true;
            }

            // Release everything two thirds of the way through, so the tail is rendered.
            if (pos <= numSamples * 2 / 3 && pos + n > numSamples * 2 / 3)
                for (auto note : notes)
                    midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);

            proc.processBlock (block, midi);

            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, pos, block, ch, 0, n);

            pos += n;
        }

        return out;
    }
}

KTEST_CASE (processor_hasAllExpectedParametersExposedToTheHost)
{
    KeepsakeProcessor proc;

    // Spec §4: every user-facing control is host-automatable from v1.
    const char* expected[] = {
        params::place, params::captureLength, params::rootNote, params::rootCents,
        params::grainSize, params::grainDensity, params::grainDrift, params::grainShimmer,
        params::grainWindow, params::grainSpread,
        params::grainSync, params::grainDivision,
        params::warpMode, params::grainSnap, params::pitchMode,
        params::focus, params::toneFrame, params::toneFrames, params::toneFrameWrap,
        params::filterType, params::filterCutoff, params::filterResonance, params::filterKeytrack,
        params::env2Attack, params::env2Decay, params::env2Sustain, params::env2Release,
        params::lfo1Shape, params::lfo1Rate, params::lfo1Sync, params::lfo1Division, params::lfo1Retrig,
        params::lfo2Shape, params::lfo2Rate, params::lfo2Sync, params::lfo2Division, params::lfo2Retrig,
        params::voiceMode, params::glideTime,
        params::warmthAmount, params::chorusAmount, params::airSize, params::airMix,
        params::ampAttack, params::ampDecay, params::ampSustain, params::ampRelease,
        params::masterGain
    };

    for (const auto* id : expected)
        EXPECT_MSG (proc.getState().getParameter (id) != nullptr,
                    juce::String ("missing parameter: ") + id);

    // The 6 mod slots (source/dest/depth each) have generated IDs.
    for (int slot = 0; slot < 6; ++slot)
        for (const auto* field : { "Source", "Dest", "Depth" })
            EXPECT_MSG (proc.getState().getParameter (params::slotId (field, slot)) != nullptr,
                        "missing mod slot parameter: " + params::slotId (field, slot));

    EXPECT_TRUE (proc.getParameters().size() == (int) std::size (expected) + 18);

    // Every parameter must have a name and produce readable text, or DAW automation
    // lanes are unusable.
    for (auto* p : proc.getParameters())
    {
        EXPECT_TRUE (p->getName (64).isNotEmpty());
        EXPECT_TRUE (p->getText (0.5f, 64).isNotEmpty());
    }
}

KTEST_CASE (processor_isSilentWithNoKeepsakeLoaded)
{
    KeepsakeProcessor proc;

    const auto out = renderMidi (proc, { 60 }, 8192, 256, 48000.0);

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_NEAR ((double) out.getMagnitude (0, out.getNumSamples()), 0.0, 1.0e-6);
}

KTEST_CASE (processor_playsAKeepsakeAcrossTheKeyboard)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 512);
    giveProcessorASource (proc, sampleRate);

    for (int note : { 36, 48, 60, 72, 84 })
    {
        const auto out = renderMidi (proc, { note }, 24000, 512, sampleRate);

        EXPECT_MSG (ktest::isFinite (out),
                    "note " + juce::String (note) + " produced non-finite output");
        EXPECT_MSG (out.getMagnitude (0, out.getNumSamples()) > 0.001f,
                    "note " + juce::String (note) + " produced silence");
    }
}

/** M2 exit test: 12-voice poly. */
KTEST_CASE (processor_playsTwelveVoicesAtOnce)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 512);
    giveProcessorASource (proc, sampleRate);

    std::vector<int> chord;

    for (int i = 0; i < KeepsakeProcessor::kNumVoices; ++i)
        chord.push_back (48 + i);

    proc.prepareToPlay (sampleRate, 512);

    juce::AudioBuffer<float> block (2, 512);
    juce::MidiBuffer midi;

    for (auto note : chord)
        midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);

    block.clear();
    proc.processBlock (block, midi);

    // A few more blocks so every voice is definitely running.
    for (int i = 0; i < 8; ++i)
    {
        juce::MidiBuffer empty;
        block.clear();
        proc.processBlock (block, empty);
    }

    EXPECT_TRUE (proc.getActiveVoiceCount() == KeepsakeProcessor::kNumVoices);
    EXPECT_TRUE (ktest::isFinite (block));
}

KTEST_CASE (processor_voiceStealingDoesNotBlowUp)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 256);
    giveProcessorASource (proc, sampleRate);

    juce::AudioBuffer<float> block (2, 256);

    // Twice the voice count, so stealing is guaranteed.
    for (int note = 40; note < 40 + KeepsakeProcessor::kNumVoices * 2; ++note)
    {
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);

        block.clear();
        proc.processBlock (block, midi);

        EXPECT_MSG (ktest::isFinite (block), "voice stealing produced non-finite output");
        EXPECT_MSG (proc.getActiveVoiceCount() <= KeepsakeProcessor::kNumVoices,
                    "more voices active than the poly limit");
    }
}

/** Renders must not depend on how the host chops up the buffer. */
KTEST_CASE (processor_outputIsIndependentOfBlockSize)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 12288;

    auto render = [&] (int blockSize)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        giveProcessorASource (proc, sampleRate);
        return renderMidi (proc, { 60 }, numSamples, blockSize, sampleRate);
    };

    const auto a = render (64);
    const auto b = render (1024);

    EXPECT_TRUE (ktest::isFinite (a) && ktest::isFinite (b));

    // Comparing two silent renders would pass this test for the wrong reason.
    EXPECT_MSG (a.getMagnitude (0, numSamples) > 0.001f, "64-sample block render was silent");
    EXPECT_MSG (b.getMagnitude (0, numSamples) > 0.001f, "1024-sample block render was silent");

    // Grain scheduling is sample-accurate and the RNG stream is per voice, so the two
    // renders should agree to within float rounding.
    float worst = 0.0f;

    for (int i = 0; i < numSamples; ++i)
        worst = juce::jmax (worst, std::abs (a.getSample (0, i) - b.getSample (0, i)));

    EXPECT_MSG (worst < 1.0e-5f,
                "block size changed the render by " + juce::String (worst, 8));
}

KTEST_CASE (processor_stateRoundTripPreservesParametersAndAudio)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor a;
    a.prepareToPlay (sampleRate, 512);
    giveProcessorASource (a, sampleRate);

    a.getState().getParameter (params::grainDensity)
        ->setValueNotifyingHost (a.getState().getParameter (params::grainDensity)->convertTo0to1 (77.0f));
    a.getState().getParameter (params::place)->setValueNotifyingHost (0.42f);

    juce::MemoryBlock stateBlock;
    a.getStateInformation (stateBlock);

    KeepsakeProcessor b;
    b.prepareToPlay (sampleRate, 512);
    b.setStateInformation (stateBlock.getData(), (int) stateBlock.getSize());

    EXPECT_NEAR ((double) b.getState().getRawParameterValue (params::grainDensity)->load(), 77.0, 0.5);
    EXPECT_NEAR ((double) b.getState().getRawParameterValue (params::place)->load(), 0.42, 0.001);

    // M1 exit test: the keepsake came back with no source file anywhere in sight.
    EXPECT_TRUE (b.getSourceStore().hasSource());

    const auto restored = b.getSourceStore().getForMessageThread();
    EXPECT_TRUE (restored != nullptr && restored->getNumSamples() > 0);
}

/** pluginval fuzzes setStateInformation with arbitrary bytes; it must not assert. */
KTEST_CASE (processor_survivesGarbageState)
{
    KeepsakeProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    juce::Random rng (12345);

    for (int trial = 0; trial < 32; ++trial)
    {
        juce::MemoryBlock junk ((size_t) rng.nextInt ({ 1, 4096 }));

        for (size_t i = 0; i < junk.getSize(); ++i)
            junk[(int) i] = (char) rng.nextInt (256);

        proc.setStateInformation (junk.getData(), (int) junk.getSize());
    }

    // Still usable afterwards.
    const auto out = renderMidi (proc, { 60 }, 4096, 256, 48000.0);
    EXPECT_TRUE (ktest::isFinite (out));
}

KTEST_CASE (processor_survivesTruncatedValidState)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor a;
    a.prepareToPlay (sampleRate, 512);
    giveProcessorASource (a, sampleRate);

    juce::MemoryBlock full;
    a.getStateInformation (full);

    KeepsakeProcessor b;
    b.prepareToPlay (sampleRate, 512);

    for (int fraction : { 2, 4, 8, 16 })
        b.setStateInformation (full.getData(), (int) full.getSize() / fraction);

    // A truncated preset must be rejected outright, not half-applied: loading half a
    // keepsake would be worse than loading none.
    EXPECT_MSG (! b.getSourceStore().hasSource(), "a truncated preset was partially applied");

    const auto out = renderMidi (b, { 60 }, 4096, 256, sampleRate);
    EXPECT_TRUE (ktest::isFinite (out));

    // The intact blob still loads, so the guard is not simply rejecting everything.
    b.setStateInformation (full.getData(), (int) full.getSize());
    EXPECT_MSG (b.getSourceStore().hasSource(), "the intact preset failed to load");
}

KTEST_CASE (processor_survivesSampleRateChanges)
{
    KeepsakeProcessor proc;
    proc.prepareToPlay (44100.0, 512);
    giveProcessorASource (proc, 44100.0);

    for (double rate : { 44100.0, 48000.0, 96000.0, 88200.0 })
    {
        proc.prepareToPlay (rate, 512);

        const auto source = proc.getSourceStore().getForMessageThread();
        EXPECT_MSG (source != nullptr, "lost the keepsake at " + juce::String (rate));

        if (source != nullptr)
            EXPECT_MSG (std::abs (source->sampleRate - rate) < 1.0,
                        "source not resampled to " + juce::String (rate));

        const auto out = renderMidi (proc, { 60 }, (int) (rate * 0.25), 512, rate);
        EXPECT_MSG (ktest::isFinite (out), "non-finite output at " + juce::String (rate));
    }
}

KTEST_CASE (processor_auditionStartsAndStopsWithoutClicking)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 256);
    giveProcessorASource (proc, sampleRate);

    juce::AudioBuffer<float> out (2, 24000);
    out.clear();

    juce::AudioBuffer<float> block (2, 256);

    for (int pos = 0; pos < out.getNumSamples(); pos += 256)
    {
        if (pos == 2560)
            proc.startAudition (KeepsakeProcessor::Audition::window);

        if (pos == 12800)
            proc.stopAudition();

        juce::MidiBuffer midi;
        block.clear();
        proc.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, block, ch, 0, juce::jmin (256, out.getNumSamples() - pos));
    }

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_TRUE (out.getMagnitude (0, out.getNumSamples()) > 0.001f);
    EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.25f,
                "audition start/stop clicked: step of "
                    + juce::String (ktest::maxDiscontinuity (out), 4));
}

// =============================================================================
// M3: Tone engine through the processor
// =============================================================================

namespace
{
    void setParam (KeepsakeProcessor& proc, const juce::String& id, float plainValue)
    {
        auto* p = proc.getState().getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
    }
}

KTEST_CASE (processor_focusZeroIsBitIdenticalToCloudOnly)
{
    constexpr double sampleRate = 48000.0;

    auto render = [&] (bool withExtraction)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorASource (proc, sampleRate);

        if (withExtraction)
            proc.extractNow();

        return renderMidi (proc, { 60 }, 24000, 512, sampleRate);
    };

    // Focus defaults to 0 (full Cloud). With wavetables extracted and without,
    // the render must not differ by a single bit - the Tone path is genuinely
    // skipped, not just quiet.
    const auto without = render (false);
    const auto with = render (true);

    EXPECT_TRUE (without.getMagnitude (0, without.getNumSamples()) > 0.001f);

    float worst = 0.0f;
    for (int i = 0; i < without.getNumSamples(); ++i)
        worst = juce::jmax (worst, std::abs (without.getSample (0, i) - with.getSample (0, i)));

    EXPECT_MSG (! (worst > 0.0f),
                "focus=0 render changed when wavetables exist: diff " + juce::String (worst, 8));
}

KTEST_CASE (processor_fullFocusPlaysTheToneEngine)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 512);
    giveProcessorASource (proc, sampleRate);
    proc.extractNow();

    setParam (proc, params::focus, 1.0f);

    const auto out = renderMidi (proc, { 60 }, 24000, 512, sampleRate);

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_MSG (out.getMagnitude (0, out.getNumSamples()) > 0.001f,
                "full-Tone render was silent");
    EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.25f,
                "full-Tone render clicked: step "
                    + juce::String (ktest::maxDiscontinuity (out), 4));
}

KTEST_CASE (processor_toneWithoutExtractionIsSilentNotACrash)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 512);
    giveProcessorASource (proc, sampleRate);
    // no extractNow(): the async worker never runs in the harness

    setParam (proc, params::focus, 1.0f);

    const auto out = renderMidi (proc, { 60 }, 8192, 512, sampleRate);

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_NEAR ((double) out.getMagnitude (0, out.getNumSamples()), 0.0, 1.0e-6);
}

/** M3 exit test: Place automation sweeps without clicks - stepping Place and
    re-extracting between blocks exercises the swap crossfade deterministically
    (the swap starts at a block boundary; determinism-sensitive tests never swap
    mid-render, which is why this one only asserts continuity). */
KTEST_CASE (processor_placeSweepWithReExtractionIsClickFree)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 48000, blockSize = 256;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, blockSize);
    giveProcessorASource (proc, sampleRate);
    proc.extractNow();

    setParam (proc, params::focus, 0.5f); // both engines running - worst case

    juce::AudioBuffer<float> out (2, numSamples);
    out.clear();
    juce::AudioBuffer<float> block (2, blockSize);

    bool noteSent = false;
    int blockIndex = 0;

    for (int pos = 0; pos < numSamples; pos += blockSize)
    {
        // ~10 extractions across the sweep, i.e. the spec's "catching up in
        // steps" cadence, each one a fresh set swap mid-note.
        if (blockIndex % 20 == 0)
        {
            setParam (proc, params::place, (float) pos / (float) numSamples);
            proc.extractNow();
        }

        juce::MidiBuffer midi;

        if (! noteSent)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 57, 0.8f), 0);
            noteSent = true;
        }

        block.clear();
        proc.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, block, ch, 0, juce::jmin (blockSize, numSamples - pos));

        ++blockIndex;
    }

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_MSG (out.getMagnitude (0, numSamples) > 0.001f, "place sweep rendered silence");
    EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.25f,
                "place sweep with re-extraction clicked: step "
                    + juce::String (ktest::maxDiscontinuity (out), 4));
}

KTEST_CASE (processor_garbageRestoreDoesNotBreakExtraction)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 512);

    // Garbage restore leaves no source; extractNow must publish a null set (Tone
    // silent) rather than crash or extract against nothing.
    juce::MemoryBlock junk (512);
    juce::Random rng (99);
    for (size_t i = 0; i < junk.getSize(); ++i)
        junk[(int) i] = (char) rng.nextInt (256);

    proc.setStateInformation (junk.getData(), (int) junk.getSize());
    proc.extractNow();

    setParam (proc, params::focus, 1.0f);
    const auto out = renderMidi (proc, { 60 }, 4096, 256, sampleRate);
    EXPECT_TRUE (ktest::isFinite (out));

    // And a real source afterwards recovers the full path.
    giveProcessorASource (proc, sampleRate);
    proc.extractNow();

    const auto out2 = renderMidi (proc, { 60 }, 24000, 256, sampleRate);
    EXPECT_TRUE (out2.getMagnitude (0, out2.getNumSamples()) > 0.001f);
}

// =============================================================================
// M4: filter
// =============================================================================

KTEST_CASE (processor_filterActuallyFilters)
{
    constexpr double sampleRate = 48000.0;

    auto render = [&] (float cutoffHz)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorASource (proc, sampleRate);
        proc.extractNow();
        setParam (proc, params::focus, 1.0f); // Tone-only: deterministic harmonics
        setParam (proc, params::filterCutoff, cutoffHz);
        return renderMidi (proc, { 60 }, 48000 + 32768, 512, sampleRate);
    };

    const auto open = render (20000.0f);
    const auto closed = render (300.0f); // LP well below the note's harmonics

    const auto openSpec = ktest::powerSpectrumDb (open, 40000);
    const auto closedSpec = ktest::powerSpectrumDb (closed, 40000);

    // Compare energy around the 8th harmonic of C3 (~2093Hz): the closed filter
    // must attenuate it by a lot; 300Hz -> 2093Hz is ~2.8 octaves above cutoff,
    // a 12dB/oct LP predicts ~-33dB. Assert > 20dB to stay robust.
    const auto bin = (int) std::round (8.0 * analysis::noteFrequencyHz (60) * 32768.0 / sampleRate);

    float openPeak = -300.0f, closedPeak = -300.0f;
    for (int k = bin - 3; k <= bin + 3; ++k)
    {
        openPeak = juce::jmax (openPeak, openSpec[(size_t) k]);
        closedPeak = juce::jmax (closedPeak, closedSpec[(size_t) k]);
    }

    EXPECT_MSG (openPeak - closedPeak > 20.0f,
                "LP at 300Hz only attenuated the 8th harmonic by "
                    + juce::String (openPeak - closedPeak, 1) + " dB");
}

KTEST_CASE (processor_filterExtremesAreStableAndClickFree)
{
    constexpr double sampleRate = 48000.0;

    for (auto [type, cutoff, q] : { std::tuple { 0, 20.0f, 10.0f },
                                    std::tuple { 1, 640.0f, 10.0f },
                                    std::tuple { 2, 18000.0f, 10.0f },
                                    std::tuple { 0, 20000.0f, 0.5f } })
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorASource (proc, sampleRate);
        proc.extractNow();
        setParam (proc, params::focus, 0.5f);
        setParam (proc, params::filterType, (float) type);
        setParam (proc, params::filterCutoff, cutoff);
        setParam (proc, params::filterResonance, q);

        const auto out = renderMidi (proc, { 60 }, 24000, 512, sampleRate);

        EXPECT_MSG (ktest::isFinite (out),
                    "filter type " + juce::String (type) + " cutoff " + juce::String (cutoff)
                        + " produced non-finite output");
    }
}

// =============================================================================
// M4: LFOs + mod matrix
// =============================================================================

namespace
{
    void setModSlot (KeepsakeProcessor& proc, int slot, int source, int dest, float depthPercent)
    {
        setParam (proc, params::slotId ("Source", slot), (float) source);
        setParam (proc, params::slotId ("Dest", slot), (float) dest);
        setParam (proc, params::slotId ("Depth", slot), depthPercent);
    }
}

KTEST_CASE (lfo_shapesAndSyncMathAreCorrect)
{
    LFO lfo (1);
    lfo.noteOn (true);

    // A 1Hz sine at 48k advanced by 12000 samples (quarter cycle) reads ~1.
    const auto quarter = lfo.advance (LFO::sine, 1.0, 12000, 48000.0);
    EXPECT_NEAR ((double) quarter, 1.0, 1.0e-3);

    // Synced rate: 120 BPM, 1/4 (index 4) = 2Hz; 1/1 (index 0) = 0.5Hz.
    EXPECT_NEAR (LFO::syncedRateHz (120.0, 4), 2.0, 1.0e-9);
    EXPECT_NEAR (LFO::syncedRateHz (120.0, 0), 0.5, 1.0e-9);
    EXPECT_NEAR (LFO::syncedRateHz (120.0, 11), 16.0, 1.0e-9); // 1/32

    // Retrig determinism: two retriggered runs produce identical S&H streams.
    LFO a (42), b (42);
    a.noteOn (true);
    b.noteOn (true);

    for (int i = 0; i < 64; ++i)
    {
        const auto va = a.advance (LFO::sampleAndHold, 7.0, 480, 48000.0);
        const auto vb = b.advance (LFO::sampleAndHold, 7.0, 480, 48000.0);
        EXPECT_MSG (! (std::abs (va - vb) > 0.0f), "S&H streams diverged");
        EXPECT_TRUE (va >= -1.0f && va <= 1.0f);
    }
}

KTEST_CASE (processor_blockSizeIndependentWithLfoModulationActive)
{
    // The whole point of anchoring the control tick to the voice-local clock:
    // this render has an active LFO on Focus and must STILL be bit-identical
    // at different host block sizes.
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 12288;

    auto render = [&] (int blockSize)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        giveProcessorASource (proc, sampleRate);
        proc.extractNow();
        setParam (proc, params::focus, 0.5f);
        setParam (proc, params::lfo1Rate, 3.0f);
        setModSlot (proc, 0, 2 /*LFO1*/, 1 /*Focus*/, 60.0f);
        return renderMidi (proc, { 60 }, numSamples, blockSize, sampleRate);
    };

    const auto a = render (64);
    const auto b = render (1024);

    EXPECT_TRUE (a.getMagnitude (0, numSamples) > 0.001f);

    float worst = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        worst = juce::jmax (worst, std::abs (a.getSample (0, i) - b.getSample (0, i)));

    EXPECT_MSG (! (worst > 0.0f),
                "LFO-modulated render depends on block size: diff " + juce::String (worst, 8));
}

KTEST_CASE (processor_modRoutedSweepsAreClickFree)
{
    constexpr double sampleRate = 48000.0;

    struct Case { const char* name; int dest; float depth; };

    // Fast LFO, full-ish depth, on every twitchy destination.
    for (auto c : { Case { "cutoff", 3, 100.0f },
                    Case { "frame", 8, 100.0f },
                    Case { "focus", 1, 80.0f },
                    Case { "pitch", 9, 50.0f },
                    Case { "place", 2, 40.0f } })
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 256);
        giveProcessorASource (proc, sampleRate);
        proc.extractNow();
        setParam (proc, params::focus, 0.5f);
        // Moderate Q on purpose: at high resonance a sweeping peak legitimately
        // swells the signal to several times unity, and a max-step click detector
        // then measures physics, not bugs (a TPT SVF's state is continuous under
        // coefficient changes). High-Q stability has its own test; the bounded-
        // peak assert below still catches genuine runaway here.
        setParam (proc, params::filterResonance, 2.0f);
        setParam (proc, params::lfo1Rate, 8.0f);
        setModSlot (proc, 0, 2 /*LFO1*/, c.dest, c.depth);

        const auto out = renderMidi (proc, { 89 }, 48000, 256, sampleRate);

        EXPECT_MSG (ktest::isFinite (out), juce::String (c.name) + ": non-finite");
        EXPECT_MSG (out.getMagnitude (0, out.getNumSamples()) > 0.001f,
                    juce::String (c.name) + ": silent");
        EXPECT_MSG (out.getMagnitude (0, out.getNumSamples()) < 4.0f,
                    juce::String (c.name) + ": runaway level "
                        + juce::String (out.getMagnitude (0, out.getNumSamples()), 3));
        EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.3f,
                    juce::String (c.name) + " LFO sweep clicked: step "
                        + juce::String (ktest::maxDiscontinuity (out), 4));
    }
}

KTEST_CASE (processor_pingPongWrapDiffersFromLoop)
{
    constexpr double sampleRate = 48000.0;

    auto render = [&] (float wrapMode)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 256);
        giveProcessorASource (proc, sampleRate);
        proc.extractNow();
        setParam (proc, params::focus, 1.0f);
        setParam (proc, params::toneFrame, 0.5f);
        setParam (proc, params::toneFrameWrap, wrapMode);
        setParam (proc, params::lfo1Rate, 2.0f);
        setParam (proc, params::lfo1Shape, 2.0f); // saw: guaranteed to cross the fold
        setModSlot (proc, 0, 2, 8 /*Frame*/, 100.0f);
        return renderMidi (proc, { 60 }, 24000, 256, sampleRate);
    };

    const auto loop = render (0.0f);
    const auto pingPong = render (1.0f);

    EXPECT_TRUE (ktest::isFinite (loop) && ktest::isFinite (pingPong));

    // Both click-free, and the two wrap modes must actually differ.
    EXPECT_MSG (ktest::maxDiscontinuity (loop) < 0.3f, "Loop wrap clicked");
    EXPECT_MSG (ktest::maxDiscontinuity (pingPong) < 0.3f, "Ping-Pong wrap clicked");

    float diff = 0.0f;
    for (int i = 0; i < loop.getNumSamples(); ++i)
        diff = juce::jmax (diff, std::abs (loop.getSample (0, i) - pingPong.getSample (0, i)));

    EXPECT_MSG (diff > 1.0e-4f, "Loop and Ping-Pong renders are identical - the wrap toggle is dead");
}

// =============================================================================
// M4: Focus coupling
// =============================================================================

KTEST_CASE (coupling_curvesKeepTheirLoadBearingProperties)
{
    // Identity at f=0: this is what keeps the focus-0 render bit-identical to
    // the pre-Tone engine. Exact equality on purpose.
    EXPECT_TRUE (juce::exactlyEqual (coupling::densityMultiplier (0.0f), 1.0f));
    EXPECT_TRUE (juce::exactlyEqual (coupling::grainSizeMultiplier (0.0f), 1.0f));
    EXPECT_TRUE (juce::exactlyEqual (coupling::shimmerMultiplier (0.0f), 1.0f));
    EXPECT_TRUE (juce::exactlyEqual (coupling::toneGain (0.0f), 0.0f));
    EXPECT_TRUE (juce::exactlyEqual (coupling::cloudGain (0.0f), 1.0f));

    // Designed endpoints at f=1.
    EXPECT_NEAR ((double) coupling::densityMultiplier (1.0f), 2.0, 1.0e-6);
    EXPECT_NEAR ((double) coupling::grainSizeMultiplier (1.0f), 0.5, 1.0e-6);
    EXPECT_NEAR ((double) coupling::shimmerMultiplier (1.0f), 0.0, 1.0e-6);
    EXPECT_NEAR ((double) coupling::toneGain (1.0f), 1.0, 1.0e-6);
    EXPECT_NEAR ((double) coupling::cloudGain (1.0f), 0.0, 1.0e-6);
    EXPECT_NEAR ((double) coupling::toneDetuneCents (1.0f, 0), 0.0, 1.0e-6);
    EXPECT_NEAR ((double) coupling::toneDetuneCents (0.0f, 0), 7.0, 1.0e-6);
    EXPECT_NEAR ((double) coupling::toneDetuneCents (0.0f, 1), -7.0, 1.0e-6);

    float previousTone = -1.0f, previousDensity = 0.0f;

    for (int i = 0; i <= 100; ++i)
    {
        const auto f = (float) i / 100.0f;

        // The overlap invariant: density x size == 1 everywhere. Cloud's level
        // rides 1/sqrt(overlap), so this is the "no level pumping" guarantee -
        // tuning one curve without its reciprocal breaks the instrument feel.
        EXPECT_NEAR ((double) (coupling::densityMultiplier (f) * coupling::grainSizeMultiplier (f)),
                     1.0, 1.0e-6);

        // "Ducks earlier" means: at or below the EQUAL-POWER reference curve
        // everywhere (f^1.5 <= f, sin monotonic), strictly below in the middle.
        EXPECT_TRUE (coupling::toneGain (f)
                     <= std::sin (juce::MathConstants<float>::halfPi * f) + 1.0e-6f);

        // ...strictly below at the midpoint (the audible part of the design)...
        if (i == 50)
            EXPECT_TRUE (coupling::toneGain (f)
                         < std::sin (juce::MathConstants<float>::halfPi * f) - 0.05f);

        // ...and both headline curves are monotonic.
        EXPECT_TRUE (coupling::toneGain (f) >= previousTone);
        EXPECT_TRUE (coupling::densityMultiplier (f) > previousDensity);
        previousTone = coupling::toneGain (f);
        previousDensity = coupling::densityMultiplier (f);
    }
}

/** The machine-checkable proxy for "one knob feels like an instrument, not a
    mixer": sweeping Focus end to end must neither click nor pump level. */
KTEST_CASE (processor_focusSweepIsClickFreeAndLevelStable)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 96000, blockSize = 256; // 2s sweep

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, blockSize);
    giveProcessorASource (proc, sampleRate);
    proc.extractNow();
    setParam (proc, params::ampAttack, 1.0f);

    juce::AudioBuffer<float> out (2, numSamples);
    out.clear();
    juce::AudioBuffer<float> block (2, blockSize);
    bool noteSent = false;

    for (int pos = 0; pos < numSamples; pos += blockSize)
    {
        setParam (proc, params::focus, (float) pos / (float) numSamples);

        juce::MidiBuffer midi;

        if (! noteSent)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 57, 0.8f), 0);
            noteSent = true;
        }

        block.clear();
        proc.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, block, ch, 0, juce::jmin (blockSize, numSamples - pos));
    }

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.3f,
                "focus sweep clicked: step " + juce::String (ktest::maxDiscontinuity (out), 4));

    // Per-100ms RMS windows (skipping the attack) must stay inside a +-3dB
    // corridor around their median - texture may change, level may not.
    std::vector<float> windowsDb;

    for (int start = 9600; start + 4800 <= numSamples; start += 4800)
    {
        const auto rms = out.getRMSLevel (0, start, 4800);
        windowsDb.push_back (juce::Decibels::gainToDecibels (rms + 1.0e-9f));
    }

    auto sorted = windowsDb;
    std::sort (sorted.begin(), sorted.end());
    const auto median = sorted[sorted.size() / 2];

    for (size_t i = 0; i < windowsDb.size(); ++i)
        EXPECT_MSG (std::abs (windowsDb[i] - median) < 3.0f,
                    "focus sweep pumped: window " + juce::String ((int) i) + " at "
                        + juce::String (windowsDb[i] - median, 2) + " dB from median");
}

// =============================================================================
// M4: mono / legato / glide + wheel
// =============================================================================

KTEST_CASE (processor_monoModeHoldsExactlyOneVoice)
{
    constexpr double sampleRate = 48000.0;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 256);
    giveProcessorASource (proc, sampleRate);
    proc.extractNow();
    setParam (proc, params::voiceMode, 1.0f); // Mono

    juce::AudioBuffer<float> block (2, 256);
    juce::MidiBuffer midi;

    for (int note : { 48, 52, 55, 59, 62 })
        midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);

    block.clear();
    proc.processBlock (block, midi);

    for (int i = 0; i < 8; ++i)
    {
        juce::MidiBuffer empty;
        block.clear();
        proc.processBlock (block, empty);
    }

    EXPECT_MSG (proc.getActiveVoiceCount() == 1,
                "mono chord left " + juce::String (proc.getActiveVoiceCount()) + " voices active");
    EXPECT_TRUE (ktest::isFinite (block));
    EXPECT_TRUE (block.getMagnitude (0, block.getNumSamples()) > 0.001f);
}

KTEST_CASE (processor_legatoDoesNotRetriggerTheEnvelope)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 48000, blockSize = 256;
    constexpr int changeAt = 19200; // 0.4s into an 800ms attack

    auto render = [&] (float mode)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        giveProcessorASource (proc, sampleRate);
        proc.extractNow();
        setParam (proc, params::voiceMode, mode);
        // juce::ADSR's noteOn continues the attack from the CURRENT level, so a
        // mid-attack retrigger is inaudible - the distinguishing phase is decay:
        // by the note change the envelope sits at sustain 0.2; a Mono retrigger
        // climbs back to peak, Legato stays at sustain.
        setParam (proc, params::ampAttack, 1.0f);
        setParam (proc, params::ampDecay, 120.0f);
        setParam (proc, params::ampSustain, 0.2f);
        setParam (proc, params::glideTime, 0.0f);

        juce::AudioBuffer<float> out (2, numSamples);
        out.clear();
        juce::AudioBuffer<float> block (2, blockSize);
        bool first = false, second = false;

        for (int pos = 0; pos < numSamples; pos += blockSize)
        {
            juce::MidiBuffer midi;

            if (! first) { midi.addEvent (juce::MidiMessage::noteOn (1, 48, 0.8f), 0); first = true; }

            if (! second && pos >= changeAt)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
                second = true;
            }

            block.clear();
            proc.processBlock (block, midi);

            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, pos, block, ch, 0, juce::jmin (blockSize, numSamples - pos));
        }

        return out;
    };

    const auto legato = render (2.0f);
    const auto mono = render (1.0f);

    // Right after the change: the Mono retrigger has snapped back to the
    // attack peak; Legato stays at sustain (0.2). Window covers the first 60ms.
    const auto legatoAfter = legato.getRMSLevel (0, changeAt + 480, 2880);
    const auto monoAfter = mono.getRMSLevel (0, changeAt + 480, 2880);
    const auto legatoBefore = legato.getRMSLevel (0, changeAt - 4800, 2880);

    EXPECT_MSG (std::abs (legatoAfter - legatoBefore) < legatoBefore * 0.5f,
                "legato level jumped at the note change ("
                    + juce::String (legatoBefore, 4) + " -> " + juce::String (legatoAfter, 4) + ")");
    EXPECT_MSG (monoAfter > legatoAfter * 1.5f,
                "mono retrigger looks identical to legato (mono "
                    + juce::String (monoAfter, 4) + " vs legato " + juce::String (legatoAfter, 4)
                    + ") - the retrigger flag is dead");
}

KTEST_CASE (processor_glideMovesPitchGradually)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 96000, blockSize = 256;
    constexpr int changeAt = 24000; // note change at 0.5s, glide 300ms

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, blockSize);
    giveProcessorASource (proc, sampleRate);
    proc.extractNow();
    setParam (proc, params::voiceMode, 2.0f); // Legato
    setParam (proc, params::glideTime, 300.0f);
    setParam (proc, params::focus, 1.0f); // Tone only: clean spectrum to measure
    setParam (proc, params::ampAttack, 1.0f);

    juce::AudioBuffer<float> out (2, numSamples);
    out.clear();
    juce::AudioBuffer<float> block (2, blockSize);
    bool first = false, second = false;

    for (int pos = 0; pos < numSamples; pos += blockSize)
    {
        juce::MidiBuffer midi;

        if (! first) { midi.addEvent (juce::MidiMessage::noteOn (1, 48, 0.8f), 0); first = true; }

        if (! second && pos >= changeAt)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 72, 0.8f), 0);
            second = true;
        }

        block.clear();
        proc.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, block, ch, 0, juce::jmin (blockSize, numSamples - pos));
    }

    EXPECT_TRUE (ktest::isFinite (out));

    auto dominantHz = [&] (int start, int fftOrder)
    {
        return ktest::dominantHz (out, start, fftOrder, sampleRate);
    };

    // Well after the glide (long window, fine resolution): pitch has landed.
    const auto landed = dominantHz (60000, 15);
    EXPECT_NEAR (landed, analysis::noteFrequencyHz (72), analysis::noteFrequencyHz (72) * 0.03);

    // Right after the change the pitch must still be travelling. The window has
    // to be SHORTER than the glide (4096 samples = 85ms inside a 300ms glide) -
    // a long window here would span the landing and report the target.
    const auto early = dominantHz (changeAt + 1600, 12);
    EXPECT_MSG (early < analysis::noteFrequencyHz (60),
                "pitch jumped instantly to the target: " + juce::String (early, 1)
                    + " Hz right after the change (glide is dead)");
}

// =============================================================================
// M5: FX chain (Warmth -> Chorus -> Air) and the per-voice reverb send
// =============================================================================

namespace
{
    float rmsOfWindow (const juce::AudioBuffer<float>& b, int start, int num)
    {
        double sum = 0.0;

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            const auto* d = b.getReadPointer (ch);

            for (int i = start; i < start + num; ++i)
                sum += (double) d[i] * d[i];
        }

        return (float) std::sqrt (sum / ((double) num * b.getNumChannels()));
    }

    /** Peak spectrum level within +-3 bins of the given frequency. */
    float peakPowerNearHz (const std::vector<float>& spectrumDb, double hz, double sampleRate)
    {
        const auto fftSize = (int) spectrumDb.size() * 2;
        const auto bin = (int) std::lround (hz / sampleRate * (double) fftSize);
        auto peak = -300.0f;

        for (int k = juce::jmax (0, bin - 3);
             k <= juce::jmin ((int) spectrumDb.size() - 1, bin + 3); ++k)
            peak = juce::jmax (peak, spectrumDb[(size_t) k]);

        return peak;
    }
}

KTEST_CASE (fx_warmthAddsHarmonicsWithoutChangingLevel)
{
    // "tanh w/ drive compensation" (spec §2.6): full Warmth must audibly
    // saturate - odd harmonics appear - while staying a color control, not a
    // volume control: steady-state RMS within +-3dB of the clean render. The
    // makeup constants in FxChain.cpp are owned by this test.
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 61440; // note-off at 40960; FFT fits before it

    auto render = [&] (float warmthPercent)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorASource (proc, sampleRate);
        // Root BEFORE extraction: cycle slicing uses it, and a wrong period
        // bakes harmonics into the frames themselves (a -20dB-dirty baseline).
        setParam (proc, params::rootNote, 57.0f); // source is a 220Hz sine = A2
        proc.extractNow();
        setParam (proc, params::focus, 1.0f);     // pure Tone: a clean baseline
        setParam (proc, params::warmthAmount, warmthPercent);
        return renderMidi (proc, { 57 }, numSamples, 512, sampleRate);
    };

    const auto clean = render (0.0f);
    const auto warm = render (100.0f);

    constexpr int steadyStart = 4800;
    const auto cleanRms = rmsOfWindow (clean, steadyStart, 32768);
    const auto warmRms = rmsOfWindow (warm, steadyStart, 32768);

    EXPECT_TRUE (cleanRms > 0.01f);
    EXPECT_TRUE (ktest::isFinite (warm));

    const auto rmsShiftDb = juce::Decibels::gainToDecibels (warmRms / cleanRms);
    EXPECT_MSG (std::abs (rmsShiftDb) < 3.0f,
                "warmth is a volume control: " + juce::String (rmsShiftDb, 2) + " dB shift");

    // Third harmonic (tanh is odd-symmetric), relative to the fundamental.
    const auto cleanSpectrum = ktest::powerSpectrumDb (clean, steadyStart);
    const auto warmSpectrum = ktest::powerSpectrumDb (warm, steadyStart);

    const auto cleanH3 = peakPowerNearHz (cleanSpectrum, 660.0, sampleRate)
                         - peakPowerNearHz (cleanSpectrum, 220.0, sampleRate);
    const auto warmH3 = peakPowerNearHz (warmSpectrum, 660.0, sampleRate)
                        - peakPowerNearHz (warmSpectrum, 220.0, sampleRate);

    EXPECT_MSG (cleanH3 < -50.0f,
                "clean baseline is already dirty: h3 at " + juce::String (cleanH3, 1) + " dB");
    EXPECT_MSG (warmH3 > cleanH3 + 25.0f,
                "full warmth adds no harmonics: h3 " + juce::String (warmH3, 1)
                    + " dB vs clean " + juce::String (cleanH3, 1) + " dB");
}

KTEST_CASE (fx_chorusThickensWithoutChangingLevel)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 61440;

    auto render = [&] (float chorusPercent)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorASource (proc, sampleRate);
        setParam (proc, params::chorusAmount, chorusPercent);
        return renderMidi (proc, { 60 }, numSamples, 512, sampleRate);
    };

    const auto dry = render (0.0f);
    const auto wet = render (100.0f);

    EXPECT_TRUE (ktest::isFinite (wet));

    // The chorus must actually do something...
    float worst = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        worst = juce::jmax (worst, std::abs (dry.getSample (0, i) - wet.getSample (0, i)));

    EXPECT_MSG (worst > 0.01f, "chorus at 100% changed nothing");

    // ...without becoming a volume control (linear mix rule at mix 0.5).
    constexpr int steadyStart = 4800;
    const auto shiftDb = juce::Decibels::gainToDecibels (
        rmsOfWindow (wet, steadyStart, 32768) / rmsOfWindow (dry, steadyStart, 32768));

    EXPECT_MSG (std::abs (shiftDb) < 3.0f,
                "chorus is a volume control: " + juce::String (shiftDb, 2) + " dB shift");

    // And no clicks from engaging it.
    EXPECT_TRUE (ktest::maxDiscontinuity (wet) < 0.25f);
}

KTEST_CASE (fx_airMixProducesADecayingTail)
{
    // Air is a send-return: the dry path is untouched, and the reverb tail
    // rings after the amp envelope has fully released.
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 192000; // 4s; note-off at 128000, dry dead ~160000

    auto render = [&] (float mixPercent)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorASource (proc, sampleRate);
        setParam (proc, params::airSize, 60.0f);
        setParam (proc, params::airMix, mixPercent);
        return renderMidi (proc, { 60 }, numSamples, 512, sampleRate);
    };

    const auto dry = render (0.0f);
    const auto wet = render (70.0f);

    EXPECT_TRUE (ktest::isFinite (wet));

    // After the release tail (600ms default) is long gone, only the reverb rings.
    const auto dryTail = rmsOfWindow (dry, 176000, 8000);
    const auto wetTailEarly = rmsOfWindow (wet, 176000, 8000);
    const auto wetTailLate = rmsOfWindow (wet, 184000, 8000);

    EXPECT_MSG (dryTail < 1.0e-6f,
                "dry render still sounding in the tail window: " + juce::String (dryTail, 9));
    EXPECT_MSG (wetTailEarly > 1.0e-4f,
                "no reverb tail: " + juce::String (wetTailEarly, 9));
    EXPECT_MSG (wetTailLate < wetTailEarly,
                "reverb tail is not decaying");
}

KTEST_CASE (fx_perVoiceReverbSendIsAudibleAtMixZero)
{
    // THE M4->M5 contract: the Reverb Mix mod destination is a per-voice wet
    // send. With the Air Mix knob fully down, a Velocity->Reverb Mix routing
    // must still put this voice into the reverb.
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 192000;

    auto render = [&] (bool routed)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorASource (proc, sampleRate);
        setParam (proc, params::airSize, 60.0f); // Mix stays at default 0

        if (routed)
            setModSlot (proc, 0, 4 /*Velocity*/, 11 /*Reverb Mix*/, 100.0f);

        return renderMidi (proc, { 60 }, numSamples, 512, sampleRate);
    };

    const auto unrouted = render (false);
    const auto routed = render (true);

    EXPECT_TRUE (ktest::isFinite (routed));

    const auto unroutedTail = rmsOfWindow (unrouted, 176000, 8000);
    const auto routedTail = rmsOfWindow (routed, 176000, 8000);

    EXPECT_MSG (unroutedTail < 1.0e-6f,
                "reverb sounding with nothing routed and Mix 0: " + juce::String (unroutedTail, 9));
    EXPECT_MSG (routedTail > 1.0e-4f,
                "per-voice send inaudible at Mix 0: " + juce::String (routedTail, 9));
}

KTEST_CASE (fx_closingTheMixKnobDoesNotCutTheTail)
{
    // Deactivation rings out: the reverb keeps processing until its own
    // output decays, so automating Air Mix to zero never guillotines a tail.
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    constexpr int numBlocks = 375; // 4s

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, blockSize);
    giveProcessorASource (proc, sampleRate);
    setParam (proc, params::airSize, 70.0f);
    setParam (proc, params::airMix, 70.0f);

    juce::AudioBuffer<float> out (2, numBlocks * blockSize);
    out.clear();
    juce::AudioBuffer<float> block (2, blockSize);

    constexpr int noteOffBlock = 180;  // ~1.9s: leave time for the amp release
    constexpr int mixZeroBlock = 300;  // ~3.2s: dry long dead, tail ringing

    for (int b = 0; b < numBlocks; ++b)
    {
        if (b == mixZeroBlock)
            setParam (proc, params::airMix, 0.0f);

        juce::MidiBuffer midi;

        if (b == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);

        if (b == noteOffBlock)
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);

        block.clear();
        proc.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, b * blockSize, block, ch, 0, blockSize);
    }

    EXPECT_TRUE (ktest::isFinite (out));

    // The tail must survive the knob hitting zero...
    const auto afterKnob = rmsOfWindow (out, (mixZeroBlock + 4) * blockSize, 8000);
    EXPECT_MSG (afterKnob > 1.0e-5f,
                "tail cut when Mix hit 0: " + juce::String (afterKnob, 9));

    // ...and still decay rather than ring forever.
    const auto muchLater = rmsOfWindow (out, (numBlocks - 16) * blockSize, 8000);
    EXPECT_MSG (muchLater < afterKnob, "tail is not decaying after Mix closed");
}

KTEST_CASE (processor_blockSizeIndependentWithFxActive)
{
    // The whole FX chain - saturation crossfade, chorus, reverb send-return,
    // per-voice sends - must preserve the bit-identical block-size guarantee.
    // This is what the smoother-snapping discipline in FxChain::prepare buys.
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 12288;

    auto render = [&] (int blockSize)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        giveProcessorASource (proc, sampleRate);
        proc.extractNow();
        setParam (proc, params::focus, 0.5f);
        setParam (proc, params::warmthAmount, 40.0f);
        setParam (proc, params::chorusAmount, 60.0f);
        setParam (proc, params::airSize, 50.0f);
        setParam (proc, params::airMix, 40.0f);
        setModSlot (proc, 0, 4 /*Velocity*/, 11 /*Reverb Mix*/, 80.0f);
        return renderMidi (proc, { 60 }, numSamples, blockSize, sampleRate);
    };

    const auto a = render (64);
    const auto b = render (1024);

    EXPECT_TRUE (a.getMagnitude (0, numSamples) > 0.001f);

    float worst = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        worst = juce::jmax (worst, std::abs (a.getSample (0, i) - b.getSample (0, i)));

    EXPECT_MSG (! (worst > 0.0f),
                "FX-active render depends on block size: diff " + juce::String (worst, 8));
}

KTEST_CASE (fx_sweepingTheKnobsIsClickFree)
{
    // Automating Warmth/Chorus/Air Mix through zero and back must not click:
    // the warmth crossfade is smoothed, the chorus cools down before its hard
    // bypass, and the reverb rings out instead of cutting.
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    constexpr int numBlocks = 375; // 2s

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, blockSize);
    giveProcessorASource (proc, sampleRate);

    juce::AudioBuffer<float> out (2, numBlocks * blockSize);
    out.clear();
    juce::AudioBuffer<float> block (2, blockSize);

    for (int b = 0; b < numBlocks; ++b)
    {
        // 0 -> max -> 0 over the render, hitting exact zero at both ends.
        const auto sweep = 100.0f * std::sin (juce::MathConstants<float>::pi
                                              * (float) b / (float) numBlocks);
        setParam (proc, params::warmthAmount, sweep);
        setParam (proc, params::chorusAmount, sweep);
        setParam (proc, params::airMix, sweep);

        juce::MidiBuffer midi;

        if (b == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);

        block.clear();
        proc.processBlock (block, midi);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, b * blockSize, block, ch, 0, blockSize);
    }

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_TRUE (out.getMagnitude (0, out.getNumSamples()) > 0.001f);

    const auto worstStep = ktest::maxDiscontinuity (out);
    EXPECT_MSG (worstStep < 0.25f,
                "FX sweep clicks: step " + juce::String (worstStep, 3));
}

// =============================================================================
// M5: presets and Randomize
// =============================================================================

namespace
{
    /** A scratch preset directory, deleted on destruction. */
    struct ScratchPresetDir
    {
        ScratchPresetDir()
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("keepsake-preset-tests")
                       .getNonexistentSibling())
        {
            dir.createDirectory();
        }

        ~ScratchPresetDir() { dir.deleteRecursively(); }

        juce::File dir;
    };
}

KTEST_CASE (presets_saveAndLoadRoundTripIncludingTheKeepsake)
{
    constexpr double sampleRate = 48000.0;
    ScratchPresetDir scratch;

    // Author a patch with a source loaded and distinctive settings...
    KeepsakeProcessor author;
    author.prepareToPlay (sampleRate, 512);
    giveProcessorASource (author, sampleRate);
    setParam (author, params::focus, 0.7f);
    setParam (author, params::warmthAmount, 33.0f);

    PresetManager authorPresets (author, scratch.dir);
    EXPECT_TRUE (authorPresets.savePreset ("My Moment"));
    EXPECT_TRUE (authorPresets.getCurrentName() == "My Moment");
    EXPECT_TRUE (author.getPresetDisplayName() == "My Moment");

    // ...and load it into a fresh instance: parameters AND the embedded audio
    // must arrive (spec §3: "Presets embed capture audio").
    KeepsakeProcessor player;
    player.prepareToPlay (sampleRate, 512);

    PresetManager playerPresets (player, scratch.dir);
    EXPECT_TRUE (playerPresets.getPresetNames().size() == 1);
    EXPECT_TRUE (playerPresets.loadPreset ("My Moment"));

    EXPECT_NEAR ((double) player.getState().getRawParameterValue (params::focus)->load(), 0.7, 1.0e-4);
    EXPECT_NEAR ((double) player.getState().getRawParameterValue (params::warmthAmount)->load(), 33.0, 1.0e-3);
    EXPECT_TRUE (player.getPresetDisplayName() == "My Moment");

    const auto out = renderMidi (player, { 60 }, 12000, 512, sampleRate);
    EXPECT_MSG (out.getMagnitude (0, out.getNumSamples()) > 0.001f,
                "loaded preset does not play - embedded audio missing");
}

KTEST_CASE (presets_missingParametersLoadAsDefaultsNotStaleValues)
{
    // An older preset (same state version, fewer parameters - exactly what an
    // M4-era session is) must load its missing parameters at their DEFAULTS.
    // Without the default-merge in setStateInformation, replaceState leaves
    // absent parameters at whatever they were before the load - stale state
    // bleeding across preset switches.
    constexpr double sampleRate = 48000.0;
    ScratchPresetDir scratch;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 512);
    setParam (proc, params::focus, 0.7f);

    // Craft the "old" blob: current state minus the warmthAmount child.
    juce::MemoryBlock blob;
    proc.getStateInformation (blob);

    juce::ValueTree tree;
    {
        juce::MemoryInputStream in (blob.getData(), blob.getSize(), false);
        in.readInt(); in.readInt(); in.readInt(); // magic/version/length
        tree = juce::ValueTree::readFromStream (in);
    }

    auto child = tree.getChildWithProperty ("id", juce::String (params::warmthAmount));
    EXPECT_TRUE (child.isValid());
    tree.removeChild (child, nullptr);

    juce::MemoryBlock oldBlob;
    {
        juce::MemoryOutputStream stream (oldBlob, false);
        stream.writeInt ((int) KeepsakeProcessor::kStateMagic);
        stream.writeInt (KeepsakeProcessor::kStateVersion);

        juce::MemoryBlock payload;
        {
            juce::MemoryOutputStream payloadStream (payload, false);
            tree.writeToStream (payloadStream);
        }

        stream.writeInt ((int) payload.getSize());
        stream.write (payload.getData(), payload.getSize());
    }

    const auto file = scratch.dir.getChildFile (juce::String ("Old") + PresetManager::kExtension);
    EXPECT_TRUE (file.replaceWithData (oldBlob.getData(), oldBlob.getSize()));

    // Dirty the parameter the preset lacks, then load.
    setParam (proc, params::warmthAmount, 80.0f);
    setParam (proc, params::focus, 0.1f);

    PresetManager presets (proc, scratch.dir);
    EXPECT_TRUE (presets.loadPreset ("Old"));

    EXPECT_NEAR ((double) proc.getState().getRawParameterValue (params::focus)->load(), 0.7, 1.0e-4);
    EXPECT_MSG (proc.getState().getRawParameterValue (params::warmthAmount)->load() < 1.0e-6f,
                "missing parameter kept its stale value instead of its default");
}

KTEST_CASE (presets_prevNextCycleThroughSortedNames)
{
    constexpr double sampleRate = 48000.0;
    ScratchPresetDir scratch;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, 512);

    PresetManager presets (proc, scratch.dir);
    EXPECT_TRUE (! presets.loadNext()); // empty directory: nothing to step to

    for (const auto* name : { "Beta", "Alpha", "Gamma" })
        EXPECT_TRUE (presets.savePreset (name));

    // Sorted order regardless of save order.
    EXPECT_TRUE (presets.getPresetNames()[0] == "Alpha");
    EXPECT_TRUE (presets.getPresetNames()[1] == "Beta");
    EXPECT_TRUE (presets.getPresetNames()[2] == "Gamma");

    // Save left us on Gamma; next wraps to the start.
    EXPECT_TRUE (presets.loadNext());
    EXPECT_TRUE (presets.getCurrentName() == "Alpha");
    EXPECT_TRUE (presets.loadNext());
    EXPECT_TRUE (presets.getCurrentName() == "Beta");
    EXPECT_TRUE (presets.loadPrevious());
    EXPECT_TRUE (presets.getCurrentName() == "Alpha");
    EXPECT_TRUE (presets.loadPrevious()); // wraps backward
    EXPECT_TRUE (presets.getCurrentName() == "Gamma");
}

KTEST_CASE (randomize_rollsEverythingExceptMasterGainAndUndoRestoresExactly)
{
    KeepsakeProcessor proc;

    setParam (proc, params::masterGain, -12.0f);

    // Remember every normalized value, then roll.
    std::vector<float> before;
    for (auto* p : proc.getParameters())
        before.push_back (p->getValue());

    proc.randomizeParameters (0x5eed);

    const auto& parameters = proc.getParameters();
    int changed = 0;

    for (int i = 0; i < parameters.size(); ++i)
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameters[i]);

        if (ranged != nullptr && ranged->paramID == juce::String (params::masterGain))
        {
            EXPECT_MSG (juce::exactlyEqual (parameters[i]->getValue(), before[(size_t) i]),
                        "master gain was randomized (ear protection)");
            continue;
        }

        if (! juce::exactlyEqual (parameters[i]->getValue(), before[(size_t) i]))
            ++changed;
    }

    // Choice parameters can roll their own value by chance; "almost all
    // changed" is the honest assertion.
    EXPECT_MSG (changed > parameters.size() * 3 / 4,
                "randomize changed only " + juce::String (changed) + " of "
                    + juce::String (parameters.size()) + " parameters");

    // Undo restores every value (to within one convertFrom0to1 round trip -
    // skewed ranges are not bit-exact through the parameter system), and a
    // second undo has nothing.
    EXPECT_TRUE (proc.undoRandomize());

    for (int i = 0; i < parameters.size(); ++i)
        EXPECT_MSG (std::abs (parameters[i]->getValue() - before[(size_t) i]) < 1.0e-6f,
                    "undo failed to restore parameter " + juce::String (i));

    EXPECT_TRUE (! proc.undoRandomize());
}

KTEST_CASE (randomize_isSafeDuringPlayback)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;

    KeepsakeProcessor proc;
    proc.prepareToPlay (sampleRate, blockSize);
    giveProcessorASource (proc, sampleRate);
    proc.extractNow();

    juce::AudioBuffer<float> block (2, blockSize);
    bool sounded = false;

    for (int b = 0; b < 150; ++b)
    {
        if (b == 50)
        {
            proc.randomizeParameters (0xabc + b);
            proc.extractNow(); // what the poller would do moments later
        }

        if (b == 100)
            proc.undoRandomize();

        juce::MidiBuffer midi;

        if (b == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);

        block.clear();
        proc.processBlock (block, midi);

        EXPECT_TRUE (ktest::isFinite (block));
        sounded = sounded || block.getMagnitude (0, blockSize) > 0.001f;
    }

    EXPECT_TRUE (sounded);
}

// =============================================================================
// Tempo-synced grain emission
// =============================================================================

namespace
{
    /** Median spacing between amplitude-envelope peaks, in samples. */
    double medianPeakSpacing (const juce::AudioBuffer<float>& buffer,
                              int start, int end, int frameSize)
    {
        // Coarse rectified envelope, then local maxima above half the global max.
        std::vector<float> envelope;

        for (int pos = start; pos + frameSize <= end; pos += frameSize)
        {
            float peak = 0.0f;
            const auto* d = buffer.getReadPointer (0);

            for (int i = pos; i < pos + frameSize; ++i)
                peak = juce::jmax (peak, std::abs (d[i]));

            envelope.push_back (peak);
        }

        const auto globalMax = *std::max_element (envelope.begin(), envelope.end());
        const auto threshold = globalMax * 0.5f;

        std::vector<double> onsets;
        bool above = false;

        for (size_t i = 0; i < envelope.size(); ++i)
        {
            if (! above && envelope[i] > threshold)
            {
                onsets.push_back ((double) start + (double) i * frameSize);
                above = true;
            }
            else if (above && envelope[i] < threshold * 0.5f)
            {
                above = false;
            }
        }

        if (onsets.size() < 3)
            return 0.0;

        std::vector<double> gaps;
        for (size_t i = 1; i < onsets.size(); ++i)
            gaps.push_back (onsets[i] - onsets[i - 1]);

        std::sort (gaps.begin(), gaps.end());
        return gaps[gaps.size() / 2];
    }

    /** Largest deviation of any peak gap from the median - a jittered train
        has the same MEDIAN as an exact grid; this is what tells them apart. */
    double worstPeakGapDeviation (const juce::AudioBuffer<float>& buffer,
                                  int start, int end, int frameSize)
    {
        const auto median = medianPeakSpacing (buffer, start, end, frameSize);

        // Recompute the onsets the same way and compare every gap.
        std::vector<float> envelope;

        for (int pos = start; pos + frameSize <= end; pos += frameSize)
        {
            float peak = 0.0f;
            const auto* d = buffer.getReadPointer (0);

            for (int i = pos; i < pos + frameSize; ++i)
                peak = juce::jmax (peak, std::abs (d[i]));

            envelope.push_back (peak);
        }

        const auto globalMax = *std::max_element (envelope.begin(), envelope.end());
        const auto threshold = globalMax * 0.5f;

        std::vector<double> onsets;
        bool above = false;

        for (size_t i = 0; i < envelope.size(); ++i)
        {
            if (! above && envelope[i] > threshold)
            {
                onsets.push_back ((double) start + (double) i * frameSize);
                above = true;
            }
            else if (above && envelope[i] < threshold * 0.5f)
            {
                above = false;
            }
        }

        double worst = 0.0;
        for (size_t i = 1; i < onsets.size(); ++i)
            worst = juce::jmax (worst, std::abs ((onsets[i] - onsets[i - 1]) - median));

        return worst;
    }
}

KTEST_CASE (grains_syncedEmissionPulsesOnTheBeat)
{
    // Sync on, 1/8 at the harness's deterministic 120 BPM fallback: grains
    // must pulse every 0.25s = 12000 samples at 48k, anchored to note-on.
    // Short grains with silence between pulses make the grid measurable.
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 144000; // 3s

    auto render = [&] (bool synced)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorASource (proc, sampleRate);
        setParam (proc, params::grainSize, 20.0f);
        setParam (proc, params::grainDrift, 0.0f);

        if (synced)
        {
            setParam (proc, params::grainSync, 1.0f);
            setParam (proc, params::grainDivision, 7.0f); // 1/8
        }

        return renderMidi (proc, { 60 }, numSamples, 512, sampleRate);
    };

    const auto pulsed = render (true);
    EXPECT_TRUE (ktest::isFinite (pulsed));

    // Steady-state region only (skip attack, stop before the note-off).
    const auto spacing = medianPeakSpacing (pulsed, 24000, 90000, 480);
    EXPECT_MSG (std::abs (spacing - 12000.0) < 600.0,
                "synced grains not on the 1/8 grid: median spacing "
                    + juce::String (spacing, 1) + " samples (expected ~12000)");

    // The grid must be EXACT, not merely correct on average - a jittered
    // train shares the median but not the regularity (onsets are quantized
    // to the 480-sample analysis frames; two frames of slack).
    const auto deviation = worstPeakGapDeviation (pulsed, 24000, 90000, 480);
    EXPECT_MSG (deviation <= 960.0,
                "synced grain grid is irregular: worst gap deviation "
                    + juce::String (deviation, 1) + " samples");

    // Negative control: free mode at default density (24/s, jittered) must
    // NOT show the 1/8 grid - proves the detector distinguishes the modes.
    const auto free_ = render (false);
    const auto freeSpacing = medianPeakSpacing (free_, 24000, 90000, 480);
    EXPECT_MSG (std::abs (freeSpacing - 12000.0) > 600.0,
                "free-mode render looks identical to the synced grid: spacing "
                    + juce::String (freeSpacing, 1));
}

KTEST_CASE (processor_blockSizeIndependentWithSyncedGrains)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 12288;

    auto render = [&] (int blockSize)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        giveProcessorASource (proc, sampleRate);
        setParam (proc, params::grainSync, 1.0f);
        setParam (proc, params::grainDivision, 9.0f); // 1/16
        return renderMidi (proc, { 60 }, numSamples, blockSize, sampleRate);
    };

    const auto a = render (64);
    const auto b = render (1024);

    EXPECT_TRUE (a.getMagnitude (0, numSamples) > 0.001f);

    float worst = 0.0f;
    for (int i = 0; i < numSamples; ++i)
        worst = juce::jmax (worst, std::abs (a.getSample (0, i) - b.getSample (0, i)));

    EXPECT_MSG (! (worst > 0.0f),
                "synced-grain render depends on block size: diff " + juce::String (worst, 8));
}

KTEST_CASE (processor_modWheelReachesTheMatrix)
{
    constexpr double sampleRate = 48000.0;

    auto render = [&] (int wheelValue)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 256);
        giveProcessorASource (proc, sampleRate);
        proc.extractNow();
        setParam (proc, params::focus, 0.0f);
        setModSlot (proc, 0, 5 /*ModWheel*/, 1 /*Focus*/, 100.0f);

        juce::AudioBuffer<float> out (2, 24000);
        out.clear();
        juce::AudioBuffer<float> block (2, 256);
        bool sent = false;

        for (int pos = 0; pos < out.getNumSamples(); pos += 256)
        {
            const auto n = juce::jmin (256, out.getNumSamples() - pos);

            juce::MidiBuffer midi;

            if (! sent)
            {
                midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, wheelValue), 0);
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 1);
                sent = true;
            }

            block.setSize (2, n, false, false, true);
            block.clear();
            proc.processBlock (block, midi);

            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, pos, block, ch, 0, n);
        }

        return out;
    };

    // Wheel at 0: focus stays 0 (pure Cloud). Wheel at 127: focus pushed to 1
    // (pure Tone). The two renders must differ substantially.
    const auto wheelDown = render (0);
    const auto wheelUp = render (127);

    EXPECT_TRUE (ktest::isFinite (wheelDown) && ktest::isFinite (wheelUp));
    EXPECT_TRUE (wheelUp.getMagnitude (0, 24000) > 0.001f);

    float diff = 0.0f;
    for (int i = 0; i < 24000; ++i)
        diff = juce::jmax (diff, std::abs (wheelDown.getSample (0, i) - wheelUp.getSample (0, i)));

    EXPECT_MSG (diff > 0.01f, "mod wheel had no effect through the matrix");
}


// =============================================================================
// Warp (tempo-mapped Keep window) + transient Snap
// =============================================================================

namespace
{
    /** Decaying noise bursts over a -60dB floor - source material with
        unambiguous transients at known positions. */
    SourceAudio::Ptr makeClickSource (const std::vector<int>& positions,
                                      int numSamples, double sampleRate)
    {
        SourceAudio::Ptr source (new SourceAudio());
        source->sampleRate = sampleRate;
        source->name = "clicks";
        source->buffer.setSize (1, numSamples);

        juce::Random rng (42);
        auto* data = source->buffer.getWritePointer (0);

        for (int i = 0; i < numSamples; ++i)
            data[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.001f;

        const auto burstLength = (int) (0.01 * sampleRate);

        for (auto pos : positions)
            for (int i = 0; i < burstLength && pos + i < numSamples; ++i)
            {
                const auto decay = 1.0f - (float) i / (float) burstLength;
                data[pos + i] += (rng.nextFloat() * 2.0f - 1.0f) * 0.8f * decay;
            }

        return source;
    }

    float maxAbsDifference (const juce::AudioBuffer<float>& a,
                            const juce::AudioBuffer<float>& b, int numSamples)
    {
        float diff = 0.0f;

        for (int i = 0; i < numSamples; ++i)
            diff = juce::jmax (diff, std::abs (a.getSample (0, i) - b.getSample (0, i)));

        return diff;
    }
}

KTEST_CASE (grains_warpSweepsTheKeepWindowAtTempo)
{
    // Source = exactly one Keep window: 250ms of 220Hz then 250ms of 880Hz.
    // Warp "1 Bar" at the harness's 120 BPM fallback sweeps it in 2s, so the
    // note must OPEN on 220Hz material and be playing 880Hz material past the
    // half-bar - at the note's own pitch (ratio 1), proving a time-stretch.
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 144000; // 3s; renderMidi releases at 2s

    SourceAudio::Ptr split (new SourceAudio());
    split->sampleRate = sampleRate;
    split->name = "split-tone";
    split->buffer.setSize (1, 24000);

    auto* data = split->buffer.getWritePointer (0);

    for (int i = 0; i < 24000; ++i)
    {
        const auto hz = i < 12000 ? 220.0 : 880.0;
        data[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz
                                           * (double) i / sampleRate);
    }

    auto render = [&] (int warpIndex)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorThisSource (proc, sampleRate, *split);
        setParam (proc, params::place, 0.0f);
        setParam (proc, params::captureLength, 500.0f);
        setParam (proc, params::rootNote, 60.0f);
        setParam (proc, params::grainSize, 30.0f);
        setParam (proc, params::grainDrift, 0.0f);
        setParam (proc, params::grainShimmer, 0.0f);
        setParam (proc, params::grainSpread, 0.0f);
        setParam (proc, params::warpMode, (float) warpIndex);
        return renderMidi (proc, { 60 }, numSamples, 512, sampleRate);
    };

    const auto warped = render (3); // "1 Bar"
    EXPECT_TRUE (ktest::isFinite (warped));

    const auto early = ktest::dominantHz (warped, 14400, 13, sampleRate); // 0.3s in
    const auto late = ktest::dominantHz (warped, 62400, 13, sampleRate);  // 1.3s in
    EXPECT_MSG (std::abs (early - 220.0) < 30.0,
                "warp start not on the window's opening material: "
                    + juce::String (early, 1) + " Hz");
    EXPECT_MSG (std::abs (late - 880.0) < 60.0,
                "warp did not reach the window's later material: "
                    + juce::String (late, 1) + " Hz");

    // Negative control: Warp Off with Drift 0 never leaves the window start.
    const auto still = render (0);
    const auto stillLate = ktest::dominantHz (still, 62400, 13, sampleRate);
    EXPECT_MSG (std::abs (stillLate - 220.0) < 30.0,
                "warp-off render moved through the window: "
                    + juce::String (stillLate, 1) + " Hz");
}

KTEST_CASE (grains_snapQuantizesGrainStartsToTransients)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 48000;

    // Hits at 50ms and 300ms sit inside the 500ms Keep window; 800ms is outside.
    auto clicks = makeClickSource ({ 2400, 14400, 38400 }, 48000, sampleRate);

    auto render = [&] (bool snap, float driftPercent)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorThisSource (proc, sampleRate, *clicks);
        setParam (proc, params::place, 0.0f);
        setParam (proc, params::captureLength, 500.0f);
        setParam (proc, params::grainSize, 20.0f);
        setParam (proc, params::grainDrift, driftPercent);
        setParam (proc, params::grainSpread, 0.0f);
        setParam (proc, params::grainSnap, snap ? 1.0f : 0.0f);
        return renderMidi (proc, { 60 }, numSamples, 512, sampleRate);
    };

    // Drift 0, Snap off: every grain reads the window start (noise floor).
    // Snap on: starts quantise to the first hit - audibly different material.
    const auto off = render (false, 0.0f);
    const auto on = render (true, 0.0f);
    EXPECT_TRUE (ktest::isFinite (off) && ktest::isFinite (on));
    EXPECT_MSG (maxAbsDifference (off, on, numSamples) > 1.0e-4f,
                "snap changed nothing - grains are not landing on hits");

    // Full Drift with Snap on shuffles BETWEEN hits rather than jittering
    // freely - it must differ from the pinned drift-0 snap render.
    const auto shuffled = render (true, 100.0f);
    EXPECT_MSG (maxAbsDifference (on, shuffled, numSamples) > 1.0e-4f,
                "drift does not shuffle snapped grains between hits");
}

KTEST_CASE (processor_blockSizeIndependentWithWarpAndSnap)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 12288;

    auto clicks = makeClickSource ({ 2400, 14400 }, 48000, sampleRate);

    auto render = [&] (int blockSize)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        giveProcessorThisSource (proc, sampleRate, *clicks);
        setParam (proc, params::warpMode, 3.0f);   // 1 Bar
        setParam (proc, params::grainSnap, 1.0f);  // Transients
        setParam (proc, params::grainSync, 1.0f);
        setParam (proc, params::grainDivision, 9.0f); // 1/16
        setParam (proc, params::grainDrift, 70.0f);   // exercise the shuffle RNG
        return renderMidi (proc, { 60 }, numSamples, blockSize, sampleRate);
    };

    const auto a = render (64);
    const auto b = render (1024);

    EXPECT_TRUE (a.getMagnitude (0, numSamples) > 0.001f);

    const auto worst = maxAbsDifference (a, b, numSamples);
    EXPECT_MSG (! (worst > 0.0f),
                "warp/snap render depends on block size: diff " + juce::String (worst, 8));
}

KTEST_CASE (processor_randomizeLeavesTuningAlone)
{
    KeepsakeProcessor proc;

    setParam (proc, params::rootNote, 48.0f);
    setParam (proc, params::rootCents, -20.0f);

    auto normalised = [&] (const char* id)
    {
        return proc.getState().getParameter (id)->getValue();
    };

    const auto rootBefore = normalised (params::rootNote);
    const auto centsBefore = normalised (params::rootCents);
    const auto gainBefore = normalised (params::masterGain);
    const auto placeBefore = normalised (params::place);
    const auto sizeBefore = normalised (params::grainSize);

    proc.randomizeParameters (1234);

    // A random Root retunes the whole instrument - every roll would sound
    // wrong regardless of texture, so tuning (and the output level) hold.
    EXPECT_TRUE (std::abs (normalised (params::rootNote) - rootBefore) < 1.0e-9f);
    EXPECT_TRUE (std::abs (normalised (params::rootCents) - centsBefore) < 1.0e-9f);
    EXPECT_TRUE (std::abs (normalised (params::masterGain) - gainBefore) < 1.0e-9f);

    // ...while the texture itself really did roll.
    EXPECT_MSG (std::abs (normalised (params::place) - placeBefore) > 1.0e-6f
                    || std::abs (normalised (params::grainSize) - sizeBefore) > 1.0e-6f,
                "randomize changed nothing at all");
}

KTEST_CASE (grains_formantModeHoldsTimbreWhileKeysSetPitch)
{
    // Root A2 with a 220Hz source, played two octaves up at A4. Repitch mode
    // must chipmunk the content to 880Hz. Formant mode keeps grains at their
    // original rate - the spectrum stays anchored low - while the 440Hz
    // emission clock imposes the played pitch as a spectral line.
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 144000;

    auto render = [&] (bool formant)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, 512);
        giveProcessorASource (proc, sampleRate); // 220Hz sine
        setParam (proc, params::rootNote, 45.0f); // A2 = 110Hz
        setParam (proc, params::grainDrift, 0.0f);
        setParam (proc, params::grainShimmer, 0.0f);
        setParam (proc, params::grainSpread, 0.0f);
        setParam (proc, params::pitchMode, formant ? 1.0f : 0.0f);
        return renderMidi (proc, { 69 }, numSamples, 512, sampleRate); // A4
    };

    const auto repitched = render (false);
    const auto held = render (true);
    EXPECT_TRUE (ktest::isFinite (repitched) && ktest::isFinite (held));

    // Sanity: repitch really is +2 octaves on the content.
    const auto chipmunk = ktest::dominantHz (repitched, 24000, 15, sampleRate);
    EXPECT_MSG (std::abs (chipmunk - 880.0) < 30.0,
                "repitch control wrong: " + juce::String (chipmunk, 1) + " Hz");

    // Formant mode: the strongest component must stay well below the
    // chipmunked 880 - the source timbre did not ride up the keyboard.
    const auto anchored = ktest::dominantHz (held, 24000, 15, sampleRate);
    EXPECT_MSG (anchored < 660.0,
                "formant mode still chipmunks: dominant "
                    + juce::String (anchored, 1) + " Hz");

    // ...and the played pitch is present as a real spectral line: the bin at
    // 440Hz must sit within 15dB of the spectrum's peak.
    const auto spectrum = ktest::powerSpectrumDb (held, 24000, 15);
    const auto binAt = [&] (double hz) { return (int) std::round (hz / sampleRate * 32768.0); };

    float peakDb = -300.0f;
    int peakBin = 2;

    for (int k = 2; k < (int) spectrum.size(); ++k)
        if (spectrum[(size_t) k] > peakDb)
        {
            peakDb = spectrum[(size_t) k];
            peakBin = k;
        }

    float lineDb = -300.0f;
    for (int k = binAt (440.0) - 2; k <= binAt (440.0) + 2; ++k)
        lineDb = juce::jmax (lineDb, spectrum[(size_t) k]);

    juce::String topPeaks;
    {
        // Local maxima at least 6 bins apart, top five by level.
        std::vector<std::pair<float, int>> maxima;

        for (int k = 4; k < (int) spectrum.size() - 1; ++k)
            if (spectrum[(size_t) k] > spectrum[(size_t) (k - 1)]
                && spectrum[(size_t) k] >= spectrum[(size_t) (k + 1)])
                maxima.push_back ({ spectrum[(size_t) k], k });

        std::sort (maxima.begin(), maxima.end(), std::greater<>());

        for (size_t i = 0; i < juce::jmin ((size_t) 5, maxima.size()); ++i)
            topPeaks << juce::String ((double) maxima[i].second * sampleRate / 32768.0, 1)
                     << "Hz@" << juce::String (maxima[i].first, 1) << "dB ";
    }

    EXPECT_MSG (lineDb > peakDb - 15.0f,
                "played pitch missing from formant render: 440Hz line is "
                    + juce::String (peakDb - lineDb, 1) + " dB under the peak at "
                    + juce::String ((double) peakBin * sampleRate / 32768.0, 1)
                    + " Hz (dominant probe said " + juce::String (anchored, 1)
                    + " Hz); top peaks: " + topPeaks);
}

KTEST_CASE (processor_blockSizeIndependentWithFormantAndWarp)
{
    constexpr double sampleRate = 48000.0;
    constexpr int numSamples = 12288;

    auto render = [&] (int blockSize)
    {
        KeepsakeProcessor proc;
        proc.prepareToPlay (sampleRate, blockSize);
        giveProcessorASource (proc, sampleRate);
        setParam (proc, params::pitchMode, 1.0f);
        setParam (proc, params::warpMode, 3.0f); // 1 Bar
        return renderMidi (proc, { 72 }, numSamples, blockSize, sampleRate);
    };

    const auto a = render (64);
    const auto b = render (1024);

    EXPECT_TRUE (a.getMagnitude (0, numSamples) > 0.001f);

    const auto worst = maxAbsDifference (a, b, numSamples);
    EXPECT_MSG (! (worst > 0.0f),
                "formant/warp render depends on block size: diff " + juce::String (worst, 8));
}
