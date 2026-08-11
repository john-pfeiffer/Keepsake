#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/**
    Central definition of every automatable parameter in Keepsake.

    Spec §4: "every user-facing control is registered in AudioProcessorValueTreeState
    and therefore exposed to host automation from v1 ... No hidden/UI-only parameters."

    Anything the user can change lives here. UI components attach to these IDs; nothing
    in the UI is allowed to hold state of its own. The one exception is the audition
    transport (play/stop buttons), which is a momentary action rather than a control
    value and is handled by PluginProcessor::AuditionState.
*/
namespace keepsake::params
{
    // --- Capture ------------------------------------------------------------
    inline constexpr auto place        = "place";         // 0..1 through the source file
    inline constexpr auto captureLength = "captureLength"; // 10 ms..10 s

    // Keep-window length bounds, shared by the parameter range and the
    // waveform bracket drag so they cannot drift apart. The 10s ceiling gives
    // Warp a full 4-bar phrase at 120 BPM to sweep.
    inline constexpr float kKeepLengthMinMs = 10.0f;
    inline constexpr float kKeepLengthMaxMs = 10000.0f;

    // --- Root (drives Cloud repitch, and Tone cycle extraction in M3) --------
    inline constexpr auto rootNote  = "rootNote";  // MIDI note number
    inline constexpr auto rootCents = "rootCents"; // -50..+50 cents

    // --- Cloud (granular) engine -------------------------------------------
    inline constexpr auto grainSize    = "grainSize";    // 5..250 ms
    inline constexpr auto grainDensity = "grainDensity"; // 2..200 grains/s
    inline constexpr auto grainDrift   = "grainDrift";   // 0..100 %
    inline constexpr auto grainShimmer = "grainShimmer"; // 0..100 cents
    inline constexpr auto grainWindow  = "grainWindow";  // 0..1 Hann->Tukey->Expodec
    inline constexpr auto grainSpread  = "grainSpread";  // 0..100 %
    inline constexpr auto grainSync     = "grainSync";     // choice: Free/Sync
    inline constexpr auto grainDivision = "grainDivision"; // frozen 12-entry list

    // --- Warp / Snap (tempo-mapped Keep window, transient slicing) ----------
    inline constexpr auto warpMode  = "warpMode";  // choice: Off..4 Bars (frozen)
    inline constexpr auto grainSnap = "grainSnap"; // choice: Off/Transients

    // --- Cloud pitch behaviour ----------------------------------------------
    inline constexpr auto pitchMode = "pitchMode"; // choice: Repitch/Formant

    // --- Tone (wavetable) engine --------------------------------------------
    inline constexpr auto focus         = "focus";         // 0..1 Cloud->Tone
    inline constexpr auto toneFrame     = "toneFrame";     // 0..1 scan across frames
    inline constexpr auto toneFrames    = "toneFrames";    // choice: 2/4/8/16
    inline constexpr auto toneFrameWrap = "toneFrameWrap"; // choice: Loop/Ping-Pong

    // --- Filter (per voice, Focus blend -> SVF -> amp env) -------------------
    inline constexpr auto filterType      = "filterType";      // choice LP/BP/HP
    inline constexpr auto filterCutoff    = "filterCutoff";    // 20..20000 Hz
    inline constexpr auto filterResonance = "filterResonance"; // Q 0.5..10
    inline constexpr auto filterKeytrack  = "filterKeytrack";  // 0..100 %

    // --- ENV2 (freely assignable) -------------------------------------------
    inline constexpr auto env2Attack  = "env2Attack";
    inline constexpr auto env2Decay   = "env2Decay";
    inline constexpr auto env2Sustain = "env2Sustain";
    inline constexpr auto env2Release = "env2Release";

    // --- LFO 1/2 -------------------------------------------------------------
    inline constexpr auto lfo1Shape    = "lfo1Shape";    // Sine/Tri/Saw/S&H
    inline constexpr auto lfo1Rate     = "lfo1Rate";     // 0.02..20 Hz
    inline constexpr auto lfo1Sync     = "lfo1Sync";     // Free/Sync
    inline constexpr auto lfo1Division = "lfo1Division"; // frozen 12-entry list
    inline constexpr auto lfo1Retrig   = "lfo1Retrig";   // Off/On
    inline constexpr auto lfo2Shape    = "lfo2Shape";
    inline constexpr auto lfo2Rate     = "lfo2Rate";
    inline constexpr auto lfo2Sync     = "lfo2Sync";
    inline constexpr auto lfo2Division = "lfo2Division";
    inline constexpr auto lfo2Retrig   = "lfo2Retrig";

    // --- Mod matrix: 6 slots x {source, dest, depth} -------------------------
    // IDs are "mod<N>Source"/"mod<N>Dest"/"mod<N>Depth", N = 1..6; use slotId().
    inline juce::String slotId (const char* field, int slotIndexZeroBased)
    {
        return "mod" + juce::String (slotIndexZeroBased + 1) + field;
    }

