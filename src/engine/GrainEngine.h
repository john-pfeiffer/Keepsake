#pragma once

#include "CaptureBuffer.h"
#include <juce_dsp/juce_dsp.h>

namespace keepsake
{
    /**
        Grain window shapes, morphed by a single knob (spec §2.2).

        0.0 -> Hann, 0.5 -> Tukey (plateau), 1.0 -> Expodec (sharp attack, long decay).
        Values in between crossfade the adjacent pair, so the knob is continuous.
    */
    struct GrainWindow
    {
        static float hann (float t) noexcept;
        static float tukey (float t, float plateau = 0.5f) noexcept;
        static float expodec (float t) noexcept;

        /** @param t      0..1 through the grain
            @param morph  0..1 Hann -> Tukey -> Expodec */
        static float shape (float t, float morph) noexcept;
    };

    /** 4-point, 3rd-order Hermite interpolation (spec §2.2). */
    float hermite (float xm1, float x0, float x1, float x2, float frac) noexcept;

    /**
        Per-voice granular scheduler reading directly from the source buffer.

        Spec §2.1: Cloud treats the capture window as a *moving read region* over the
        source, so Place is smooth and free - grains simply spawn at the new position.
        There is no per-Place re-capture, which is what makes Place automatable in real
        time on this engine.

        Real-time contract: the grain pool is pre-allocated in prepare(); process()
        performs no allocation, no locking and no logging.
    */
    class GrainEngine
    {
    public:
        static constexpr int kMaxGrains = 32; // spec §2.2

        struct Settings
        {
            double place = 0.0;          // 0..1 through the source file
            double captureLengthMs = 120.0;
            double grainSizeMs = 60.0;
            double densityPerSecond = 24.0;
            double drift = 0.15;         // 0..1
            double shimmerCents = 0.0;   // 0..100
            double windowMorph = 0.0;    // 0..1
            double spread = 0.4;         // 0..1
            double playbackRatio = 1.0;  // MIDI note vs Root

            // Tempo sync: when > 0, grains spawn at EXACTLY this interval
            // (no jitter - the grid is the point), anchored to note-on via
            // reset(). 0 = free-running from densityPerSecond.
            double syncIntervalSamples = 0.0;
        };

        void prepare (double sampleRate);
        void reset() noexcept;

        /** Seeds the per-voice RNG. Equal seeds give bit-identical renders. */
        void setSeed (int seed) noexcept { rng.setSeed (seed); }

        /** Adds this voice's grains into the (stereo) block. Audio thread only. */
        void process (juce::AudioBuffer<float>& output,
                      int startSample,
                      int numSamples,
                      const SourceAudio* source,
                      const Settings& settings) noexcept;

        int getActiveGrainCount() const noexcept;

    private:
        struct Grain
        {
            bool active = false;
            double readPos = 0.0;   // absolute position in source samples
            double rate = 1.0;
            double age = 0.0;       // samples elapsed
            double length = 1.0;    // samples
            float gainL = 0.707f;
            float gainR = 0.707f;
            float windowMorph = 0.0f;
        };

        void spawnGrain (const SourceAudio& source, const Settings& s, int windowStart, int windowLength) noexcept;
        float readSource (const SourceAudio& source, int channel, double position) const noexcept;

        std::array<Grain, kMaxGrains> grains {};
        double currentSampleRate = 44100.0;
        double samplesUntilNextGrain = 0.0;
        juce::Random rng { 0x5EED };
        int nextGrainSlot = 0;
        float normaliseGain = 1.0f;
        bool normaliseInitialised = false;
    };
}
