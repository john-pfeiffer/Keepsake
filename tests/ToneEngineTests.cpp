#include "TestHarness.h"
#include "TestUtils.h"
#include "engine/Analysis.h"
#include "engine/ToneEngine.h"

using namespace keepsake;

namespace
{
    constexpr double kSr = 48000.0;

    WavetableSet::Ptr makeSetFrom (SourceAudio& source, double place, double lengthMs,
                                   double rootMidi, int numFrames)
    {
        const auto window = CaptureWindow::resolve (source, place, lengthMs, kSr);
        return analysis::buildWavetableSet (source, window,
                                            analysis::rootFrequencyHz (rootMidi, 0.0),
                                            numFrames);
    }

    /** Renders the oscillator standalone in irregular chunks (block-size bugs
        never show up with one big call). */
    juce::AudioBuffer<float> renderTone (ToneEngine& tone, const WavetableSet* set,
                                         double frequency, double framePos, int numSamples)
    {
        juce::AudioBuffer<float> out (1, numSamples);
        const int chunks[] = { 64, 128, 33, 512, 7, 256 };
        int pos = 0, c = 0;

        while (pos < numSamples)
        {
            const auto n = juce::jmin (chunks[c++ % 6], numSamples - pos);
            tone.process (out.getWritePointer (0, pos), n, set, frequency, framePos);
            pos += n;
        }

        return out;
    }
}

KTEST_CASE (tone_nullSetIsSilent)
{
    ToneEngine tone;
    tone.prepare (kSr);
    tone.noteOn (nullptr);

    const auto out = renderTone (tone, nullptr, 220.0, 0.0, 4096);
    EXPECT_NEAR ((double) out.getMagnitude (0, 4096), 0.0, 1.0e-12);
}

/** M3 exit test, tonal half: alias-free chromatic playback from a harmonic
    ("vocal-like") source with Root set correctly. Octaves plus mip-boundary
    neighbours per CI run; the boundary notes only stay clean because level
    interpolation is continuous. */
KTEST_CASE (tone_aliasFreeAcrossKeyboard_harmonicSource)
{
    auto source = ktest::makeHarmonicSource (analysis::noteFrequencyHz (48), 1.0, kSr);
    auto set = makeSetFrom (*source, 0.25, 150.0, 48.0, 8);
    EXPECT_TRUE (set != nullptr);

    if (set == nullptr)
        return;

    // C1..C7 octaves (C3=60 naming) plus notes bracketing a mip boundary (~750Hz
    // at 48k is inc=32 -> level 5; FS5 = note 90 sits right there).
    for (int note : { 36, 48, 60, 72, 84, 96, 108, 89, 90, 91 })
    {
        ToneEngine tone;
        tone.prepare (kSr);
        tone.noteOn (set.get());

        const auto f = analysis::noteFrequencyHz (note);
        const auto out = renderTone (tone, set.get(), f, 0.0, 48000 + 32768);

        EXPECT_MSG (ktest::isFinite (out), "note " + juce::String (note) + " non-finite");
        EXPECT_MSG (out.getMagnitude (0, out.getNumSamples()) > 0.001f,
                    "note " + juce::String (note) + " rendered silence");

        const auto spectrum = ktest::powerSpectrumDb (out, 48000);
        const auto spurs = ktest::worstOffGridDb (spectrum, f, kSr);

        EXPECT_MSG (spurs < -50.0f,
                    "note " + juce::String (note) + ": worst off-grid spur "
                        + juce::String (spurs, 1) + " dB (limit -50)");
    }
}

/** M3 exit test, drum half: an aperiodic source with an arbitrary Root still
    plays back exactly periodic - so the same harmonic-grid assertion holds. */
KTEST_CASE (tone_aliasFreeAcrossKeyboard_drumSource)
{
    auto source = ktest::makeDrumSource (0.5, kSr);
    auto set = makeSetFrom (*source, 0.1, 120.0, 60.0, 8); // Root is arbitrary here
    EXPECT_TRUE (set != nullptr);

    if (set == nullptr)
        return;

    for (int note : { 36, 60, 84, 108 })
    {
        ToneEngine tone;
        tone.prepare (kSr);
        tone.noteOn (set.get());

        const auto f = analysis::noteFrequencyHz (note);
        const auto out = renderTone (tone, set.get(), f, 0.3, 48000 + 32768);

        EXPECT_MSG (ktest::isFinite (out), "note " + juce::String (note) + " non-finite");

        const auto spectrum = ktest::powerSpectrumDb (out, 48000);
        const auto spurs = ktest::worstOffGridDb (spectrum, f, kSr);

        EXPECT_MSG (spurs < -50.0f,
                    "drum note " + juce::String (note) + ": worst off-grid spur "
                        + juce::String (spurs, 1) + " dB (limit -50)");
    }
}

