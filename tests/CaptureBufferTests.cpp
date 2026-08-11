#include "TestHarness.h"
#include "TestUtils.h"

using namespace keepsake;

KTEST_CASE (captureWindow_clampsToSourceBounds)
{
    auto source = ktest::makeSineSource (440.0, 1.0, 48000.0);

    // Place at the very end must not push the window past the end of the file.
    const auto w = CaptureWindow::resolve (*source, 1.0, 500.0, 48000.0);

    EXPECT_TRUE (w.startSample >= 0);
    EXPECT_TRUE (w.endSample() <= source->getNumSamples());
    EXPECT_TRUE (w.numSamples == 24000); // 500ms at 48k
}

KTEST_CASE (captureWindow_lengthLongerThanFileIsTruncated)
{
    auto source = ktest::makeSineSource (440.0, 0.05, 48000.0); // 50ms file

    const auto w = CaptureWindow::resolve (*source, 0.5, 500.0, 48000.0);

    EXPECT_TRUE (w.startSample == 0);
    EXPECT_TRUE (w.numSamples == source->getNumSamples());
}

KTEST_CASE (captureWindow_placeMovesTheStart)
{
    auto source = ktest::makeSineSource (440.0, 4.0, 48000.0);

    const auto atStart = CaptureWindow::resolve (*source, 0.0, 100.0, 48000.0);
    const auto atMiddle = CaptureWindow::resolve (*source, 0.5, 100.0, 48000.0);
    const auto atEnd = CaptureWindow::resolve (*source, 1.0, 100.0, 48000.0);

    EXPECT_TRUE (atStart.startSample == 0);
    EXPECT_TRUE (atMiddle.startSample > atStart.startSample);
    EXPECT_TRUE (atEnd.startSample > atMiddle.startSample);
    EXPECT_TRUE (atEnd.endSample() == source->getNumSamples());
}

KTEST_CASE (captureWindow_emptySourceIsSafe)
{
    SourceAudio::Ptr empty (new SourceAudio());
    empty->sampleRate = 48000.0;

    const auto w = CaptureWindow::resolve (*empty, 0.5, 100.0, 48000.0);

    EXPECT_TRUE (w.numSamples == 0);
}

/** Spec §2.1 / M1 exit test: the keepsake survives without the source file. */
KTEST_CASE (serialisation_roundTripsAudioThroughBase64)
{
    CaptureIO io;

    auto original = ktest::makeSineSource (440.0, 0.25, 48000.0, "original.wav");

    const auto encoded = CaptureIO::encodeToBase64 (*original);
    EXPECT_TRUE (encoded.isNotEmpty());

    auto restored = io.decodeFromBase64 (encoded, "original.wav", 48000.0, 48000.0);
    EXPECT_TRUE (restored != nullptr);

    if (restored == nullptr)
        return;

    EXPECT_TRUE (restored->getNumChannels() == 2);
    EXPECT_NEAR ((double) restored->getNumSamples(), (double) original->getNumSamples(), 2.0);

    // FLAC is lossless, but the writer quantises to 24 bit.
    constexpr float tolerance = 1.0e-4f;
    float worst = 0.0f;
    const auto n = juce::jmin (restored->getNumSamples(), original->getNumSamples());

    for (int i = 0; i < n; ++i)
        worst = juce::jmax (worst, std::abs (restored->buffer.getSample (0, i)
                                             - original->buffer.getSample (0, i)));

    EXPECT_MSG (worst < tolerance,
                "round-tripped audio differs by " + juce::String (worst, 8));
}

KTEST_CASE (serialisation_resamplesToTheHostRateOnRestore)
{
    CaptureIO io;

    auto original = ktest::makeSineSource (440.0, 0.5, 44100.0);
    const auto encoded = CaptureIO::encodeToBase64 (*original);

    auto restored = io.decodeFromBase64 (encoded, "x", 44100.0, 96000.0);

    EXPECT_TRUE (restored != nullptr);

    if (restored == nullptr)
        return;

    EXPECT_NEAR (restored->sampleRate, 96000.0, 0.001);

    // 0.5s at 96k, within a couple of samples of the interpolator's rounding.
    EXPECT_NEAR ((double) restored->getNumSamples(), 48000.0, 4.0);
}

KTEST_CASE (serialisation_emptyBlobDecodesToNull)
{
    CaptureIO io;
    EXPECT_TRUE (io.decodeFromBase64 ({}, "x", 48000.0, 48000.0) == nullptr);
    EXPECT_TRUE (io.decodeFromBase64 ("not valid base64 @@@@", "x", 48000.0, 48000.0) == nullptr);
}

