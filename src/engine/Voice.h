#pragma once

#include "GrainEngine.h"
#include "LFO.h"
#include "ModMatrix.h"
#include <juce_dsp/juce_dsp.h>
#include "ToneEngine.h"
#include "../Parameters.h"

namespace keepsake
{
    /** Trivial sound: Keepsake responds on all notes and channels. */
    struct KeepsakeSound : public juce::SynthesiserSound
    {
        bool appliesToNote (int) override    { return true; }
        bool appliesToChannel (int) override { return true; }
    };

    /**
        One polyphonic voice.

        Owns one Cloud (granular) unit, one Tone (wavetable) unit and the amp
        envelope. Cloud renders into the stereo voice buffer, Tone into a mono
        scratch; the two are blended by the Focus parameter (equal-power - the
        engines are uncorrelated) before the envelope. The SVF filter and ENV2
        land here in M4.
    */
    /** Per-block host context, written once per processBlock on the audio thread
        and read by voices (same thread; atomics for honesty). */
    struct BlockContext
    {
        std::atomic<double> bpm { 120.0 }; // 120 = deterministic no-host fallback

        // Latest wheel/pressure, captured at the SYNTH level (KeepsakeSynth
        // overrides handleController etc.): juce::Synthesiser only dispatches
        // controller events to voices that are already playing, so a per-voice
        // capture never sees a wheel moved before the note starts. Written at
        // sample-accurate event splits on the audio thread, read at control
        // ticks - block-size invariant.
        std::atomic<float> modWheel01 { 0.0f };
        std::atomic<float> aftertouch01 { 0.0f };
    };

    class KeepsakeVoice : public juce::SynthesiserVoice
    {
    public:
        /** Modulation/parameter evaluation cadence, in samples, anchored to the
            voice-local clock (samples since noteOn) - never to host block
            boundaries. juce::Synthesiser splits blocks at MIDI positions, so the
            tick grid is identical at any host block size; that anchoring is what
            keeps the block-size-independence renders bit-identical even with
            modulation active. */
        static constexpr int kControlTickSamples = 32;

        KeepsakeVoice (const params::Handles& handles, const SourceStore& store,
                       const WavetableStore& wavetables, const BlockContext& context,
                       int voiceIndex);

        bool canPlaySound (juce::SynthesiserSound* sound) override
        {
            return dynamic_cast<KeepsakeSound*> (sound) != nullptr;
        }

        void startNote (int midiNoteNumber, float velocity,
                        juce::SynthesiserSound*, int currentPitchWheelPosition) override;

        /** Mono/legato path: change the sounding pitch (with glide) without
            restarting the audio path. retrigger=true (Mono) restarts both
            envelopes and retrig-enabled LFOs; retrigger=false (Legato) leaves
            them running. MIDI cannot express this - which is why mono lives in
            a Synthesiser subclass with voice access, not in a MIDI filter. */
        void changeNote (int newNote, float velocity, bool retrigger);
        void stopNote (float velocity, bool allowTailOff) override;
        void pitchWheelMoved (int newValue) override;
        void controllerMoved (int, int) override {} // wheel captured in BlockContext

        void setCurrentPlaybackSampleRate (double newRate) override;
        void renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                              int startSample, int numSamples) override;

        bool isVoiceActive() const override { return adsr.isActive(); }

        int getActiveGrainCount() const noexcept { return cloud.getActiveGrainCount(); }

        /** Message thread. Appends the wavetable sets this voice still holds, for
            WavetableStore::collectGarbage's pin list. */
        void getPinnedWavetableSets (std::vector<const WavetableSet*>& out) const
        {
            tone.getPinnedSets (out);
        }

        ToneEngine& getToneEngineForTests() noexcept { return tone; }

    private:
        void updateEnvelopeParameters();
        double computePlaybackRatio() const;

        /** Reads parameters + (from M4 step 4) the mod matrix into the per-tick
            state below. Called at sample 0 of the note and every tick after. */
        void evaluateControlTick (const SourceAudio* source);

        const params::Handles& params;
        const SourceStore& sources;
        const WavetableStore& wavetables;
        const BlockContext& blockContext;

        GrainEngine cloud;
        ToneEngine tone;
        juce::ADSR env2;
        juce::ADSR::Parameters env2Params;
        LFO lfo1, lfo2;
        juce::dsp::StateVariableTPTFilter<float> filter; // Focus blend -> SVF -> amp env
        float appliedCutoff = -1.0f, appliedResonance = -1.0f;
        int appliedFilterType = -1;
        // Cutoff is smoothed in octave (log2) domain at tick rate: per-tick cutoff
        // steps at high resonance are an audible zipper. Snapped at note start so
        // the static-parameter path stays bit-exact.
        double cutoffOctavesSmoothed = 0.0;
        bool cutoffNeedsSnap = true;
        juce::ADSR adsr;
        juce::ADSR::Parameters adsrParams;

        juce::AudioBuffer<float> voiceBuffer; // pre-allocated in setCurrentPlaybackSampleRate
        juce::AudioBuffer<float> toneBuffer;  // mono Tone scratch, same capacity
        juce::LinearSmoothedValue<float> cloudGain, toneGain; // equal-power focus blend

        int noteNumber = 60;
        float noteVelocity = 1.0f;
        double pitchWheelSemitones = 0.0;

        // Glide: constant-time, linear in semitones (exponential in Hz), stepped
        // on the control-tick grid - deterministic. Feeds BOTH engines and the
        // filter keytrack through glidedNote().
        double glideCurrentNote = 60.0;
        double glideTargetNote = 60.0;
        double glideStepPerTick = 0.0;
        int glideTicksRemaining = 0;

        // Voice-local sample clock; the control-tick grid divides this.
        juce::int64 voiceClock = 0;
        int voiceIndex = 0; // detune sign alternation (FocusCoupling)

        // Per-tick evaluated state, consumed by the render loop between ticks.
        ModSnapshot snapshot; // defaults = no modulation until the matrix lands
        GrainEngine::Settings cloudSettings;
        double toneFrequency = 440.0;
        double framePosTarget = 0.0;
        const WavetableSet* latestWavetableSet = nullptr;
    };
}
