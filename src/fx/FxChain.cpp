#include "FxChain.h"

namespace keepsake
{
    namespace
    {
        // Warmth: y = x + a * (tanh(d x) * makeup - x), with makeup chosen so
        // the curve has unity gain at a reference level. The reference (rather
        // than peak or small-signal normalization) is what "drive
        // compensation" buys: peak normalization turns quiet material up by
        // the full drive factor, and small-signal normalization turns hot
        // material down by it; anchoring unity at a typical program level
        // keeps the knob a color control, not a volume control. The warmth
        // level test owns these constants.
        constexpr float kWarmthMaxExtraDrive = 5.0f; // d = 1 + 5a
        constexpr float kWarmthReferenceLevel = 0.25f;

        // Chorus Amount macro (spec: "one Amount macro (maps rate/depth/mix)").
        // Slow-and-shallow at low amounts, wobblier and wetter toward 1. All
        // values inside the asserted juce::dsp::Chorus limits.
        void applyChorusMacro (juce::dsp::Chorus<float>& chorus, float a)
        {
            chorus.setRate (0.25f + 1.25f * a);
            chorus.setDepth (0.15f + 0.30f * a);
            chorus.setCentreDelay (7.0f);
            chorus.setFeedback (0.0f);
            chorus.setMix (0.5f * a);
        }

        // juce::dsp::Chorus mixes dry/wet by the LINEAR rule: at mix m the
        // output is (1-m) dry + m wet, which for the mostly-uncorrelated wet
        // path is a measured ~-3.8dB dip at m = 0.5. Undo the power sum so the
        // Amount knob thickens without ducking (the chorus level test owns
        // this): makeup = 1 / sqrt((1-m)^2 + m^2).
        float chorusMakeupFor (float a)
        {
            const auto m = 0.5f * juce::jlimit (0.0f, 1.0f, a);
            return 1.0f / std::sqrt ((1.0f - m) * (1.0f - m) + m * m);
        }

        juce::Reverb::Parameters reverbParameters (float airSize01)
        {
            juce::Reverb::Parameters p;
            p.roomSize = juce::jlimit (0.0f, 1.0f, airSize01);
            p.damping = 0.4f;
            p.wetLevel = 1.0f;
            p.dryLevel = 0.0f; // pure return; the dry path never enters the reverb
            p.width = 1.0f;
            p.freezeMode = 0.0f; // >= 0.5 latches the tank - never a reverb
            return p;
        }

        // Twice the largest internal smoothing constant in juce::dsp::Chorus
        // (oscVolume and the DryWetMixer both use 50ms), so the hard bypass
        // lands only after the dry gain has settled exactly.
        constexpr double kChorusSettleSeconds = 0.1;

        // The reverb rings out until its own output has been below this for
        // this long - level-based, so a 100% room size is not truncated.
        constexpr float kReverbSilenceThreshold = 1.0e-6f;
        constexpr double kReverbSilenceSeconds = 0.25;
    }

    void FxChain::prepare (double sampleRate, int maxBlockSize, int numChannels,
                           float warmth01, float airSize01)
    {
        rate = sampleRate;
        preparedBlockSize = juce::jmax (kMaxBlockSamples, maxBlockSize);
        channels = juce::jlimit (1, 2, numChannels);

        // Snap - a preset can restore parameters before prepareToPlay, and the
        // first render must not ramp from a stale value (outputGain precedent).
        warmth.reset (sampleRate, 0.02);
        warmth.setCurrentAndTargetValue (juce::jlimit (0.0f, 1.0f, warmth01));

        // Macro targets first, then prepare: Chorus::prepare ends in reset(),
        // which snaps every internal smoother to its target. Without this the
        // first engagement would ramp from the class defaults (mix 0.5).
        applyChorusMacro (chorus, 0.0f);
        chorus.prepare ({ sampleRate, (juce::uint32) preparedBlockSize,
                          (juce::uint32) channels });
        chorusMakeup.reset (sampleRate, 0.02);
        chorusMakeup.setCurrentAndTargetValue (1.0f);
        chorusRunning = false;
        chorusCooldown = 0;

        // Parameters BEFORE setSampleRate, whose reset snaps the gain
        // smoothers (see the header comment - construction defaults include
        // dry 0.4, and setParameters only sets targets).
        lastRoomSize = juce::jlimit (0.0f, 1.0f, airSize01);
        reverb.setParameters (reverbParameters (lastRoomSize));
        reverb.setSampleRate (sampleRate);
        reverbRunning = false;
        reverbSilentStreak = 0;

        reverbScratch.setSize (channels, preparedBlockSize);
        reverbScratch.clear();
    }

    void FxChain::process (juce::AudioBuffer<float>& buffer,
                           const juce::AudioBuffer<float>& wetSend,
                           float warmth01, float chorus01,
                           float airSize01, float airMix01, bool reverbRouted)
    {
        const auto total = buffer.getNumSamples();

        for (int start = 0; start < total; start += preparedBlockSize)
            processSlice (buffer, start, juce::jmin (preparedBlockSize, total - start),
                          wetSend, warmth01, chorus01, airSize01, airMix01, reverbRouted);
    }

