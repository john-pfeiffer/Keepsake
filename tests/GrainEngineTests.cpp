#include "TestHarness.h"
#include "TestUtils.h"
#include "engine/GrainEngine.h"

using namespace keepsake;

KTEST_CASE (window_shapesStartAndEndAtZero)
{
    // Any grain window that does not reach zero at both ends is a click generator.
    for (float morph = 0.0f; morph <= 1.0f; morph += 0.1f)
    {
        EXPECT_NEAR (GrainWindow::shape (0.0f, morph), 0.0, 1.0e-5);
        EXPECT_NEAR (GrainWindow::shape (1.0f, morph), 0.0, 1.0e-5);
    }
}

KTEST_CASE (window_shapesStayWithinUnitRange)
{
    for (float morph = 0.0f; morph <= 1.0f; morph += 0.1f)
    {
        for (int i = 0; i <= 200; ++i)
        {
            const auto v = GrainWindow::shape ((float) i / 200.0f, morph);
            EXPECT_TRUE (v >= 0.0f && v <= 1.0001f);
        }
    }
}

KTEST_CASE (window_morphIsContinuous)
{
    // No sudden jump as the knob crosses the Hann/Tukey and Tukey/Expodec joins.
    for (float t = 0.05f; t < 1.0f; t += 0.05f)
    {
        float previous = GrainWindow::shape (t, 0.0f);

        for (float morph = 0.01f; morph <= 1.0f; morph += 0.01f)
        {
            const auto v = GrainWindow::shape (t, morph);
            EXPECT_MSG (std::abs (v - previous) < 0.1f,
                        "window jumped at t=" + juce::String (t)
                            + " morph=" + juce::String (morph));
            previous = v;
        }
    }
}

KTEST_CASE (hermite_passesThroughItsKnownPoints)
{
    // frac 0 must return x0 exactly, frac 1 must return x1 exactly.
    EXPECT_NEAR (hermite (0.0f, 0.25f, 0.75f, 1.0f, 0.0f), 0.25, 1.0e-6);
    EXPECT_NEAR (hermite (0.0f, 0.25f, 0.75f, 1.0f, 1.0f), 0.75, 1.0e-6);
}

KTEST_CASE (hermite_isExactOnALinearRamp)
{
    EXPECT_NEAR (hermite (0.0f, 1.0f, 2.0f, 3.0f, 0.5f), 1.5, 1.0e-5);
}

namespace
{
    GrainEngine::Settings defaultSettings()
    {
        GrainEngine::Settings s;
        s.place = 0.25;
        s.captureLengthMs = 120.0;
        s.grainSizeMs = 60.0;
        s.densityPerSecond = 24.0;
        s.drift = 0.15;
        s.shimmerCents = 0.0;
        s.windowMorph = 0.0;
        s.spread = 0.4;
        s.playbackRatio = 1.0;
        return s;
    }

    juce::AudioBuffer<float> renderGrains (const GrainEngine::Settings& s,
                                           const SourceAudio& source,
                                           int numSamples,
                                           double sampleRate,
                                           int seed = 1234)
    {
        GrainEngine engine;
        engine.prepare (sampleRate);
        engine.setSeed (seed);

        juce::AudioBuffer<float> out (2, numSamples);
        out.clear();

        // Render in irregular chunks: block-size independence is a real requirement,
        // and a single big call would never catch a per-block state bug.
        int pos = 0;
        const int chunks[] = { 64, 128, 33, 512, 7, 256 };
        int chunkIndex = 0;

        while (pos < numSamples)
        {
            const auto n = juce::jmin (chunks[chunkIndex % 6], numSamples - pos);
            engine.process (out, pos, n, &source, s);
            pos += n;
            ++chunkIndex;
        }

        return out;
    }
}

KTEST_CASE (grains_produceAudioAndStayFinite)
{
    auto source = ktest::makeSineSource (220.0, 2.0, 48000.0);
    const auto out = renderGrains (defaultSettings(), *source, 48000, 48000.0);

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_TRUE (out.getMagnitude (0, out.getNumSamples()) > 0.01f);
}

KTEST_CASE (grains_nullSourceIsSilentNotACrash)
{
    GrainEngine engine;
    engine.prepare (48000.0);

    juce::AudioBuffer<float> out (2, 512);
    out.clear();

    engine.process (out, 0, 512, nullptr, defaultSettings());

    EXPECT_NEAR ((double) out.getMagnitude (0, 512), 0.0, 1.0e-9);
}

