cmake_minimum_required(VERSION 3.22)

project(openbfd VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# JUCE checkout; pass -DJUCE_DIR=... to use one elsewhere.
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/JUCE" CACHE PATH "Path to JUCE")

# --- core: codec + model + engine (no JUCE) -------------------------------
add_library(openbfd_core STATIC
    Source/Bfdlac/BfdlacCodec.cpp
    Source/Model/Model.cpp
    Source/Engine/Engine.cpp
    Source/Engine/Mixer.cpp)
target_include_directories(openbfd_core PUBLIC Source)
if(MSVC)
    target_compile_options(openbfd_core PRIVATE /W4)
else()
    target_compile_options(openbfd_core PRIVATE -Wall -Wextra)
endif()

# --- CLI decoder ----------------------------------------------------------
add_executable(openbfd-decode tools/decode_cli.cpp)
target_link_libraries(openbfd-decode PRIVATE openbfd_core)

# --- tests ---------------------------------------------------------------
add_executable(openbfd-tests Tests/test_main.cpp)
target_link_libraries(openbfd-tests PRIVATE openbfd_core)
enable_testing()
add_test(NAME openbfd-tests COMMAND openbfd-tests)

# --- VST3 plugin ---------------------------------------------------------
add_subdirectory("${JUCE_DIR}" JUCE)

# UI sources + embedded JetBrains Mono, shared by the plugin and the tools.
set(OPENBFD_UI_SOURCES
    Source/Plugin/UI/Theme.cpp
    Source/Plugin/UI/Motion.cpp
    Source/Plugin/UI/Widgets.cpp
    Source/Plugin/UI/KitArt.cpp
    Source/Plugin/UI/Browser.cpp
    Source/Plugin/UI/KitStage.cpp
    Source/Plugin/UI/MixerPanel.cpp
    Source/Plugin/UI/CableLayer.cpp)
juce_add_binary_data(OpenBfdFonts
    HEADER_NAME OpenBfdFonts.h
    NAMESPACE OpenBfdFonts
    SOURCES
        Resources/Fonts/JetBrainsMono-Regular.ttf
        Resources/Fonts/JetBrainsMono-Medium.ttf)

juce_add_plugin(OpenBfd
    COMPANY_NAME "openbfd"
    BUNDLE_ID "local.openbfd.plugin"
    PLUGIN_MANUFACTURER_CODE Opbf
    PLUGIN_CODE Obf1
    PRODUCT_NAME "openbfd"
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES "Instrument" "Drums"
    COPY_PLUGIN_AFTER_BUILD TRUE)

juce_generate_juce_header(OpenBfd)

target_sources(OpenBfd PRIVATE
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginEditor.cpp
    ${OPENBFD_UI_SOURCES})

target_include_directories(OpenBfd PRIVATE Source)

target_compile_definitions(OpenBfd PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(OpenBfd
    PRIVATE
        openbfd_core
        OpenBfdFonts
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# --- editor snapshot tool (UI validation without a DAW) --------------------
juce_add_console_app(openbfd-ui-shot PRODUCT_NAME "openbfd-ui-shot")
juce_generate_juce_header(openbfd-ui-shot)
target_sources(openbfd-ui-shot PRIVATE
    tools/ui_shot.cpp
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginEditor.cpp
    ${OPENBFD_UI_SOURCES})
target_include_directories(openbfd-ui-shot PRIVATE Source)
target_compile_definitions(openbfd-ui-shot PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(openbfd-ui-shot PRIVATE
    openbfd_core
    OpenBfdFonts
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_graphics)

# --- end-to-end render through the real processor (host-like) --------------
juce_add_console_app(openbfd-render PRODUCT_NAME "openbfd-render")
juce_generate_juce_header(openbfd-render)
target_sources(openbfd-render PRIVATE
    tools/render_cli.cpp
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginEditor.cpp
    ${OPENBFD_UI_SOURCES})
target_include_directories(openbfd-render PRIVATE Source)
target_compile_definitions(openbfd-render PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(openbfd-render PRIVATE
    openbfd_core
    OpenBfdFonts
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_graphics)