    // --- Voice architecture --------------------------------------------------
    inline constexpr auto voiceMode = "voiceMode"; // Poly/Mono/Legato
    inline constexpr auto glideTime = "glideTime"; // 0..2000 ms

    // --- FX chain (spec §2.6: Saturation -> Chorus -> Reverb -> Out) ---------
    inline constexpr auto warmthAmount = "warmthAmount"; // 0..100 %
    inline constexpr auto chorusAmount = "chorusAmount"; // 0..100 %
    inline constexpr auto airSize      = "airSize";      // 0..100 %
    inline constexpr auto airMix       = "airMix";       // 0..100 %

    // --- Amp envelope (ENV1, hardwired to amp) ------------------------------
    inline constexpr auto ampAttack  = "ampAttack";  // ms
    inline constexpr auto ampDecay   = "ampDecay";   // ms
    inline constexpr auto ampSustain = "ampSustain"; // 0..1
    inline constexpr auto ampRelease = "ampRelease"; // ms

    // --- Output -------------------------------------------------------------
    inline constexpr auto masterGain = "masterGain"; // dB

    /** Maps the toneFrames choice index (0..3) to the actual frame count. */
    inline constexpr int frameCountForChoice (int index) noexcept
    {
        constexpr int counts[] = { 2, 4, 8, 16 };
        return counts[index < 0 ? 0 : (index > 3 ? 3 : index)];
    }

    /** Builds the full parameter layout. */
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Cached atomic pointers, so the audio thread never does a string lookup. */
    struct Handles
    {
        void attach (juce::AudioProcessorValueTreeState& state);

        std::atomic<float>* place        = nullptr;
        std::atomic<float>* captureLength = nullptr;
        std::atomic<float>* rootNote     = nullptr;
        std::atomic<float>* rootCents    = nullptr;
        std::atomic<float>* grainSize    = nullptr;
        std::atomic<float>* grainDensity = nullptr;
        std::atomic<float>* grainDrift   = nullptr;
        std::atomic<float>* grainShimmer = nullptr;
        std::atomic<float>* grainWindow  = nullptr;
        std::atomic<float>* grainSpread  = nullptr;
        std::atomic<float>* grainSync     = nullptr;
        std::atomic<float>* grainDivision = nullptr;
        std::atomic<float>* warpMode  = nullptr; // choice index into the bars table
        std::atomic<float>* grainSnap = nullptr; // 0 Off, 1 Transients
        std::atomic<float>* pitchMode = nullptr; // 0 Repitch, 1 Formant
        std::atomic<float>* focus         = nullptr;
        std::atomic<float>* toneFrame     = nullptr;
        std::atomic<float>* toneFrames    = nullptr; // choice index 0..3 -> {2,4,8,16}
        std::atomic<float>* toneFrameWrap = nullptr; // choice index: 0 Loop, 1 Ping-Pong
        std::atomic<float>* filterType      = nullptr;
        std::atomic<float>* filterCutoff    = nullptr;
        std::atomic<float>* filterResonance = nullptr;
        std::atomic<float>* filterKeytrack  = nullptr;
        std::atomic<float>* env2Attack  = nullptr;
        std::atomic<float>* env2Decay   = nullptr;
        std::atomic<float>* env2Sustain = nullptr;
        std::atomic<float>* env2Release = nullptr;
        std::atomic<float>* lfoShape[2]    = {};
        std::atomic<float>* lfoRate[2]     = {};
        std::atomic<float>* lfoSync[2]     = {};
        std::atomic<float>* lfoDivision[2] = {};
        std::atomic<float>* lfoRetrig[2]   = {};
        std::atomic<float>* modSource[6] = {};
        std::atomic<float>* modDest[6]   = {};
        std::atomic<float>* modDepth[6]  = {};
        std::atomic<float>* voiceMode = nullptr;
        std::atomic<float>* glideTime = nullptr;

        // NormalisableRanges of the modulation destinations, for the
        // normalized-domain combine. Indexed by mod::Dest; entries without an
        // underlying parameter (None, Pitch, Reverb Mix) stay null.
        const juce::NormalisableRange<float>* destRange[12] = {};
        std::atomic<float>* warmthAmount = nullptr;
        std::atomic<float>* chorusAmount = nullptr;
        std::atomic<float>* airSize      = nullptr;
        std::atomic<float>* airMix       = nullptr;
        std::atomic<float>* ampAttack    = nullptr;
        std::atomic<float>* ampDecay     = nullptr;
        std::atomic<float>* ampSustain   = nullptr;
        std::atomic<float>* ampRelease   = nullptr;
        std::atomic<float>* masterGain   = nullptr;
    };
}
