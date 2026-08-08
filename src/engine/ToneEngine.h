#pragma once

#include "CaptureBuffer.h"

namespace keepsake
{
    /**
        Per-voice wavetable oscillator over a WavetableSet (spec §2.3).

        - Continuous mip-level interpolation: level Lf = max(0, log2(inc)) where
          inc is the phase increment in table samples; blends floor/ceil levels so
          nothing steps when the pitch wheel (or, from M4, pitch modulation)
          crosses an octave boundary mid-note.
        - Linear interpolation between adjacent frames (16 frames max - snapping
          is audible, and Frame becomes a mod target in M4), Hermite within a
          frame.
        - Set handover: when the store publishes a new set, a running voice
          crossfades old->new over ~30ms with a SHARED phase accumulator. The fade
          is linear, not equal-power: extraction phase-normalizes every frame's
          fundamental (Analysis bin-1 rotation), so old and new are correlated and
          equal-power would bump +3dB at the midpoint. A set arriving mid-fade is
          parked as pending (latest wins) and starts after the current fade ends.
        - Note-start captures the current set cold - no fade at onset, so the swap
          machinery never interacts with the amp attack.

        Real-time contract: process() performs no allocation, no locking. Raw set
        pointers held here are kept alive by WavetableStore's deferred release plus
        its pinned-pointer garbage-collection guard; getPinnedSets() is how the
        message thread learns what this voice is holding.
    */
    class ToneEngine
    {
    public:
        static constexpr double kSwapFadeSeconds = 0.030;
        static constexpr double kFrameSmoothingSeconds = 0.015;

        void prepare (double sampleRate);

        /** Note start: capture the set cold, reset phase (determinism), no fade. */
        void noteOn (const WavetableSet* set) noexcept;

        /** Adds nothing; writes mono into `out` (replacing). Audio thread only.
            @param latest     the store's current set (may be nullptr)
            @param frequency  the played frequency in Hz (note + wheel)
            @param framePos   0..1 scan position target (smoothed internally) */
        void process (float* out, int numSamples,
                      const WavetableSet* latest,
                      double frequency,
                      double framePos) noexcept;

        /** Message thread. Appends every set pointer this voice still holds -
            feed the result to WavetableStore::collectGarbage. */
        void getPinnedSets (std::vector<const WavetableSet*>& out) const;

        /** Test seam: force a single mip level (-1 = normal behaviour). Lets the
            alias test's negative control prove the detector actually detects. */
        int forcedMipLevel = -1;

    private:
        float readSample (const WavetableSet& set, double tablePhase,
                          double levelF, double framePosF) const noexcept;

        // Written on the audio thread (release), read by the message thread
        // (acquire) for GC pinning. Plain pointers are safe under the store's
        // deferred-release + pin contract.
        std::atomic<const WavetableSet*> currentSet { nullptr };
        std::atomic<const WavetableSet*> fadingSet { nullptr };  // the OLD set
        std::atomic<const WavetableSet*> pendingSet { nullptr };

        double sampleRate = 44100.0;
        double phase = 0.0; // in table samples, 0..kFrameSize
        double fadeRemaining = 0.0;
        double fadeLength = 1.0;
        bool fadingFromSilence = false;

        juce::LinearSmoothedValue<float> framePosSmoothed;
    };
}
