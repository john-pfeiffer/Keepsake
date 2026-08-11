#include "Voice.h"
#include "Analysis.h"
#include "FocusCoupling.h"

namespace keepsake
{
    namespace
    {
        constexpr int kMaxBlockSize = 8192; // voice scratch buffer capacity
    }

    KeepsakeVoice::KeepsakeVoice (const params::Handles& handles, const SourceStore& store,
                                  const WavetableStore& wavetableStore,
                                  const BlockContext& context, int voiceIndexIn)
        : params (handles), sources (store), wavetables (wavetableStore),
          blockContext (context),
          // Distinct deterministic S&H streams per voice and per LFO, same
          // precedent as the grain RNG seeding below.
          lfo1 (0x1F0 + voiceIndexIn * 7919),
          lfo2 (0x1F0 + voiceIndexIn * 7919 + 104729)
    {
        voiceIndex = voiceIndexIn;
        // Each voice gets a distinct RNG stream, otherwise stacked notes would spawn
        // identical grain patterns and sum coherently into a comb filter.
        cloud.setSeed (0x5EED + voiceIndex * 7919);
        voiceBuffer.setSize (2, kMaxBlockSize);
        toneBuffer.setSize (1, kMaxBlockSize);
    }

    void KeepsakeVoice::setCurrentPlaybackSampleRate (double newRate)
    {
        juce::SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);

