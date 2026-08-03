#include "TestHarness.h"
#include <juce_events/juce_events.h>
#include <cstring>

namespace ktest { void runBenchmark(); }

int main (int argc, char** argv)
{
    // A few JUCE subsystems (message manager, format registration) expect to be
    // initialised before use, even in a console app.
    juce::ScopedJuceInitialiser_GUI juceInit;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--bench") == 0)
        {
            ktest::runBenchmark();
            return 0;
        }
    }

    return ktest::Registry::get().runAll();
}