/** Negative control: force the full-bandwidth table at C7 and assert the
    detector DOES light up. Without this, a broken analyzer passes everything
    forever - same discipline as the M2 mutation checks, but permanent. */
KTEST_CASE (tone_aliasDetectorNegativeControl)
{
    auto source = ktest::makeHarmonicSource (analysis::noteFrequencyHz (48), 1.0, kSr);
    auto set = makeSetFrom (*source, 0.25, 150.0, 48.0, 8);
    EXPECT_TRUE (set != nullptr);

    if (set == nullptr)
        return;

    ToneEngine tone;
    tone.prepare (kSr);
    tone.noteOn (set.get());
    tone.forcedMipLevel = 0; // deliberately alias: 1024 harmonics at C7

    const auto f = analysis::noteFrequencyHz (108);
    const auto out = renderTone (tone, set.get(), f, 0.0, 48000 + 32768);

    const auto spectrum = ktest::powerSpectrumDb (out, 48000);
    const auto spurs = ktest::worstOffGridDb (spectrum, f, kSr);

    EXPECT_MSG (spurs > -40.0f,
                "negative control failed: forced aliasing measured only "
                    + juce::String (spurs, 1) + " dB - the detector is broken");
}

/* The click tests below use 6-harmonic material on purpose: the click threshold
   (0.25) must sit far above the material's own legitimate slew (~0.07/sample at
   6 harmonics) and far below a hard swap step (~2x peak). Thirty-harmonic
   material slews at ~0.35/sample and would trip the detector with no bug. */
KTEST_CASE (tone_setSwapCrossfadesWithoutClicks)
{
    // Two sets of genuinely DIFFERENT material. Sets sliced from the same steady
    // source are near-identical after phase normalization, and a swap between
    // near-identical tables steps by ~nothing - a test like that cannot tell a
    // crossfade from a hard swap (found by mutation check).
    auto harmonic = ktest::makeHarmonicSource (analysis::noteFrequencyHz (48), 1.0, kSr, 6);
    auto drum = ktest::makeDrumSource (0.5, kSr);

    auto setA = makeSetFrom (*harmonic, 0.1, 150.0, 48.0, 8);
    auto setB = makeSetFrom (*drum, 0.1, 120.0, 48.0, 8);
    EXPECT_TRUE (setA != nullptr && setB != nullptr);

    if (setA == nullptr || setB == nullptr)
        return;

    ToneEngine tone;
    tone.prepare (kSr);
    tone.noteOn (setA.get());

    juce::AudioBuffer<float> out (1, 48000);

    // Alternate repeatedly at a period-unfriendly interval so a hard swap cannot
    // hide at a lucky zero crossing.
    int pos = 0;

    while (pos < out.getNumSamples())
    {
        const auto n = juce::jmin (256, out.getNumSamples() - pos);
        const auto* latest = ((pos / 3001) % 2 == 0) ? setA.get() : setB.get();

        // M4's Focus coupling detunes Tone by up to 7 cents; the swap fade's
        // correctness argument (shared phase, fundamental-aligned sets) only
        // involves the phase accumulator, not the frequency feeding it - this
        // detuned frequency proves that claim stays true.
        tone.process (out.getWritePointer (0, pos), n, latest,
                      220.0 * std::pow (2.0, 7.0 / 1200.0), 0.5);
        pos += n;
    }

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.25f,
                "set swap clicked: step " + juce::String (ktest::maxDiscontinuity (out), 4));
}

KTEST_CASE (tone_framesCountChangeIsJustAnotherSwap)
{
    // NOTE: normalized Frame position maps to the same source moment whatever N
    // is, so an 8->16 swap at equal framePos is near-identical content BY DESIGN
    // and this test cannot distinguish crossfade from hard swap on its own. The
    // swap machinery is a single code path, mutation-proven by the test above;
    // this test asserts the frames-count path adds no special case, no click and
    // no crash.
    auto source = ktest::makeDrumSource (0.5, kSr);

    auto set8 = makeSetFrom (*source, 0.1, 120.0, 48.0, 8);
    auto set16 = makeSetFrom (*source, 0.1, 120.0, 48.0, 16);
    EXPECT_TRUE (set8 != nullptr && set16 != nullptr);

    if (set8 == nullptr || set16 == nullptr)
        return;

    ToneEngine tone;
    tone.prepare (kSr);
    tone.noteOn (set8.get());

    juce::AudioBuffer<float> out (1, 48000);
    int pos = 0;

    while (pos < out.getNumSamples())
    {
        const auto n = juce::jmin (256, out.getNumSamples() - pos);
        const auto* latest = ((pos / 3001) % 2 == 0)
                               ? (const WavetableSet*) set8.get()
                               : (const WavetableSet*) set16.get();
        tone.process (out.getWritePointer (0, pos), n, latest, 220.0, 0.8);
        pos += n;
    }

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.25f,
                "frames-count swap clicked: step "
                    + juce::String (ktest::maxDiscontinuity (out), 4));
}

