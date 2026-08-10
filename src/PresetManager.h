#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace keepsake
{
    class KeepsakeProcessor;

    /**
        The preset browser's model (spec §3: "simple bar at top - prev/next,
        save, name. Presets embed capture audio.").

        A preset file is EXACTLY the processor's state blob - the same
        magic/version/length-guarded binary that hosts store, embedded audio
        and all. That buys three things for free: the keepsake travels with
        the preset, the validation path is shared (a corrupt preset is
        rejected the same way a corrupt session is), and older presets load
        with missing parameters at their defaults (setStateInformation merges
        defaults for absent parameters before applying - deliberately there,
        not here, so host sessions get the same treatment).

        Message thread only. The directory is injectable so tests never touch
        the user's real preset folder.
    */
    class PresetManager
    {
    public:
        static constexpr auto kExtension = ".keepsake";

        /** ~/Documents/Keepsake/Presets (created lazily on first save). */
        static juce::File defaultDirectory();

        PresetManager (KeepsakeProcessor& processorToManage, juce::File presetDirectory);

        /** Re-reads the directory. Called automatically after every save. */
        void rescan();

        const juce::StringArray& getPresetNames() const noexcept { return names; }
        int getCurrentIndex() const noexcept { return currentIndex; }
        juce::String getCurrentName() const;

        /** Saves the current state under the given name (sanitized to a legal
            file name), overwriting any existing preset of that name. */
        bool savePreset (const juce::String& name);

        bool loadPreset (int index);
        bool loadPreset (const juce::String& name) { return loadPreset (names.indexOf (name)); }

        /** Wrap-around navigation; false only when there are no presets. */
        bool loadNext() { return stepBy (1); }
        bool loadPrevious() { return stepBy (-1); }

    private:
        bool stepBy (int delta);

        KeepsakeProcessor& processor;
        juce::File directory;
        juce::StringArray names; // sorted, extension stripped
        int currentIndex = -1;
    };
}
