#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

/**
    A deliberately tiny test harness.

    Keepsake's tests are mostly "render this and assert something about the samples",
    which needs assertions and a runner and nothing else. Pulling in a framework would
    add a dependency and CI time for no gain; if the suite ever outgrows this, swapping
    in Catch2 is a contained change.
*/
namespace ktest
{
    struct Failure
    {
        juce::String test;
        juce::String message;
        juce::String location;
    };

    class Registry
    {
    public:
        static Registry& get();

        void add (juce::String name, std::function<void()> body);
        int runAll();

        void recordFailure (juce::String message, juce::String location);
        const juce::String& getCurrentTest() const noexcept { return currentTest; }

    private:
        struct Test
        {
            juce::String name;
            std::function<void()> body;
        };

        std::vector<Test> tests;
        std::vector<Failure> failures;
        juce::String currentTest;
        int failuresInCurrentTest = 0;
    };

    struct Registrar
    {
        Registrar (const char* name, std::function<void()> body)
        {
            Registry::get().add (name, std::move (body));
        }
    };

    void expectTrue (bool condition, const juce::String& message, const juce::String& location);
    void expectNear (double actual, double expected, double tolerance,
                     const juce::String& message, const juce::String& location);
}

#define KTEST_STRINGIFY2(x) #x
#define KTEST_STRINGIFY(x) KTEST_STRINGIFY2(x)
#define KTEST_LOCATION (juce::String (__FILE__) + ":" + KTEST_STRINGIFY (__LINE__))

#define KTEST_CASE(name)                                                        \
    static void name();                                                         \
    static ::ktest::Registrar registrar_##name (#name, [] { name(); });          \
    static void name()

#define EXPECT_TRUE(cond) ::ktest::expectTrue ((cond), #cond, KTEST_LOCATION)
#define EXPECT_FALSE(cond) ::ktest::expectTrue (! (cond), "!(" #cond ")", KTEST_LOCATION)
#define EXPECT_NEAR(a, b, tol) ::ktest::expectNear ((a), (b), (tol), #a " ~= " #b, KTEST_LOCATION)
#define EXPECT_MSG(cond, msg) ::ktest::expectTrue ((cond), (msg), KTEST_LOCATION)
