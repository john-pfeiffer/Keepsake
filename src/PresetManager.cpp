#include "PresetManager.h"
#include "FactoryPresets.h"
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

        // Factory presets lead the list in their authored order - they are a
        // curated sequence, not an alphabetical one.
        for (const auto& preset : factory::getPresets())
            names.add (preset.name);

        numFactory = names.size();

        juce::StringArray userNames;

        for (const auto& file : directory.findChildFiles (juce::File::findFiles, false,
                                                          juce::String ("*") + kExtension))
            userNames.add (file.getFileNameWithoutExtension());

        userNames.sortNatural();
        names.addArray (userNames);

        // Keep pointing at the same preset if it survived the rescan.
        currentIndex = names.indexOf (currentName);
    }

    bool PresetManager::applyFactoryPreset (int factoryIndex)
    {
        const auto& presets = factory::getPresets();

        if (! juce::isPositiveAndBelow (factoryIndex, (int) presets.size()))
            return false;

        const auto& preset = presets[(size_t) factoryIndex];

        // Same contract a user preset gets: everything the preset does not
        // name returns to its default, so nothing bleeds across a switch.
        for (auto* p : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
                ranged->setValueNotifyingHost (ranged->getDefaultValue());

        auto& state = processor.getState();

        for (const auto& [id, plainValue] : preset.values)
            if (auto* p = state.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (plainValue));

        // Note what is NOT here: no setStateInformation, no publishSource. The
        // loaded keepsake survives, which is the whole point of these.
        processor.setPresetDisplayName (preset.name);
        return true;
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

        if (index < numFactory)
        {
            if (! applyFactoryPreset (index))
                return false;

            currentIndex = index;
            return true;
        }

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
