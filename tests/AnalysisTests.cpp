#include "TestHarness.h"
#include "TestUtils.h"
#include "engine/Analysis.h"

using namespace keepsake;

namespace
{
    constexpr int kFrame = WavetableSet::kFrameSize;
    constexpr int kLevels = WavetableSet::kNumMipLevels;

    /** Spectrum magnitude (linear) of one 2048 table, bins 0..1024. */
    std::vector<float> tableSpectrum (const float* table)
    {
        std::vector<float> scratch ((size_t) kFrame * 2, 0.0f);
        std::copy (table, table + kFrame, scratch.begin());

        juce::dsp::FFT fft (11);
        fft.performRealOnlyForwardTransform (scratch.data(), true);

        const auto* bins = reinterpret_cast<const std::complex<float>*> (scratch.data());
        std::vector<float> magnitude ((size_t) kFrame / 2 + 1);

        for (size_t k = 0; k < magnitude.size(); ++k)
            magnitude[k] = std::abs (bins[k]);

        return magnitude;
    }
}

/** The normalization probe: JUCE's FFT backends differ per platform, so unit
    round-trip gain is verified, not assumed. Everything downstream (mip levels,
    the alias test) leans on this. */
KTEST_CASE (analysis_fftRoundTripPreservesAmplitude)
{
    std::vector<float> frame ((size_t) kFrame);

    for (int i = 0; i < kFrame; ++i)
        frame[(size_t) i] = std::sin (juce::MathConstants<float>::twoPi * 3.0f
                                      * (float) i / (float) kFrame) * 0.5f;

    std::vector<float> mips ((size_t) kLevels * kFrame);
    analysis::buildMipChain (frame.data(), mips.data());

    // Level 0 keeps everything below Nyquist/1 - for a 3-cycle sine that is the
    // whole signal, so the round trip (plus phase rotation) must preserve the
    // amplitude exactly. Phase is rotated by design, so compare magnitudes.
    const auto original = tableSpectrum (frame.data());
    const auto rebuilt = tableSpectrum (mips.data());

    EXPECT_NEAR ((double) rebuilt[3], (double) original[3], (double) original[3] * 1.0e-3);

    float peak = 0.0f;
    for (int i = 0; i < kFrame; ++i)
        peak = juce::jmax (peak, std::abs (mips[(size_t) i]));

    EXPECT_NEAR ((double) peak, 0.5, 0.005);
}

KTEST_CASE (analysis_mipLevelsContainNoEnergyAboveTheirLimit)
{
    // A dense frame: every 10th harmonic up to near-Nyquist.
    std::vector<float> frame ((size_t) kFrame, 0.0f);

    for (int h = 1; h <= 1000; h += 10)
        for (int i = 0; i < kFrame; ++i)
            frame[(size_t) i] += std::sin (juce::MathConstants<float>::twoPi * (float) h
                                           * (float) i / (float) kFrame) / (float) h;

    std::vector<float> mips ((size_t) kLevels * kFrame);
    analysis::buildMipChain (frame.data(), mips.data());

    for (int level = 0; level < kLevels; ++level)
    {
        const auto spectrum = tableSpectrum (mips.data() + (size_t) level * kFrame);
        const auto keep = (kFrame / 2) >> level;

        float above = 0.0f, below = 0.0f;

        for (int k = 1; k < (int) spectrum.size(); ++k)
            (k <= keep ? below : above) = juce::jmax (k <= keep ? below : above, spectrum[(size_t) k]);

        // Spec's own suggested unit test: band-limited means *zero* energy above
        // the limit, to numerical precision.
        EXPECT_MSG (above < below * 1.0e-5f + 1.0e-6f,
                    "level " + juce::String (level) + " leaks above bin "
                        + juce::String (keep) + " (above=" + juce::String (above, 8) + ")");
    }

    // DC must be gone from every level (offset steps when frames are scanned).
    for (int level = 0; level < kLevels; ++level)
    {
        const auto spectrum = tableSpectrum (mips.data() + (size_t) level * kFrame);
        EXPECT_MSG (spectrum[0] < 1.0e-3f, "level " + juce::String (level) + " has DC");
    }
}

KTEST_CASE (analysis_sineSourceYieldsSingleCycleFrames)
{
    constexpr double sr = 48000.0, f0 = 220.0;

    auto source = ktest::makeSineSource (f0, 1.0, sr);
    const auto window = CaptureWindow::resolve (*source, 0.25, 200.0, sr);

    auto set = analysis::buildWavetableSet (*source, window, f0, 8);
    EXPECT_TRUE (set != nullptr);

    if (set == nullptr)
        return;

    EXPECT_TRUE (set->numFrames == 8);

    // Each level-0 frame of a pure sine source must be one cycle of a sine:
    // all energy in bin 1, nothing anywhere else.
    for (int frame = 0; frame < set->numFrames; ++frame)
    {
        const auto spectrum = tableSpectrum (set->getTable (frame, 0));

        float other = 0.0f;
        for (int k = 0; k < (int) spectrum.size(); ++k)
            if (k != 1)
                other = juce::jmax (other, spectrum[(size_t) k]);

        EXPECT_MSG (spectrum[1] > 100.0f * other,
                    "frame " + juce::String (frame) + " is not a clean single cycle");
    }

    // Phase normalization: every frame's fundamental is aligned, so frames of the
    // same steady sine are near-identical sample by sample.
    float worst = 0.0f;
    for (int i = 0; i < kFrame; ++i)
        worst = juce::jmax (worst, std::abs (set->getTable (0, 0)[i] - set->getTable (7, 0)[i]));

    EXPECT_MSG (worst < 0.01f, "frames of a steady sine differ by " + juce::String (worst, 6));
}