/** M2 exit test: "no clicks at any param extreme." */
KTEST_CASE (grains_noClicksAtParameterExtremes)
{
    auto source = ktest::makeSineSource (220.0, 2.0, 48000.0);

    struct Case { const char* name; GrainEngine::Settings s; };
    std::vector<Case> cases;

    auto add = [&cases] (const char* name, auto&& mutate)
    {
        auto s = defaultSettings();
        mutate (s);
        cases.push_back ({ name, s });
    };

    add ("min grain size",  [] (auto& s) { s.grainSizeMs = 5.0; });
    add ("max grain size",  [] (auto& s) { s.grainSizeMs = 250.0; });
    add ("min density",     [] (auto& s) { s.densityPerSecond = 2.0; });
    add ("max density",     [] (auto& s) { s.densityPerSecond = 200.0; });
    add ("max drift",       [] (auto& s) { s.drift = 1.0; });
    add ("max shimmer",     [] (auto& s) { s.shimmerCents = 100.0; });
    add ("expodec window",  [] (auto& s) { s.windowMorph = 1.0; });
    add ("tukey window",    [] (auto& s) { s.windowMorph = 0.5; });
    add ("max spread",      [] (auto& s) { s.spread = 1.0; });
    add ("min capture len", [] (auto& s) { s.captureLengthMs = 10.0; });
    add ("max capture len", [] (auto& s) { s.captureLengthMs = 500.0; });
    add ("place at end",    [] (auto& s) { s.place = 1.0; });
    add ("place at start",  [] (auto& s) { s.place = 0.0; });
    add ("high transpose",  [] (auto& s) { s.playbackRatio = 4.0; });
    add ("low transpose",   [] (auto& s) { s.playbackRatio = 0.25; });

    for (const auto& c : cases)
    {
        const auto out = renderGrains (c.s, *source, 48000, 48000.0);

        EXPECT_MSG (ktest::isFinite (out), juce::String (c.name) + ": produced non-finite samples");

        // Silence would sail through the click check, so make each case prove it
        // actually rendered something first.
        EXPECT_MSG (out.getMagnitude (0, out.getNumSamples()) > 0.001f,
                    juce::String (c.name) + ": rendered silence");

        // The source is a 220Hz sine at full scale; its own maximum step at 48k is
        // ~0.03. Anything above 0.25 in one sample is an envelope discontinuity, not
        // programme material.
        const auto worst = ktest::maxDiscontinuity (out);
        EXPECT_MSG (worst < 0.25f,
                    juce::String (c.name) + ": sample step of " + juce::String (worst, 4)
                        + " looks like a click");
    }
}

/** Spec §2.1: Place must be smooth on Cloud - it is the primary Place experience. */
KTEST_CASE (grains_placeSweepIsClickFree)
{
    auto source = ktest::makeSineSource (220.0, 4.0, 48000.0);

    GrainEngine engine;
    engine.prepare (48000.0);
    engine.setSeed (99);

    auto s = defaultSettings();

    juce::AudioBuffer<float> out (2, 48000);
    out.clear();

    // Sweep Place across the whole file over one second, in small blocks.
    constexpr int blockSize = 64;

    for (int pos = 0; pos < out.getNumSamples(); pos += blockSize)
    {
        s.place = (double) pos / (double) out.getNumSamples();
        engine.process (out, pos, juce::jmin (blockSize, out.getNumSamples() - pos), source.get(), s);
    }

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_MSG (out.getMagnitude (0, out.getNumSamples()) > 0.001f, "Place sweep rendered silence");
    EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.25f,
                "Place sweep produced a discontinuity of "
                    + juce::String (ktest::maxDiscontinuity (out), 4));
}

KTEST_CASE (grains_areDeterministicForAGivenSeed)
{
    auto source = ktest::makeSineSource (220.0, 2.0, 48000.0);

    const auto a = renderGrains (defaultSettings(), *source, 24000, 48000.0, 4242);
    const auto b = renderGrains (defaultSettings(), *source, 24000, 48000.0, 4242);

    float worst = 0.0f;

    for (int i = 0; i < a.getNumSamples(); ++i)
        worst = juce::jmax (worst, std::abs (a.getSample (0, i) - b.getSample (0, i)));

    // Bit-exact on purpose: same seed, same settings must give the same samples, or
    // the offline render tests have no stable reference to diff against.
    EXPECT_MSG (! (worst > 0.0f), "same seed produced different audio");
}

KTEST_CASE (grains_differentSeedsDecorrelate)
{
    auto source = ktest::makeSineSource (220.0, 2.0, 48000.0);

    const auto a = renderGrains (defaultSettings(), *source, 24000, 48000.0, 1);
    const auto b = renderGrains (defaultSettings(), *source, 24000, 48000.0, 2);

    float worst = 0.0f;

    for (int i = 0; i < a.getNumSamples(); ++i)
        worst = juce::jmax (worst, std::abs (a.getSample (0, i) - b.getSample (0, i)));

    EXPECT_MSG (worst > 0.0f, "different seeds produced identical audio");
}

KTEST_CASE (grains_neverExceedThePoolSize)
{
    auto source = ktest::makeSineSource (220.0, 2.0, 48000.0);

    GrainEngine engine;
    engine.prepare (48000.0);

    auto s = defaultSettings();
    s.densityPerSecond = 200.0; // maximum
    s.grainSizeMs = 250.0;      // maximum overlap
    s.captureLengthMs = 500.0;

    juce::AudioBuffer<float> out (2, 48000);
    out.clear();

    for (int pos = 0; pos < out.getNumSamples(); pos += 128)
    {
        engine.process (out, pos, 128, source.get(), s);
        EXPECT_TRUE (engine.getActiveGrainCount() <= GrainEngine::kMaxGrains);
    }
}

/** Overlap compensation: level should not swing wildly across the Density range. */
KTEST_CASE (grains_levelIsStableAcrossDensity)
{
    auto source = ktest::makeSineSource (220.0, 2.0, 48000.0);

    auto rmsAt = [&source] (double density)
    {
        auto s = defaultSettings();
        s.densityPerSecond = density;
        s.drift = 0.0;
        s.spread = 0.0;

        const auto out = renderGrains (s, *source, 48000, 48000.0, 7);
        return (double) out.getRMSLevel (0, 4800, 38400); // skip the settling edges
    };

    const auto low = rmsAt (8.0);
    const auto mid = rmsAt (40.0);
    const auto high = rmsAt (200.0);

    EXPECT_TRUE (low > 0.0 && mid > 0.0 && high > 0.0);

    const auto lowToHighDb = 20.0 * std::log10 (high / juce::jmax (1.0e-9, low));

    EXPECT_MSG (std::abs (lowToHighDb) < 12.0,
                "density 8/s to 200/s changed level by "
                    + juce::String (lowToHighDb, 1) + " dB");
}