    void FxChain::processSlice (juce::AudioBuffer<float>& buffer, int start, int numSamples,
                                const juce::AudioBuffer<float>& wetSend,
                                float warmth01, float chorus01,
                                float airSize01, float airMix01, bool reverbRouted)
    {
        const auto numChannels = juce::jmin (buffer.getNumChannels(), channels);

        // --- 1. Warmth ------------------------------------------------------
        // The smoothed amount is the click protection (it crossfades dry and
        // shaped); drive and makeup follow the block's target - any step in
        // them is masked by the crossfade, and one tanh per slice beats three
        // per sample.
        warmth.setTargetValue (juce::jlimit (0.0f, 1.0f, warmth01));

        if (warmth.getCurrentValue() > 0.0f || warmth.getTargetValue() > 0.0f)
        {
            const auto drive = 1.0f + kWarmthMaxExtraDrive * warmth.getTargetValue();
            const auto makeup = kWarmthReferenceLevel
                                  / std::tanh (drive * kWarmthReferenceLevel);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto a = warmth.getNextValue();

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* d = buffer.getWritePointer (ch, start);
                    d[i] += a * (std::tanh (drive * d[i]) * makeup - d[i]);
                }
            }
        }

        // --- 2. Chorus ------------------------------------------------------
        const auto chorusActive = chorus01 > 0.0f;

        if (chorusActive)
            chorusCooldown = (int) std::llround (kChorusSettleSeconds * rate);

        if (chorusActive || chorusCooldown > 0)
        {
            if (! chorusRunning)
            {
                // Re-entry: the delay lines hold audio from before the last
                // bypass; reset FIRST (it snaps smoothers to their old,
                // faded-out targets), THEN set the new macro targets so the
                // wet path fades in instead of popping.
                chorus.reset();
                chorusRunning = true;
            }

            applyChorusMacro (chorus, juce::jlimit (0.0f, 1.0f, chorus01));
            chorusMakeup.setTargetValue (chorusMakeupFor (chorus01));

            auto block = juce::dsp::AudioBlock<float> (buffer)
                             .getSubBlock ((size_t) start, (size_t) numSamples)
                             .getSubsetChannelBlock (0, (size_t) numChannels);
            juce::dsp::ProcessContextReplacing<float> context (block);
            chorus.process (context);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto g = chorusMakeup.getNextValue();

                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.getWritePointer (ch, start)[i] *= g;
            }

            if (! chorusActive)
            {
                chorusCooldown = juce::jmax (0, chorusCooldown - numSamples);
                chorusRunning = chorusCooldown > 0;
            }
        }
        else
        {
            // The 100ms cooldown outlasts the 20ms makeup smoothing, so the
            // gain has always settled back to exactly 1 before the bypass.
            chorusMakeup.setCurrentAndTargetValue (1.0f);
        }

        // --- 3. Air (send-return reverb) ------------------------------------
        // Runs while the Mix knob is up OR any mod slot routes to Reverb Mix
        // (the per-voice send must be audible at Mix 0 - that is its point),
        // then rings out until its own output decays before the hard bypass.
        const auto reverbActive = airMix01 > 0.0f || reverbRouted;

        if (reverbActive)
        {
            reverbRunning = true;
            reverbSilentStreak = 0;
        }

        if (reverbRunning)
        {
            // Only roomSize ever changes (wet/dry/width are fixed); its 10ms
            // internal ramp is per-sample and deterministic.
            const auto roomSize = juce::jlimit (0.0f, 1.0f, airSize01);

            if (! juce::exactlyEqual (roomSize, lastRoomSize))
            {
                lastRoomSize = roomSize;
                reverb.setParameters (reverbParameters (roomSize));
            }

            // Send bus = main x Mix + per-voice sends. The per-voice bus can
            // be undersized if a host exceeds every declared maximum; whatever
            // lies beyond its capacity simply was not collected.
            const auto sendGain = juce::jlimit (0.0f, 1.0f, airMix01);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                reverbScratch.copyFrom (ch, 0, buffer, ch, start, numSamples);
                reverbScratch.applyGain (ch, 0, numSamples, sendGain);

                const auto avail = juce::jlimit (0, numSamples,
                                                 wetSend.getNumSamples() - start);

                if (avail > 0 && wetSend.getNumChannels() > 0)
                    reverbScratch.addFrom (ch, 0, wetSend,
                                           juce::jmin (ch, wetSend.getNumChannels() - 1),
                                           start, avail);
            }

            if (numChannels >= 2)
                reverb.processStereo (reverbScratch.getWritePointer (0),
                                      reverbScratch.getWritePointer (1), numSamples);
            else
                reverb.processMono (reverbScratch.getWritePointer (0), numSamples);

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.addFrom (ch, start, reverbScratch, ch, 0, numSamples);

            if (! reverbActive)
            {
                const auto level = reverbScratch.getMagnitude (0, numSamples);

                reverbSilentStreak = level < kReverbSilenceThreshold
                                       ? reverbSilentStreak + numSamples
                                       : 0;

                if (reverbSilentStreak >= (int) (kReverbSilenceSeconds * rate))
                {
                    // Fully decayed: stop, and clear the tank so no residue
                    // replays when the reverb next engages.
                    reverbRunning = false;
                    reverbSilentStreak = 0;
                    reverb.reset();
                }
            }
        }
    }
}