KTEST_CASE (analysis_shortWindowDuplicatesFrames)
{
    constexpr double sr = 48000.0, f0 = 110.0; // P ~ 436 samples

    auto source = ktest::makeSineSource (f0, 1.0, sr);

    // 20ms window = 960 samples = barely 2 cycles; ask for 16 frames.
    const auto window = CaptureWindow::resolve (*source, 0.5, 20.0, sr);
    auto set = analysis::buildWavetableSet (*source, window, f0, 16);

    EXPECT_TRUE (set != nullptr && set->numFrames == 16);

    if (set == nullptr)
        return;

    // The duplicated tail slots must be exact copies of the last distinct frame.
    float diff = 0.0f;
    for (int i = 0; i < kFrame; ++i)
        diff = juce::jmax (diff, std::abs (set->getTable (15, 0)[i] - set->getTable (14, 0)[i]));

    EXPECT_MSG (! (diff > 0.0f), "duplicated frames are not identical copies");
}

KTEST_CASE (analysis_subPeriodWindowIsDefinedAndFinite)
{
    constexpr double sr = 48000.0;

    // Root 36 (C1, ~65Hz) -> P ~ 734 samples; a 10ms window is only 480. The
    // whole window becomes the single cycle - defined behaviour, not a crash.
    auto source = ktest::makeSineSource (65.4, 1.0, sr);
    const auto window = CaptureWindow::resolve (*source, 0.3, 10.0, sr);

    EXPECT_TRUE (window.numSamples < 734);

    auto set = analysis::buildWavetableSet (*source, window,
                                            analysis::rootFrequencyHz (36.0, 0.0), 8);
    EXPECT_TRUE (set != nullptr);

    if (set == nullptr)
        return;

    for (int i = 0; i < kFrame; ++i)
        EXPECT_TRUE (std::isfinite (set->getTable (0, 0)[i]));
}

KTEST_CASE (analysis_drumSourceWithArbitraryRootProducesLoopableFrames)
{
    constexpr double sr = 48000.0;

    auto source = ktest::makeDrumSource (0.5, sr);
    const auto window = CaptureWindow::resolve (*source, 0.1, 120.0, sr);

    auto set = analysis::buildWavetableSet (*source, window,
                                            analysis::rootFrequencyHz (60.0, 0.0), 8);
    EXPECT_TRUE (set != nullptr);

    if (set == nullptr)
        return;

    for (int frame = 0; frame < set->numFrames; ++frame)
    {
        const auto* t = set->getTable (frame, 0);

        for (int i = 0; i < kFrame; ++i)
            EXPECT_TRUE (std::isfinite (t[i]));

        // Loopable: the wrap step must be comparable to an ordinary in-frame step,
        // not a cliff (the wrap crossfade's job on aperiodic material).
        float maxStep = 0.0f;
        for (int i = 1; i < kFrame; ++i)
            maxStep = juce::jmax (maxStep, std::abs (t[i] - t[i - 1]));

        const auto wrapStep = std::abs (t[0] - t[kFrame - 1]);
        EXPECT_MSG (wrapStep <= maxStep * 2.0f + 1.0e-6f,
                    "frame " + juce::String (frame) + " wrap step " + juce::String (wrapStep, 6)
                        + " vs max in-frame step " + juce::String (maxStep, 6));
    }
}

KTEST_CASE (analysis_isDeterministic)
{
    constexpr double sr = 48000.0;

    auto source = ktest::makeDrumSource (0.5, sr);
    const auto window = CaptureWindow::resolve (*source, 0.2, 100.0, sr);
    const auto f0 = analysis::rootFrequencyHz (60.0, 0.0);

    auto a = analysis::buildWavetableSet (*source, window, f0, 8);
    auto b = analysis::buildWavetableSet (*source, window, f0, 8);

    EXPECT_TRUE (a != nullptr && b != nullptr);

    if (a == nullptr || b == nullptr)
        return;

    EXPECT_TRUE (a->data.size() == b->data.size());
    EXPECT_MSG (a->data == b->data, "same inputs produced different wavetable data");
}

KTEST_CASE (analysis_rootFrequencyMatchesA440)
{
    EXPECT_NEAR (analysis::noteFrequencyHz (69.0), 440.0, 1.0e-9);
    EXPECT_NEAR (analysis::noteFrequencyHz (57.0), 220.0, 1.0e-9);
    EXPECT_NEAR (analysis::rootFrequencyHz (69.0, 100.0), analysis::noteFrequencyHz (70.0), 1.0e-9);
}
