#pragma once

#include <juce_dsp/juce_dsp.h>

namespace keepsake
{
    /**
        The master FX chain (spec §2.6): Warmth (soft saturation) -> Chorus
        (one Amount macro) -> Air (reverb, Size + Mix). Deliberately small.

        Air is a SEND-RETURN, not an insert: the reverb input is
        (main bus x Mix) + the per-voice wet-send bus, and the reverb's wet
        output is ADDED to the main bus. Two reasons:

        1. The Reverb Mix mod destination has been, since M4, a per-voice
           wet-send level (ModSnapshot::reverbSend) - the binding contract was
           that M5's FX chain implements a dry bus plus a per-voice-scaled wet
           bus. One shared reverb fed by a send bus is that contract; an insert
           reverb cannot express "this voice is wetter than that one".
        2. The dry signal passes through untouched at any Mix, so the Air knob
           adds space without ducking the instrument - consistent with the M4
           corridor rule (texture may change, level may not).

        Bypass discipline: each effect is ACTIVATED BY ITS PARAMETERS, never by
        signal content. Signal-triggered activation would start an effect's
        internal smoothing ramps at a host-block-dependent position and break
        the bit-identical-across-block-sizes guarantee; parameter-driven
        activation is invariant whenever parameters are static, which is
        exactly the case the determinism tests pin down. With everything at
        default (Warmth 0, Chorus 0, Air Mix 0, no ReverbMix routing) the
        chain is EXACTLY bypassed - the buffer is untouched, preserving every
        pre-M5 bit-identity regression.

        Two subtleties came out of reading the JUCE sources, do not undo them:

        - juce::Reverb's gains are SmoothedValues, and construction leaves them
          at the demo defaults (dry 0.4!). prepare() therefore applies the
          wet-only parameters FIRST and calls setSampleRate() second - its
          reset snaps current = target. Otherwise the first engaged block
          would leak a 10ms dry-gain ramp into the output, anchored at a
          block-quantized position: an intermittent block-size-dependence bug.
        - Deactivation never hard-cuts: the chorus cools down until its ~50ms
          internal mix smoothing has settled, and the reverb keeps running
          until its own output has actually decayed to silence (a fixed
          wall-clock tail would truncate large room sizes), then both reset()
          so no stale delay-line/comb content replays on re-entry.
    */
    class FxChain
    {
    public:
        /** Sized to at least kMaxBlockSamples regardless of the host's
            declared maximum - the same distrust the voice scratch shows. */
        static constexpr int kMaxBlockSamples = 8192;

        void prepare (double sampleRate, int maxBlockSize, int numChannels,
                      float warmth01, float airSize01);

        /** All amounts are 0..1. reverbRouted = a mod slot routes to Reverb
            Mix (keeps the reverb running for per-voice sends even at Mix 0).
            Audio thread; slices internally if a host exceeds the prepared
            maximum block size. */
        void process (juce::AudioBuffer<float>& buffer,
                      const juce::AudioBuffer<float>& wetSend,
                      float warmth01, float chorus01,
                      float airSize01, float airMix01, bool reverbRouted);

    private:
        void processSlice (juce::AudioBuffer<float>& buffer, int start, int numSamples,
                           const juce::AudioBuffer<float>& wetSend,
                           float warmth01, float chorus01,
                           float airSize01, float airMix01, bool reverbRouted);

        juce::LinearSmoothedValue<float> warmth; // amount, smoothed against zipper

        juce::dsp::Chorus<float> chorus;
        juce::LinearSmoothedValue<float> chorusMakeup; // undoes the linear-mix level dip
        int chorusCooldown = 0; // samples of post-zero settling before hard bypass
        bool chorusRunning = false;

        juce::Reverb reverb;
        juce::AudioBuffer<float> reverbScratch; // the send bus fed to the reverb
        bool reverbRunning = false;
        int reverbSilentStreak = 0; // consecutive near-silent output samples
        float lastRoomSize = -1.0f;

        double rate = 48000.0;
        int preparedBlockSize = kMaxBlockSamples;
        int channels = 2;
    };
}
