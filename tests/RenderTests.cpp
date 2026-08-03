#include "TestHarness.h"
#include "TestUtils.h"
#include "PluginProcessor.h"

using namespace keepsake;

namespace
{
    /** Loads a synthetic keepsake into a processor without touching the filesystem. */
    void giveProcessorASource (KeepsakeProcessor& proc, double sampleRate)
    {
        auto source = ktest::makeSineSource (220.0, 2.0, sampleRate, "test-tone");

        // Round-trip through the real serialisation path so the tests exercise the
        // same code a restored preset would.
        const auto encoded = CaptureIO::encodeToBase64 (*source);

        auto state = proc.getState().copyState();
        state.setProperty ("audioData", encoded, nullptr);
        state.setProperty ("audioName", "test-tone", nullptr);
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
        params::ampAttack, params::ampDecay, params::ampSustain, params::ampRelease,
        params::masterGain
    };

    for (const auto* id : expected)
        EXPECT_MSG (proc.getState().getParameter (id) != nullptr,
                    juce::String ("missing parameter: ") + id);

    EXPECT_TRUE (proc.getParameters().size() == (int) std::size (expected));

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