KTEST_CASE (sourceStore_publishAndRetireKeepsOldPointerAlive)
{
    SourceStore store;

    EXPECT_FALSE (store.hasSource());

    auto first = ktest::makeSineSource (440.0, 0.1, 48000.0, "first");
    store.publish (first);

    auto* audioThreadView = store.getForAudioThread();
    EXPECT_TRUE (audioThreadView != nullptr);
    EXPECT_TRUE (store.getForMessageThread()->name == "first");

    // Swapping in a replacement must not invalidate a pointer the audio thread is
    // already holding - that is the whole point of the deferred-release scheme.
    store.publish (ktest::makeSineSource (220.0, 0.1, 48000.0, "second"));
    store.collectGarbage();

    EXPECT_TRUE (audioThreadView->name == "first");
    EXPECT_TRUE (store.getForMessageThread()->name == "second");
}

// =============================================================================
// Transient detection (Warp/Snap groundwork)
// =============================================================================

namespace
{
    /** Decaying noise bursts at the given positions over a -60dB noise floor. */
    juce::AudioBuffer<float> makeClickTrack (const std::vector<int>& positions,
                                             int numSamples, double sampleRate)
    {
        juce::AudioBuffer<float> buffer (1, numSamples);
        juce::Random rng (42);
        auto* data = buffer.getWritePointer (0);

        for (int i = 0; i < numSamples; ++i)
            data[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.001f;

        const auto burstLength = (int) (0.01 * sampleRate); // 10ms

        for (auto pos : positions)
            for (int i = 0; i < burstLength && pos + i < numSamples; ++i)
            {
                const auto decay = 1.0f - (float) i / (float) burstLength;
                data[pos + i] += (rng.nextFloat() * 2.0f - 1.0f) * 0.8f * decay;
            }

        return buffer;
    }
}

KTEST_CASE (transients_detectorFindsPlantedOnsets)
{
    constexpr double sampleRate = 48000.0;
    const std::vector<int> planted = { 24000, 48000, 72000 };

    const auto buffer = makeClickTrack (planted, 96000, sampleRate);
    const auto found = detectTransients (buffer, sampleRate);

    EXPECT_MSG ((int) found.size() == 3,
                "expected 3 onsets, found " + juce::String ((int) found.size()));

    // Each hit within 10ms of where it was planted, in order.
    for (size_t i = 0; i < juce::jmin (found.size(), planted.size()); ++i)
        EXPECT_MSG (std::abs (found[i] - planted[i]) <= 480,
                    "onset " + juce::String ((int) i) + " at " + juce::String (found[i])
                        + ", planted at " + juce::String (planted[i]));
}

KTEST_CASE (transients_silenceAndSteadyToneYieldNone)
{
    constexpr double sampleRate = 48000.0;

    juce::AudioBuffer<float> silence (1, 48000);
    silence.clear();
    EXPECT_TRUE (detectTransients (silence, sampleRate).empty());

    // A steady mid-file tone has no energy JUMP after its own attack - the
    // detector must not pepper sustained material with false hits.
    auto tone = ktest::makeSineSource (220.0, 1.0, sampleRate);
    const auto found = detectTransients (tone->buffer, sampleRate);
    EXPECT_MSG ((int) found.size() <= 1, // the attack itself may register
                "steady tone produced " + juce::String ((int) found.size()) + " onsets");
}

KTEST_CASE (transients_importPathsPopulateTheSource)
{
    // decodeFromBase64 is the restore path every preset goes through - the
    // detected hits must arrive with the source, in host-rate samples.
    constexpr double sampleRate = 48000.0;
    const std::vector<int> planted = { 12000, 36000 };

    SourceAudio::Ptr original (new SourceAudio());
    original->sampleRate = sampleRate;
    original->name = "clicks";
    original->buffer = makeClickTrack (planted, 48000, sampleRate);

    CaptureIO io;
    const auto encoded = CaptureIO::encodeToBase64 (*original);
    auto restored = io.decodeFromBase64 (encoded, "clicks", sampleRate, sampleRate);

    EXPECT_TRUE (restored != nullptr);
    EXPECT_MSG ((int) restored->transients.size() == 2,
                "restored source carries " + juce::String ((int) restored->transients.size())
                    + " transients, expected 2");
}
