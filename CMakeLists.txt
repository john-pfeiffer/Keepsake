cmake_minimum_required(VERSION 3.22)

project(Keepsake VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# JUCE 8 removed MinGW support. Without an MSVC environment, `cmake -G Ninja` on
# Windows silently selects MinGW gcc and the build fails deep inside juceaide with
# hundreds of warnings and no obvious cause. Fail here instead, with the fix.
if (WIN32 AND NOT MSVC)
    message(FATAL_ERROR
            "Keepsake requires MSVC on Windows (JUCE 8 removed MinGW support), but "
            "CMake selected '${CMAKE_CXX_COMPILER_ID}'.\n"
            "Run from a Developer Command Prompt, or use the Visual Studio generator:\n"
            "  cmake -B build -G \"Visual Studio 17 2022\" -A x64")
endif ()

if (APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
endif ()

option(KEEPSAKE_BUILD_TESTS "Build the Keepsake test suite" ON)

# Source files are UTF-8 (spec references in comments use section signs). Tell MSVC
# explicitly, otherwise it guesses the local code page and warns. String *literals*
# are kept ASCII-only on purpose — juce::String(const char*) cannot infer an encoding
# and asserts on high bytes.
if (MSVC)
    add_compile_options(/utf-8)
endif ()

# --- JUCE -------------------------------------------------------------------
# Prefer a checkout in libs/JUCE (fast local iteration); otherwise fetch it.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/JUCE/CMakeLists.txt")
    message(STATUS "Keepsake: using local JUCE at libs/JUCE")
    add_subdirectory(libs/JUCE)
else ()
    include(FetchContent)
    FetchContent_Declare(
            JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.4
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif ()

# --- Plugin -----------------------------------------------------------------
# Identity is FROZEN as of 1.0.0: the AU manufacturer/plugin code pair and the
# bundle id are what saved DAW projects reference - changing any of them after
# release orphans every project that uses the plugin.
juce_add_plugin(Keepsake
        COMPANY_NAME "Elan Vital Studios"
        BUNDLE_ID "com.elanvitalstudios.keepsake"
        PLUGIN_MANUFACTURER_CODE EVSR
        PLUGIN_CODE Kpsk
        FORMATS VST3 AU Standalone
        PRODUCT_NAME "Keepsake"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_CATEGORIES Instrument Synth
        AU_MAIN_TYPE kAudioUnitType_MusicDevice)

juce_generate_juce_header(Keepsake)

target_sources(Keepsake PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
        src/Parameters.cpp
        src/PresetManager.cpp
        src/engine/Analysis.cpp
        src/engine/CaptureBuffer.cpp
        src/engine/GrainEngine.cpp
        src/engine/ToneEngine.cpp
        src/engine/Voice.cpp
        src/fx/FxChain.cpp
        src/ui/WaveformView.cpp
        src/ui/ControlPanels.cpp)

# PUBLIC so the test target inherits the include path when it links SharedCode.
target_include_directories(Keepsake PUBLIC src)

target_compile_definitions(Keepsake PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(Keepsake
        PUBLIC
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# --- Tests ------------------------------------------------------------------
if (KEEPSAKE_BUILD_TESTS)
    enable_testing()

    # A plain executable rather than juce_add_console_app: the tests link the plugin's
    # shared code, which already carries every JUCE module and the JucePlugin_*
    # definitions. juce_add_console_app would additionally force
    # JUCE_STANDALONE_APPLICATION=1 on top of the shared code's own definition of the
    # same macro, which is a redefinition warning on every translation unit.
    # TestMain initialises JUCE itself via ScopedJuceInitialiser_GUI.
    add_executable(KeepsakeTests)

    target_sources(KeepsakeTests PRIVATE
            tests/TestMain.cpp
            tests/TestHarness.cpp
            tests/AnalysisTests.cpp
            tests/CaptureBufferTests.cpp
            tests/GrainEngineTests.cpp
            tests/ToneEngineTests.cpp
            tests/RenderTests.cpp
            tests/Benchmark.cpp)

    target_include_directories(KeepsakeTests PRIVATE src tests)

    # juce_add_console_app already sets JUCE_STANDALONE_APPLICATION; setting it again
    # here only produces a redefinition warning.
    target_compile_definitions(KeepsakeTests PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)

    # Link the plugin's shared code so the tests exercise the real processor.
    target_link_libraries(KeepsakeTests PRIVATE
            Keepsake
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    add_test(NAME KeepsakeTests COMMAND KeepsakeTests)
endif ()
