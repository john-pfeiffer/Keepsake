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

    // --- Tone (wavetable) engine --------------------------------------------
    inline constexpr auto focus         = "focus";         // 0..1 Cloud->Tone
    inline constexpr auto toneFrame     = "toneFrame";     // 0..1 scan across frames
    inline constexpr auto toneFrames    = "toneFrames";    // choice: 2/4/8/16
    inline constexpr auto toneFrameWrap = "toneFrameWrap"; // choice: Loop/Ping-Pong

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
        std::atomic<float>* focus         = nullptr;
        std::atomic<float>* toneFrame     = nullptr;
        std::atomic<float>* toneFrames    = nullptr; // choice index 0..3 -> {2,4,8,16}
        std::atomic<float>* toneFrameWrap = nullptr; // choice index: 0 Loop, 1 Ping-Pong
        std::atomic<float>* ampAttack    = nullptr;
        std::atomic<float>* ampDecay     = nullptr;
        std::atomic<float>* ampSustain   = nullptr;
        std::atomic<float>* ampRelease   = nullptr;
        std::atomic<float>* masterGain   = nullptr;
    };
}
