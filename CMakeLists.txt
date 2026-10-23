cmake_minimum_required(VERSION 3.22)
project(Cimra VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

# JUCE is downloaded at configure time. To use an existing checkout instead:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

set(CIMRA_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND CIMRA_FORMATS AU)
endif()

juce_add_plugin(Cimra
    COMPANY_NAME "mandev"
    PLUGIN_MANUFACTURER_CODE Mndv
    PLUGIN_CODE Cmra
    PRODUCT_NAME "Cimra"
    FORMATS ${CIMRA_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Reverb Spatial
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(Cimra PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Cimra PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Cimra
    PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# DSP tests: plain C++, no JUCE needed.
enable_testing()
add_executable(cimra_dsp_test tests/dsp_test.cpp)
target_include_directories(cimra_dsp_test PRIVATE Source)
add_test(NAME dsp COMMAND cimra_dsp_test)
