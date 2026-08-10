#include "TestUtils.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <iostream>

/**
    Dev-only UI snapshot mode (KeepsakeTests --ui-snapshot <dir>).

    Renders the real editor headlessly (run under xvfb on Linux) and writes a
    PNG per tab at the design size, plus one at the minimum window size. The
    editor cannot be eyeballed from a headless container any other way - the
    rc.1 layout shipped with knobs collapsed to slivers precisely because
    nothing ever LOOKED at it. Not a product feature.
*/
namespace ktest
{
    namespace
    {
        juce::TabbedComponent* findTabs (juce::Component& root)
        {
            if (auto* tabs = dynamic_cast<juce::TabbedComponent*> (&root))
                return tabs;

            for (auto* child : root.getChildren())
                if (auto* found = findTabs (*child))
                    return found;

            return nullptr;
        }

        void writePng (juce::Component& component, const juce::File& file)
        {
            const auto image = component.createComponentSnapshot (component.getLocalBounds());

            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat().writeImageToStream (image, stream);
            std::cout << "  wrote " << file.getFullPathName() << "\n";
        }

        /** A source with visible structure, so the waveform view shows a real
            shape rather than a solid sine block. */
        void loadSnapshotSource (keepsake::KeepsakeProcessor& proc, double sampleRate)
        {
            auto source = makeDrumSource (4.0, sampleRate, 42);
            const auto encoded = keepsake::CaptureIO::encodeToBase64 (*source);

            auto state = proc.getState().copyState();
            state.setProperty ("audioData", encoded, nullptr);
            state.setProperty ("audioName", "snapshot-source", nullptr);
            state.setProperty ("audioRate", sampleRate, nullptr);

            juce::MemoryBlock block;
            {
                juce::MemoryOutputStream stream (block, false);
                stream.writeInt ((int) keepsake::KeepsakeProcessor::kStateMagic);
                stream.writeInt (keepsake::KeepsakeProcessor::kStateVersion);

                juce::MemoryBlock payload;
                {
                    juce::MemoryOutputStream payloadStream (payload, false);
                    state.writeToStream (payloadStream);
                }

                stream.writeInt ((int) payload.getSize());
                stream.write (payload.getData(), payload.getSize());
            }

            proc.setStateInformation (block.getData(), (int) block.getSize());
        }
    }

    int runUiSnapshot (const juce::String& outputDir)
    {
        const juce::File dir (juce::File::getCurrentWorkingDirectory().getChildFile (outputDir));
        dir.createDirectory();

        keepsake::KeepsakeProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        loadSnapshotSource (proc, 48000.0);
        proc.extractNow();

        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());

        if (editor == nullptr)
        {
            std::cerr << "no editor\n";
            return 1;
        }

        editor->setVisible (true); // no desktop peer: createComponentSnapshot paints directly

        auto snapshotAllTabs = [&] (int width, int height, const juce::String& prefix)
        {
            editor->setSize (width, height);

            auto* tabs = findTabs (*editor);

            const auto tabCount = tabs != nullptr ? tabs->getNumTabs() : 1;

            for (int i = 0; i < tabCount; ++i)
            {
                if (tabs != nullptr)
                    tabs->setCurrentTabIndex (i, false);

                const auto tabName = tabs != nullptr ? tabs->getTabNames()[i] : juce::String ("only");
                writePng (*editor, dir.getChildFile (prefix + "-" + juce::String (i)
                                                     + "-" + tabName + ".png"));
            }
        };

        std::cout << "UI snapshots -> " << dir.getFullPathName() << "\n";
        snapshotAllTabs (keepsake::KeepsakeEditor::kDesignWidth,
                         keepsake::KeepsakeEditor::kDesignHeight, "default");

        // The constrainer's minimum: everything must stay legible here too.
        snapshotAllTabs (keepsake::KeepsakeEditor::kDesignWidth * 2 / 3,
                         keepsake::KeepsakeEditor::kDesignHeight * 2 / 3, "min");

        return 0;
    }
}
