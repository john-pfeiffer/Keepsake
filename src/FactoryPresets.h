#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

namespace keepsake::factory
{
    /**
        Built-in starting points, applied as PARAMETERS ONLY.

        A user preset is a whole keepsake - it embeds the audio, so it plays
        anywhere. A factory preset is the opposite thing on purpose: a sound
        design for whatever you already have loaded. It carries no audio and
        never republishes the source, so picking one auditions a new treatment
        of your own sample instead of replacing it.

        That is also why these live in code rather than as .keepsake files on
        disk: there is nothing to install on first run, nothing to embed, and
        no way for a factory preset to wipe someone's keepsake.
    */
    struct Preset
    {
        const char* name;

        /** Parameter ID -> PLAIN value (ms, %, cents, choice index...), not
            normalised. Anything not listed comes back at its default, the same
            contract a user preset gets from setStateInformation. */
        std::vector<std::pair<const char*, float>> values;
    };

    /** The frozen factory list, in display order. */
    const std::vector<Preset>& getPresets();
}
