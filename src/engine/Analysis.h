#pragma once

#include "CaptureBuffer.h"
#include <juce_dsp/juce_dsp.h>

namespace keepsake::analysis
{
    /**
        Wavetable extraction and mipmap construction (spec §2.3).

        Everything here is pure and synchronous: same inputs give bit-identical
        outputs on the same machine, and nothing touches the audio thread. The
        processor's background worker calls buildWavetableSet(); tests call it (or
        the smaller pieces) directly, which is what keeps the block-size-independence
        renders deterministic - the async path never runs in the test harness.

        Note on cross-platform expectations: juce::dsp::FFT dispatches to different
        backends per platform (vDSP, IPP, fallback), so extraction output is NOT
        bit-identical across platforms. Do not write cross-platform golden-file
        tests against it; same-process determinism is the guarantee.
    */

    /** The one formula for Root -> Hz. Voice::computePlaybackRatio and extraction
        must agree on this or pitch and table drift apart. */
    double rootFrequencyHz (double rootNoteMidi, double rootCents) noexcept;

    /** Frequency of a played MIDI note (A440), used by the Tone oscillator. */
    double noteFrequencyHz (double midiNote) noexcept;

    /**
        Phase of the fundamental (frequency f0) around `centre`, measured by a
        Hann-windowed single-bin DFT over roughly +-1.5 periods.

        Returns true and writes the phase (radians, cos convention: the local
        fundamental ~ A*cos(w0*(t-centre) + phase)) when the fundamental has
        meaningful energy; returns false for silence, in which case callers slice
        at the anchor verbatim.

        This replaces naive zero-crossing search: on drums or noisy material raw
        crossings are arbitrary, but the projection onto the fundamental is always
        well-defined, deterministic, and needs no filter settling or group-delay
        bookkeeping.
    */
    bool fundamentalPhaseAt (const float* mono, int numSamples,
                             double centre, double f0, double sampleRate,
                             double& phaseOut) noexcept;

    /**
        Extracts one cycle of `periodSamples` starting at fractional position
        `start`, resampled to WavetableSet::kFrameSize samples via Hermite reads,
        with a short raised-cosine wrap crossfade so aperiodic material loops
        without a step. Reads are edge-clamped.
    */
    void extractResampledCycle (const float* mono, int numSamples,
                                double start, double periodSamples,
                                float* outFrame);

    /**
        Builds all mip levels for one frame.

        Forward FFT once; zero DC and Nyquist; rotate the spectrum so bin 1 has
        zero phase (exact fractional-sample circular rotation - this is what keeps
        every frame's fundamental aligned, so frame morphing and set-swap
        crossfades reinforce instead of cancelling); then per level L zero all bins
        above (kFrameSize/2) >> L and inverse-transform.

        `dest` receives kNumMipLevels contiguous frames of kFrameSize floats.
    */
    void buildMipChain (const float* frame, float* dest);

    /**
        The full pipeline: mono-sums the capture window (plus a guard for cycles
        near the window end), slices N phase-aligned cycles evenly across it,
        resamples, mip-builds, and returns an immutable set.

        Defined edge behaviour (both reachable from the UI):
        - window shorter than N distinct cycles: extract what fits, duplicate the
          last frame to fill N slots.
        - window shorter than ONE period (10ms window + low Root): the whole window
          becomes the single cycle, resampled to kFrameSize. Pitch then reads as
          the window rate rather than Root - defined and click-free.

        Returns nullptr only for an empty window.
    */
    WavetableSet::Ptr buildWavetableSet (const SourceAudio& source,
                                         const CaptureWindow& window,
                                         double f0,
                                         int numFrames);
}
