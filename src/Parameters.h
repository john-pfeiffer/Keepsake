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
    inline constexpr auto captureLength = "captureLength"; // 10..500 ms

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

    // --- Amp envelope (ENV1, hardwired to amp) ------------------------------
    inline constexpr auto ampAttack  = "ampAttack";  // ms
    inline constexpr auto ampDecay   = "ampDecay";   // ms
    inline constexpr auto ampSustain = "ampSustain"; // 0..1
    inline constexpr auto ampRelease = "ampRelease"; // ms

    // --- Output -------------------------------------------------------------
    inline constexpr auto masterGain = "masterGain"; // dB

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
        std::atomic<float>* ampAttack    = nullptr;
        std::atomic<float>* ampDecay     = nullptr;
        std::atomic<float>* ampSustain   = nullptr;
        std::atomic<float>* ampRelease   = nullptr;
        std::atomic<float>* masterGain   = nullptr;
    };
}