KTEST_CASE (tone_firstSetArrivingMidNoteFadesInFromSilence)
{
    auto source = ktest::makeHarmonicSource (analysis::noteFrequencyHz (48), 1.0, kSr, 6);
    auto set = makeSetFrom (*source, 0.25, 150.0, 48.0, 8);
    EXPECT_TRUE (set != nullptr);

    if (set == nullptr)
        return;

    ToneEngine tone;
    tone.prepare (kSr);
    tone.noteOn (nullptr); // note started before any extraction completed

    juce::AudioBuffer<float> out (1, 24000);
    int pos = 0;

    while (pos < out.getNumSamples())
    {
        const auto n = juce::jmin (256, out.getNumSamples() - pos);
        const auto* latest = pos < 8000 ? nullptr : set.get();
        tone.process (out.getWritePointer (0, pos), n, latest, 220.0, 0.0);
        pos += n;
    }

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_NEAR ((double) out.getMagnitude (0, 0, 8000), 0.0, 1.0e-12); // silent before
    EXPECT_TRUE (out.getMagnitude (0, 12000, 12000) > 0.01f);            // audible after
    EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.25f,
                "fade-in from silence clicked: step "
                    + juce::String (ktest::maxDiscontinuity (out), 4));
}

KTEST_CASE (tone_frameSweepIsClickFree)
{
    auto source = ktest::makeHarmonicSource (analysis::noteFrequencyHz (48), 1.0, kSr, 6);
    auto set = makeSetFrom (*source, 0.25, 300.0, 48.0, 16);
    EXPECT_TRUE (set != nullptr);

    if (set == nullptr)
        return;

    ToneEngine tone;
    tone.prepare (kSr);
    tone.noteOn (set.get());

    juce::AudioBuffer<float> out (1, 48000);
    int pos = 0;

    while (pos < out.getNumSamples())
    {
        const auto n = juce::jmin (64, out.getNumSamples() - pos);
        const auto framePos = (double) pos / (double) out.getNumSamples();
        tone.process (out.getWritePointer (0, pos), n, set.get(), 220.0, framePos);
        pos += n;
    }

    EXPECT_TRUE (ktest::isFinite (out));
    EXPECT_TRUE (out.getMagnitude (0, out.getNumSamples()) > 0.001f);
    EXPECT_MSG (ktest::maxDiscontinuity (out) < 0.25f,
                "frame sweep clicked: step " + juce::String (ktest::maxDiscontinuity (out), 4));
}

KTEST_CASE (tone_renderIsIndependentOfBlockSize)
{
    auto source = ktest::makeHarmonicSource (analysis::noteFrequencyHz (48), 1.0, kSr);
    auto set = makeSetFrom (*source, 0.25, 150.0, 48.0, 8);
    EXPECT_TRUE (set != nullptr);

    if (set == nullptr)
        return;

    auto render = [&set] (int blockSize)
    {
        ToneEngine tone;
        tone.prepare (kSr);
        tone.noteOn (set.get());

        juce::AudioBuffer<float> out (1, 12288);
        int pos = 0;

        while (pos < out.getNumSamples())
        {
            const auto n = juce::jmin (blockSize, out.getNumSamples() - pos);
            // Parameters held static on purpose: the smoothing idiom (target per
            // block, smooth per sample) is only block-size-invariant for static
            // targets. Do not "fix" a tiny diff here by weakening the tolerance -
            // moving targets belong in the click tests, not this one.
            tone.process (out.getWritePointer (0, pos), n, set.get(), 220.0, 0.5);
            pos += n;
        }

        return out;
    };

    const auto a = render (64);
    const auto b = render (1024);

    EXPECT_TRUE (a.getMagnitude (0, a.getNumSamples()) > 0.001f);

    float worst = 0.0f;
    for (int i = 0; i < a.getNumSamples(); ++i)
        worst = juce::jmax (worst, std::abs (a.getSample (0, i) - b.getSample (0, i)));

    EXPECT_MSG (! (worst > 0.0f), "block size changed the tone render by " + juce::String (worst, 8));
}
