#include "PresetManager.h"
#include "PluginProcessor.h"

namespace keepsake
{
    juce::File PresetManager::defaultDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("Keepsake")
                   .getChildFile ("Presets");
    }

    PresetManager::PresetManager (KeepsakeProcessor& processorToManage,
                                  juce::File presetDirectory)
        : processor (processorToManage), directory (std::move (presetDirectory))
    {
        rescan();
    }

    void PresetManager::rescan()
    {
        const auto currentName = getCurrentName();

        names.clear();

        for (const auto& file : directory.findChildFiles (juce::File::findFiles, false,
                                                          juce::String ("*") + kExtension))
            names.add (file.getFileNameWithoutExtension());

        names.sortNatural();

        // Keep pointing at the same preset if it survived the rescan.
        currentIndex = names.indexOf (currentName);
    }

    juce::String PresetManager::getCurrentName() const
    {
        return names[currentIndex]; // StringArray returns empty out of range
    }

    bool PresetManager::savePreset (const juce::String& name)
    {
        const auto legal = juce::File::createLegalFileName (name.trim());

        if (legal.isEmpty())
            return false;

        if (! directory.createDirectory())
            return false;

        // The display name travels inside the blob, so a host session reload
        // (or loading this preset later) restores the bar's label too.
        processor.setPresetDisplayName (legal);

        juce::MemoryBlock state;
        processor.getStateInformation (state);

        const auto file = directory.getChildFile (legal + kExtension);

        if (! file.replaceWithData (state.getData(), state.getSize()))
            return false;

        rescan();
        currentIndex = names.indexOf (legal);
        return true;
    }

    bool PresetManager::loadPreset (int index)
    {
        if (! juce::isPositiveAndBelow (index, names.size()))
            return false;

        juce::MemoryBlock state;
        const auto file = directory.getChildFile (names[index] + kExtension);

        if (! file.loadFileAsData (state))
            return false;

        // setStateInformation validates the header and rejects garbage; a
        // failed load simply leaves the current state alone.
        processor.setStateInformation (state.getData(), (int) state.getSize());
        currentIndex = index;
        return true;
    }

    bool PresetManager::stepBy (int delta)
    {
        if (names.isEmpty())
            return false;

        const auto count = names.size();
        const auto from = currentIndex < 0 ? (delta > 0 ? -1 : 0) : currentIndex;

        return loadPreset (((from + delta) % count + count) % count);
    }
}
