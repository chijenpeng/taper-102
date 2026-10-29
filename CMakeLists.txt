cmake_minimum_required(VERSION 3.22)

project(Taper102 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(COPY_PLUGIN_AFTER_BUILD "Copy AU and VST3 bundles after building the plug-in target" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Prefer a local JUCE checkout for day-to-day development (tools/link-juce.sh in plugin-dev). A fresh clone
# remains buildable: when third_party/JUCE is absent CMake fetches the exact release used by this project.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# The plug-in: the processor and editor in Source/, the header-only record -> tape -> reproduce chain in
# Source/dsp/. The headers are included by file name, so both directories are include paths.
set(TAPER102_SOURCES
    Source/PluginProcessor102.cpp
    Source/PluginProcessor102.h
    Source/PluginEditor102.cpp
    Source/PluginEditor102.h
    Source/ValueText.h)
set(TAPER102_INCLUDES Source Source/dsp)
set(TAPER102_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

juce_add_plugin(Taper102
    PRODUCT_NAME "Taper 102"
    COMPANY_NAME "codycodie"
    BUNDLE_ID "com.c0dypeng.taper102"
    PLUGIN_MANUFACTURER_CODE Cdpg
    PLUGIN_CODE T102
    FORMATS AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    AU_SANDBOX_SAFE TRUE
    VST3_CATEGORIES Fx Distortion
    COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD})

# Third-party licence texts shipped inside the AU and VST3 bundles.
if(TARGET Taper102_AU) # AU exists only on macOS
  juce_add_bundle_resources_directory(Taper102_AU Resources/Notices)
endif()
juce_add_bundle_resources_directory(Taper102_VST3 Resources/Notices)

# Inter and JetBrains Mono, both SIL Open Font License (licences are next to the fonts).
juce_add_binary_data(Taper102Assets SOURCES
    Resources/fonts/Inter-SemiBold.ttf
    Resources/fonts/JetBrainsMono-Medium.ttf)

target_sources(Taper102 PRIVATE ${TAPER102_SOURCES})
target_include_directories(Taper102 PRIVATE ${TAPER102_INCLUDES})
target_compile_definitions(Taper102 PUBLIC ${TAPER102_DEFS})
target_link_libraries(Taper102
    PRIVATE
        Taper102Assets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
if(MSVC)
    target_compile_options(Taper102 PRIVATE /W4)
else()
    target_compile_options(Taper102 PRIVATE -Wall -Wextra)
endif()

# Processor regression: parameters, state (including schema 1 migration), Link, 0 VU, bypass, latency,
# setups, stereo, sample rates, control changes and audio-thread allocations.
juce_add_console_app(taper102_processor_tests)
target_sources(taper102_processor_tests PRIVATE tests/Processor102Tests.cpp ${TAPER102_SOURCES})
target_include_directories(taper102_processor_tests PRIVATE ${TAPER102_INCLUDES})
target_compile_definitions(taper102_processor_tests PRIVATE ${TAPER102_DEFS})
target_link_libraries(taper102_processor_tests
    PRIVATE
        Taper102Assets
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Renders the editor to a PNG (docs/editor.png) and can show any control's hover help.
# Run it from the repository root.
juce_add_console_app(taper102_snapshot)
target_sources(taper102_snapshot PRIVATE tests/snapshot102.cpp ${TAPER102_SOURCES})
target_include_directories(taper102_snapshot PRIVATE ${TAPER102_INCLUDES})
target_compile_definitions(taper102_snapshot PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1 ${TAPER102_DEFS})
target_link_libraries(taper102_snapshot
    PRIVATE
        Taper102Assets
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

enable_testing()
add_test(NAME taper102_processor_tests COMMAND taper102_processor_tests)