        if (newRate > 0.0)
        {
            cloud.prepare (newRate);
            tone.prepare (newRate); // also drops held wavetable pointers (GC safety)
            filter.prepare ({ newRate, (juce::uint32) voiceBuffer.getNumSamples(), 2 });
            appliedCutoff = appliedResonance = -1.0f; // force re-apply on next tick
            appliedFilterType = -1;
            adsr.setSampleRate (newRate);
            env2.setSampleRate (newRate);

            cloudGain.reset (newRate, 0.02);
            toneGain.reset (newRate, 0.02);
            wetSend.reset (newRate, 0.015);
            wetSend.setCurrentAndTargetValue (0.0f);
        }
    }

    void KeepsakeVoice::updateEnvelopeParameters()
    {
        adsrParams.attack  = juce::jmax (0.001f, params.ampAttack->load()  * 0.001f);
        adsrParams.decay   = juce::jmax (0.001f, params.ampDecay->load()   * 0.001f);
        adsrParams.sustain = juce::jlimit (0.0f, 1.0f, params.ampSustain->load());
        adsrParams.release = juce::jmax (0.001f, params.ampRelease->load() * 0.001f);
        adsr.setParameters (adsrParams);

        env2Params.attack  = juce::jmax (0.001f, params.env2Attack->load()  * 0.001f);
        env2Params.decay   = juce::jmax (0.001f, params.env2Decay->load()   * 0.001f);
        env2Params.sustain = juce::jlimit (0.0f, 1.0f, params.env2Sustain->load());
        env2Params.release = juce::jmax (0.001f, params.env2Release->load() * 0.001f);
        env2.setParameters (env2Params);
    }

    double KeepsakeVoice::computePlaybackRatio() const
    {
        // Spec §2.2: grains are repitched by playback-rate scaling relative to Root.
        // Pitch modulation feeds BOTH engines (here and in toneFrequency) so
        // mid-Focus vibrato tracks as one instrument.
        const auto root = (double) params.rootNote->load() + (double) params.rootCents->load() * 0.01;
        const auto semitones = (glideCurrentNote + pitchWheelSemitones
                                + (double) snapshot.pitchSemitones) - root;

        return std::pow (2.0, semitones / 12.0);
    }

    void KeepsakeVoice::startNote (int midiNoteNumber, float velocity,
                                   juce::SynthesiserSound*, int currentPitchWheelPosition)
    {
        noteNumber = midiNoteNumber;
        noteVelocity = juce::jlimit (0.0f, 1.0f, velocity);
        glideCurrentNote = glideTargetNote = (double) midiNoteNumber;
        glideTicksRemaining = 0;
        pitchWheelMoved (currentPitchWheelPosition);

        cloud.reset();
        filter.reset();
        cutoffNeedsSnap = true;
        voiceClock = 0; // re-anchor the control-tick grid to this note

        // The reverb send fades in from silence over its smoothing time - a
        // voice must not open a note with a full-level send step.
        wetSend.setCurrentAndTargetValue (0.0f);

        updateEnvelopeParameters();
        env2.noteOn();
        lfo1.noteOn (params.lfoRetrig[0]->load() >= 0.5f);
        lfo2.noteOn (params.lfoRetrig[1]->load() >= 0.5f);
        tone.noteOn (wavetables.getForAudioThread()); // capture the set cold, no fade
        updateEnvelopeParameters();
        adsr.noteOn();

        // Snap the focus blend at note start so the attack is not a fade artefact.
        const auto focus = juce::jlimit (0.0f, 1.0f, params.focus->load());
        cloudGain.setCurrentAndTargetValue (std::cos (focus * juce::MathConstants<float>::halfPi));
        toneGain.setCurrentAndTargetValue (std::sin (focus * juce::MathConstants<float>::halfPi));
    }

    void KeepsakeVoice::stopNote (float, bool allowTailOff)
    {
        if (allowTailOff)
        {
            adsr.noteOff();
            env2.noteOff();
        }
        else
        {
            // Voice stealing path (spec §2.5: "quick fade-out"). The Synthesiser calls
            // this with allowTailOff=false; clearing immediately would click, so we
            // hand off to a very short release instead of cutting the voice dead.
            auto stealParams = adsrParams;
            stealParams.release = 0.005f;
            adsr.setParameters (stealParams);
            adsr.noteOff();
            env2.noteOff();
        }
    }

    void KeepsakeVoice::changeNote (int newNote, float velocity, bool retrigger)
    {
        // The base class's currentlyPlayingNote goes stale here (it is private,
        // setter-less). Safe: in Mono/Legato every noteOff routes through
        // KeepsakeSynth's own stack, never the base class's note matching, and a
        // mode switch calls allNotesOff - the stale value is never consulted.
        noteNumber = newNote;
        noteVelocity = juce::jlimit (0.0f, 1.0f, velocity);

        glideTargetNote = (double) newNote;

        const auto glideSeconds = (double) params.glideTime->load() * 0.001;
        const auto ticks = juce::jmax (1, (int) std::round (glideSeconds * getSampleRate()
                                                            / (double) kControlTickSamples));
        glideStepPerTick = (glideTargetNote - glideCurrentNote) / (double) ticks;
        glideTicksRemaining = ticks;

        if (retrigger)
        {
            adsr.noteOn();
            env2.noteOn();
            lfo1.noteOn (params.lfoRetrig[0]->load() >= 0.5f);
            lfo2.noteOn (params.lfoRetrig[1]->load() >= 0.5f);
        }
    }

    void KeepsakeVoice::pitchWheelMoved (int newValue)
    {
        // +/- 2 semitones, the conventional default range.
        pitchWheelSemitones = ((double) newValue - 8192.0) / 8192.0 * 2.0;
    }

    void KeepsakeVoice::renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                                         int startSample, int numSamples)
    {
        if (! adsr.isActive() || numSamples <= 0)
            return;

        const auto* source = sources.getForAudioThread();

        if (source == nullptr)
        {
            // No keepsake loaded: still advance the envelope so that a released note
            // frees its voice instead of hanging forever waiting for audio that will
            // never arrive.
            for (int i = 0; i < numSamples; ++i)
                adsr.getNextSample();

            voiceClock += numSamples; // keep the tick grid note-anchored regardless

            if (! adsr.isActive())
                clearCurrentNote();

            return;
        }

        // Render in tick-bounded chunks: every chunk ends at a control-tick
        // boundary (or the block end), so parameter/mod evaluation happens at
        // identical voice-local positions whatever the host block size. Chunks
        // are also capped by the scratch capacity (allocating here would break
        // RT safety).
        int offset = 0;

        while (offset < numSamples)
        {
            if (voiceClock % kControlTickSamples == 0)
                evaluateControlTick (source);

            const auto toNextTick =
                kControlTickSamples - (int) (voiceClock % kControlTickSamples);
            const auto chunk = juce::jmin (juce::jmin (numSamples - offset, toNextTick),
                                           voiceBuffer.getNumSamples());

            voiceBuffer.clear (0, chunk);

            // Exactly zero gain skips an engine outright. Beyond saving CPU this
            // guarantees Focus at 0 renders bit-identically to the pre-Tone (M2)
            // engine, which a regression test asserts.
            const auto cloudActive = cloudGain.getCurrentValue() > 0.0f
                                     || cloudGain.getTargetValue() > 0.0f;
            const auto toneActive = toneGain.getCurrentValue() > 0.0f
                                    || toneGain.getTargetValue() > 0.0f;

            if (cloudActive)
                cloud.process (voiceBuffer, 0, chunk, source, cloudSettings);

            if (toneActive)
                tone.process (toneBuffer.getWritePointer (0), chunk,
                              latestWavetableSet, toneFrequency, framePosTarget);

            {
                auto* left = voiceBuffer.getWritePointer (0);
                auto* right = voiceBuffer.getWritePointer (1);
                const auto* toneMono = toneBuffer.getReadPointer (0);

                for (int i = 0; i < chunk; ++i)
                {
                    const auto cg = cloudGain.getNextValue();
                    const auto tg = toneGain.getNextValue();
                    const auto t = toneActive ? toneMono[i] * tg : 0.0f;

                    left[i] = left[i] * cg + t;
                    right[i] = right[i] * cg + t;
                }
            }

            {
                // Spec signal flow: Focus x-fade -> SVF -> amp envelope.
                auto block = juce::dsp::AudioBlock<float> (voiceBuffer)
                                 .getSubBlock (0, (size_t) chunk);
                juce::dsp::ProcessContextReplacing<float> context (block);
                filter.process (context);
            }

            adsr.applyEnvelopeToBuffer (voiceBuffer, 0, chunk);

            const auto velocityGain = 0.3f + 0.7f * noteVelocity;

            for (int ch = 0; ch < outputBuffer.getNumChannels(); ++ch)
                outputBuffer.addFrom (ch, startSample + offset,
                                      voiceBuffer, juce::jmin (ch, 1), 0,
                                      chunk, velocityGain);

            // Per-voice reverb send (M5 Air): the same post-filter,
            // post-envelope signal, scaled by the smoothed snapshot send
            // level. Per-sample getNextValue like the blend gains - a
            // per-chunk gain ramp would round differently when a control tick
            // straddles a host block boundary and break the bit-identical
            // block-size guarantee. The active test mirrors cloudActive:
            // current OR target nonzero, so a ramp-down keeps its tail.
            {
                const auto sendActive = wetSend.getCurrentValue() > 0.0f
                                        || wetSend.getTargetValue() > 0.0f;

                if (sendActive && wetSendBus != nullptr)
                {
                    const auto pos = startSample + offset;
                    const auto collected = juce::jlimit (0, chunk,
                                                         wetSendBus->getNumSamples() - pos);
                    const auto busChannels = wetSendBus->getNumChannels();

                    for (int i = 0; i < chunk; ++i)
                    {
                        const auto g = velocityGain * wetSend.getNextValue();

                        if (i < collected)
                            for (int ch = 0; ch < busChannels; ++ch)
                                wetSendBus->getWritePointer (ch)[pos + i]
                                    += voiceBuffer.getReadPointer (juce::jmin (ch, 1))[i] * g;
                    }
                }
                else
                {
                    wetSend.skip (chunk); // keep the smoother on the sample clock
                }
            }

            offset += chunk;
            voiceClock += chunk;
        }

        if (! adsr.isActive())
            clearCurrentNote();
    }

    void KeepsakeVoice::evaluateControlTick (const SourceAudio* source)
    {
        juce::ignoreUnused (source);

        updateEnvelopeParameters();

        const auto sampleRate = getSampleRate();

        if (glideTicksRemaining > 0)
        {
            glideCurrentNote += glideStepPerTick;

            if (--glideTicksRemaining == 0)
                glideCurrentNote = glideTargetNote; // land exactly
        }

        // --- Advance the per-voice mod sources by one tick (except at note
        // start: the first evaluation reads initial values so the attack is
        // correct), then evaluate the matrix into the snapshot. ---
        ModSources modSources;

        auto lfoRate = [this, sampleRate] (int i)
        {
            if (params.lfoSync[i]->load() >= 0.5f)
                return LFO::syncedRateHz (blockContext.bpm.load (std::memory_order_relaxed),
                                          (int) std::lround (params.lfoDivision[i]->load()));

            return (double) params.lfoRate[i]->load();
        };

        // Advance exactly one tick of real time per evaluation (none at note
        // start - the first evaluation reads initial values). juce::ADSR has no
        // peek, so the last getNextSample IS the tick's value.
        const auto advance = voiceClock > 0 ? kControlTickSamples : 0;

        if (advance > 0)
        {
            for (int i = 0; i < advance - 1; ++i)
                env2.getNextSample();

            modSources.env2 = env2.getNextSample();
        }
        else
        {
            modSources.env2 = 0.0f;
        }

        modSources.lfo1 = lfo1.advance ((int) std::lround (params.lfoShape[0]->load()),
                                        lfoRate (0), advance, sampleRate);
        modSources.lfo2 = lfo2.advance ((int) std::lround (params.lfoShape[1]->load()),
                                        lfoRate (1), advance, sampleRate);
        modSources.velocity = noteVelocity;
        modSources.modWheel = blockContext.modWheel01.load (std::memory_order_relaxed);
        modSources.aftertouch = blockContext.aftertouch01.load (std::memory_order_relaxed);

        ModSlotValues slots[mod::kNumSlots];

        for (int i = 0; i < mod::kNumSlots; ++i)
        {
            slots[i].source = (int) std::lround (params.modSource[i]->load());
            slots[i].dest   = (int) std::lround (params.modDest[i]->load());
            slots[i].depth  = params.modDepth[i]->load() * 0.01f;
        }

        snapshot = evaluateModMatrix (slots, modSources);
        wetSend.setTargetValue (snapshot.reverbSend); // consumed by the M5 Air send

        // --- Apply: normalized-domain combine on every destination that has an
        // underlying parameter (offset 0 short-circuits, keeping the static path
        // bit-identical). ---
        auto modded = [this] (int dest, std::atomic<float>* param, float offset)
        {
            return applyNormalizedMod (*params.destRange[dest], param->load(), offset);
        };

        // Post-mod focus drives both the blend and the coupling curves (§2.4).
        const auto focus = juce::jlimit (0.0f, 1.0f,
                                         modded (mod::destFocus, params.focus, snapshot.focus));

        cloudSettings.place           = (double) modded (mod::destPlace, params.place, snapshot.place);
        cloudSettings.captureLengthMs = (double) params.captureLength->load();
        cloudSettings.grainSizeMs     = (double) modded (mod::destGrainSize, params.grainSize, snapshot.grainSize)
                                          * (double) coupling::grainSizeMultiplier (focus);
        cloudSettings.densityPerSecond = (double) modded (mod::destDensity, params.grainDensity, snapshot.density)
                                          * (double) coupling::densityMultiplier (focus);

        // Tempo-synced emission: the division grid replaces Density outright,
        // and the focus density-coupling is deliberately NOT applied - a
        // rhythm must not double mid-morph (focus still couples grain size).
        // Reuses the LFOs' frozen division list + sync math.
        if (params.grainSync->load() >= 0.5f)
        {
            const auto bpm = blockContext.bpm.load (std::memory_order_relaxed);
            const auto division = (int) std::lround (params.grainDivision->load());
            cloudSettings.syncIntervalSamples = sampleRate / LFO::syncedRateHz (bpm, division);
        }
        else
        {
            cloudSettings.syncIntervalSamples = 0.0;
        }
        cloudSettings.drift           = (double) modded (mod::destDrift, params.grainDrift, snapshot.drift) * 0.01;
        cloudSettings.shimmerCents    = (double) modded (mod::destShimmer, params.grainShimmer, snapshot.shimmer)
                                          * (double) coupling::shimmerMultiplier (focus);
        cloudSettings.windowMorph     = (double) params.grainWindow->load();
        cloudSettings.spread          = (double) modded (mod::destSpread, params.grainSpread, snapshot.spread) * 0.01;
        cloudSettings.playbackRatio   = computePlaybackRatio();

        // The blend gains come from the coupling curves: cloud side is the M3
        // equal-power cosine; tone ducks earlier than linear toward Cloud. Targets
        // smoothed so Focus motion does not zipper.
        cloudGain.setTargetValue (coupling::cloudGain (focus));
        toneGain.setTargetValue (coupling::toneGain (focus) * coupling::kToneEngineTrim);

        const auto detuneCents = coupling::toneDetuneCents (focus, voiceIndex);
        toneFrequency = analysis::noteFrequencyHz (glideCurrentNote + pitchWheelSemitones
                                                   + (double) snapshot.pitchSemitones
                                                   + (double) detuneCents * 0.01);

        // Frame: combine plainly (linear 0..1 range), then fold by the wrap mode.
        // The fold happens HERE, not in ToneEngine - its 0..1 contract stays
        // pristine, and the 15ms frame smoothing downstream turns a Loop wrap
        // into a fast correlated sweep instead of a click.
        {
            const auto base = params.toneFrame->load();

            if (juce::exactlyEqual (snapshot.frame, 0.0f))
            {
                framePosTarget = (double) base; // bit-identical static path
            }
            else
            {
                const auto x = base + snapshot.frame;

                if (params.toneFrameWrap->load() >= 0.5f) // Ping-Pong
                {
                    auto t = std::fmod ((double) x, 2.0);
                    if (t < 0.0) t += 2.0;
                    framePosTarget = t <= 1.0 ? t : 2.0 - t;
                }
                else // Loop
                {
                    framePosTarget = (double) x - std::floor ((double) x);
                }
            }
        }

        latestWavetableSet = wavetables.getForAudioThread();

        // Filter, updated per tick and only when values actually moved: the
        // epsilon keeps the static-parameter path bit-stable, and each
        // setCutoffFrequency costs a tan(). TPT SVF stays stable under fast
        // cutoff motion, which is why per-tick (not per-sample) is enough even
        // for a 20Hz cutoff LFO.
        const auto keytrack = (double) params.filterKeytrack->load() * 0.01;
        auto cutoffTarget = (double) modded (mod::destCutoff, params.filterCutoff, snapshot.cutoff)
                        * std::pow (2.0, (glideCurrentNote - 60.0) / 12.0 * keytrack);
        cutoffTarget = juce::jlimit (20.0, 0.45 * sampleRate, cutoffTarget);

        // One-pole in octave domain (~8ms) against per-tick zipper under cutoff
        // modulation; a static cutoff snaps at note start and never drifts.
        const auto targetOctaves = std::log2 (cutoffTarget);

        if (cutoffNeedsSnap)
        {
            cutoffOctavesSmoothed = targetOctaves;
            cutoffNeedsSnap = false;
        }
        else if (! juce::exactlyEqual (cutoffOctavesSmoothed, targetOctaves))
        {
            constexpr double alpha = 0.08; // tau ~8ms at a 32-sample tick, 48k
            cutoffOctavesSmoothed += alpha * (targetOctaves - cutoffOctavesSmoothed);

            if (std::abs (cutoffOctavesSmoothed - targetOctaves) < 1.0e-4)
                cutoffOctavesSmoothed = targetOctaves; // settle exactly
        }

        const auto cutoff = std::exp2 (cutoffOctavesSmoothed);

        const auto resonance = params.filterResonance->load();
        const auto type = (int) std::lround (params.filterType->load());

        if (type != appliedFilterType)
        {
            appliedFilterType = type;
            filter.setType (type == 1 ? juce::dsp::StateVariableTPTFilterType::bandpass
                            : type == 2 ? juce::dsp::StateVariableTPTFilterType::highpass
                                        : juce::dsp::StateVariableTPTFilterType::lowpass);
        }

        if (std::abs ((float) cutoff - appliedCutoff) > 0.01f)
        {
            appliedCutoff = (float) cutoff;
            filter.setCutoffFrequency ((float) cutoff);
        }

        if (std::abs (resonance - appliedResonance) > 1.0e-4f)
        {
            appliedResonance = resonance;
            filter.setResonance (resonance);
        }
    }
}
