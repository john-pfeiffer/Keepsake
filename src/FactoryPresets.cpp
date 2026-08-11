#include "FactoryPresets.h"
#include "Parameters.h"

namespace keepsake::factory
{
    const std::vector<Preset>& getPresets()
    {
        using namespace keepsake::params;

        // Warp and Snap both want a window long enough to hold a phrase, so
        // these reach well past the 120ms default Keep. Values are plain units.
        static const std::vector<Preset> presets {
            { "Warped Phrase", {
                { captureLength, 2000.0f },
                { warpMode, 3.0f },          // 1 Bar
                { grainSize, 80.0f },
                { grainDensity, 40.0f },
                { grainDrift, 10.0f },
                { grainWindow, 0.3f },
                { grainSpread, 60.0f },
                { ampAttack, 20.0f },
                { ampRelease, 800.0f },
                { airSize, 60.0f },
                { airMix, 25.0f } } },

            { "Sliced Regroove", {
                { captureLength, 2000.0f },
                { grainSnap, 1.0f },         // starts land on real hits
                { grainSync, 1.0f },
                { grainDivision, 7.0f },     // 1/8
                { grainSize, 60.0f },
                { grainDrift, 0.0f },        // faithful: nearest hit, no shuffle
                { grainWindow, 1.0f },       // expodec keeps the attacks sharp
                { grainSpread, 30.0f },
                { ampAttack, 1.0f },
                { ampRelease, 300.0f } } },

            { "Shuffled Slices", {
                { captureLength, 2000.0f },
                { grainSnap, 1.0f },
                { grainSync, 1.0f },
                { grainDivision, 9.0f },     // 1/16
                { grainSize, 40.0f },
                { grainDrift, 100.0f },      // full shuffle: generative regroove
                { grainWindow, 1.0f },
                { grainSpread, 70.0f },
                { ampAttack, 1.0f },
                { ampRelease, 250.0f },
                { airMix, 20.0f } } },

            { "Formant Pad", {
                { pitchMode, 1.0f },         // timbre holds across the keyboard
                { captureLength, 400.0f },
                { grainSize, 120.0f },
                { grainDrift, 8.0f },
                { grainWindow, 0.0f },
                { grainSpread, 60.0f },
                { ampAttack, 300.0f },
                { ampRelease, 1200.0f },
                { warmthAmount, 30.0f },
                { airSize, 70.0f },
                { airMix, 35.0f } } },

            // Deliberately uses none of the new modes: the classic frozen
            // texture, so the list still shows what the plugin was already for.
            { "Frozen Bloom", {
                { captureLength, 120.0f },
                { grainSize, 150.0f },
                { grainDensity, 60.0f },
                { grainDrift, 40.0f },
                { grainShimmer, 25.0f },
                { grainWindow, 0.5f },
                { grainSpread, 80.0f },
                { focus, 0.35f },
                { ampAttack, 600.0f },
                { ampRelease, 2000.0f },
                { chorusAmount, 30.0f },
                { airSize, 80.0f },
                { airMix, 45.0f } } }
        };

        return presets;
    }
}
