#include "TestHarness.h"
#include <iostream>

namespace ktest
{
    Registry& Registry::get()
    {
        static Registry instance;
        return instance;
    }

    void Registry::add (juce::String name, std::function<void()> body)
    {
        tests.push_back ({ std::move (name), std::move (body) });
    }

    void Registry::recordFailure (juce::String message, juce::String location)
    {
        ++failuresInCurrentTest;
        failures.push_back ({ currentTest, std::move (message), std::move (location) });
    }

    int Registry::runAll()
    {
        int passed = 0;

        for (auto& test : tests)
        {
            currentTest = test.name;
            failuresInCurrentTest = 0;

            std::cout << "[ RUN      ] " << test.name << std::endl;

            try
            {
                test.body();
            }
            catch (const std::exception& e)
            {
                recordFailure (juce::String ("threw std::exception: ") + e.what(), "");
            }
            catch (...)
            {
                recordFailure ("threw an unknown exception", "");
            }

            if (failuresInCurrentTest == 0)
            {
                ++passed;
                std::cout << "[       OK ] " << test.name << std::endl;
            }
            else
            {
                std::cout << "[  FAILED  ] " << test.name
                          << " (" << failuresInCurrentTest << " assertion(s))" << std::endl;
            }
        }

        std::cout << "\n" << passed << " / " << tests.size() << " tests passed." << std::endl;

        if (! failures.empty())
        {
            std::cout << "\nFailures:" << std::endl;

            for (const auto& f : failures)
                std::cout << "  " << f.test << ": " << f.message
                          << (f.location.isEmpty() ? juce::String() : "  (" + f.location + ")")
                          << std::endl;
        }

        return failures.empty() ? 0 : 1;
    }

    void expectTrue (bool condition, const juce::String& message, const juce::String& location)
    {
        if (! condition)
            Registry::get().recordFailure ("expected " + message, location);
    }

    void expectNear (double actual, double expected, double tolerance,
                     const juce::String& message, const juce::String& location)
    {
        if (! (std::abs (actual - expected) <= tolerance))
            Registry::get().recordFailure (
                message + " - got " + juce::String (actual, 6)
                    + ", expected " + juce::String (expected, 6)
                    + " (tolerance " + juce::String (tolerance, 6) + ")",
                location);
    }
}
