#include "TestUtils.h"
#include "PluginProcessor.h"
#include <chrono>
#include <iostream>

/**
    Throughput measurement for the M2 exit criterion ("CPU < ~8% for 8 voices").

    This is deliberately NOT a test case. Wall-clock thresholds on shared CI runners
    produce flaky red builds that teach people to ignore the suite; run it by hand
    (KeepsakeTests --bench) and read the number.
*/
namespace ktest
{
    void runBenchmark()
    {
        constexpr double sampleRate = 48000.0;
        constexpr int blockSize = 256;
        constexpr double seconds = 10.0;

        std::cout << "\nKeepsake engine benchmark\n"
                  << "  sample rate " << sampleRate << " Hz, block " << blockSize << "\n"
                  << "  (CPU % is the share of one core needed to keep up in real time)\n\n";

        for (int numVoices : { 1, 4, 8, 12 })
        {
            keepsake::KeepsakeProcessor proc;
            proc.prepareToPlay (sampleRate, blockSize);

            // Give it a real keepsake to chew on.
            auto source = makeSineSource (220.0, 2.0, sampleRate, "bench");
            const auto encoded = keepsake::CaptureIO::encodeToBase64 (*source);

            auto state = proc.getState().copyState();
            state.setProperty ("audioData", encoded, nullptr);
            state.setProperty ("audioName", "bench", nullptr);
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
            proc.prepareToPlay (sampleRate, blockSize);

            juce::AudioBuffer<float> audio (2, blockSize);

            // Sustained notes, so every voice is genuinely running.
            {
                juce::MidiBuffer midi;

                for (int i = 0; i < numVoices; ++i)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 48 + i, 0.8f), 0);

                audio.clear();
                proc.processBlock (audio, midi);
            }

            const auto numBlocks = (int) (seconds * sampleRate / blockSize);
            const auto start = std::chrono::steady_clock::now();

            for (int i = 0; i < numBlocks; ++i)
            {
                juce::MidiBuffer midi;
                audio.clear();
                proc.processBlock (audio, midi);
            }

            const auto elapsed = std::chrono::duration<double> (
                                     std::chrono::steady_clock::now() - start).count();

            const auto audioSeconds = (double) numBlocks * blockSize / sampleRate;
            const auto cpuPercent = elapsed / audioSeconds * 100.0;

            std::cout << "  " << juce::String (numVoices).paddedLeft (' ', 2) << " voices: "
                      << juce::String (cpuPercent, 2) << " % of one core"
                      << "   (realtime factor " << juce::String (audioSeconds / elapsed, 1) << "x)"
                      << std::endl;
        }

        std::cout << std::endl;
    }
}
