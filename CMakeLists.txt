cmake_minimum_required(VERSION 3.22)

# ----------------------------------------------------------------------------
# OrbitCab — IR Cabinet Loader plugin (C++/JUCE). Felitronics line, by Darwin's Cat.
#
# One CMake config builds VST3 + AU on macOS and Windows. JUCE 8 is fetched by
# CMake (FetchContent, pinned tag) — NOT vendored, NOT system-installed.
# See docs/BUILD.md and docs/ARCHITECTURE.md.
# ----------------------------------------------------------------------------

# Version = numeric MAJOR.MINOR.PATCH, no prerelease labels. Keep in sync with the
# release git tag vX.Y.Z. This feeds JucePlugin_VersionString (what the update check reports).
project(OrbitCab VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Universal binary on Apple Silicon + Intel. Set at configure time only (it has
# no effect after the cache is written). Skipped if the caller already chose.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS build archs" FORCE)
endif()

# ----------------------------------------------------------------------------
# JUCE — fetched, pinned. Bump ORBITCAB_JUCE_TAG deliberately (it's the only knob).
# ----------------------------------------------------------------------------
include(FetchContent)

set(ORBITCAB_JUCE_TAG "8.0.13" CACHE STRING "JUCE git tag to build against")

FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${ORBITCAB_JUCE_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(juce)

# ----------------------------------------------------------------------------
# The plugin target.
#
# 4-char codes (LOCKED post-release):
#   PLUGIN_MANUFACTURER_CODE = Dcat  (Darwin's Cat — ≥1 uppercase, AU rule)
#   PLUGIN_CODE              = Orbt  (OrbitCab, unique per plugin)
# Changing these post-release makes hosts treat it as a different plugin.
# ----------------------------------------------------------------------------
juce_add_plugin(OrbitCab
    PRODUCT_NAME                "OrbitCab"
    COMPANY_NAME                "Darwin's Cat"
    BUNDLE_ID                   "com.darwinscat.orbitcab"
    PLUGIN_MANUFACTURER_CODE    Dcat
    PLUGIN_CODE                 Orbt
    FORMATS                     VST3 AU Standalone
    VST3_CATEGORIES             "Fx" "Distortion"
    AU_MAIN_TYPE                "kAudioUnitType_Effect"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     TRUE   # auto-install to the user plug-in folders during dev
)

target_sources(OrbitCab PRIVATE
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/UpdateChecker.cpp
    src/PresetManager.cpp
    src/PluginEditor.cpp
    src/ui/SlotComponent.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp        # headless DSP core (cab::) — D24
)

# ----------------------------------------------------------------------------
# Bundled IRs — embedded into the binary as a C++ byte array (no disk reads).
# juce_add_binary_data generates an `OrbitCabData` target exposing `BinaryData::*`.
# The full CC0 Brutal (01-15) + Emerald (16-21) packs — 48 kHz/24-bit
# variants (the convolver resamples to host rate anyway, so one rate is enough).
# All bundled IRs are CC0 — see ASSET-LICENSES.md / THIRD_PARTY_NOTICES.
# The editor enumerates these at runtime via BinaryData::namedResourceList — adding
# a row here is all it takes to surface a new IR in the browser (no code change).
# ----------------------------------------------------------------------------
juce_add_binary_data(OrbitCabData
    SOURCES
        # Brutal pack (mod. Behringer BG412S — V30 / Rockdriver Jr / DV-77)
        resources/ir/01-cookie-monster.wav
        resources/ir/02-darth-genocider.wav
        resources/ir/03-kitten-slayer.wav
        resources/ir/04-kaiju-tamer.wav
        resources/ir/05-iceburn-suicide.wav
        resources/ir/06-vertical-lip-stabber.wav
        resources/ir/07-manslaughter-joe.wav
        resources/ir/08-big-bubba.wav
        resources/ir/09-devils-cunnilingus.wav
        resources/ir/10-october-32th.wav
        resources/ir/11-wumbo.wav
        resources/ir/12-world-collider.wav
        resources/ir/13-cannibal-choir.wav
        resources/ir/14-cathode-ray-fleshburn.wav
        resources/ir/15-impaler-jim.wav
        # Emerald pack (Marshall 1960AX — Greenback G12M 25W)
        resources/ir/16-nacho-guacamole.wav
        resources/ir/17-pickle-punisher.wav
        resources/ir/18-wasabi-warrior.wav
        resources/ir/19-pesto-paladin.wav
        resources/ir/20-don-spinacio.wav
        resources/ir/21-kill-dill.wav
        # Brand — the Darwin's Cat logo (SVG), drawn in the header
        resources/brand/logo-darwinscat.svg
        # Wordmark font — Michroma (OFL), embedded for the "rbitCab" header mark
        resources/fonts/Michroma-Regular.ttf
)

target_compile_definitions(OrbitCab PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0   # JUCE 8 has no splash requirement on any tier, incl. AGPLv3
)

target_link_libraries(OrbitCab PRIVATE
    OrbitCabData                # the embedded IR(s) — BinaryData::* symbols
    juce::juce_audio_utils      # pulls in audio_processors / audio_basics / gui_basics ...
    juce::juce_dsp              # Convolution / ProcessorChain / Oversampling later
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# Offline DSP test harness (console) — renders through the real processor to verify
# DSP behaviour (e.g. TRIM actually shortens the IR). Not part of the plugin build.
# ----------------------------------------------------------------------------
juce_add_console_app(orbitcab_dsp_test PRODUCT_NAME "OrbitCabDspTest")
target_sources(orbitcab_dsp_test PRIVATE
    tools/dsp-test/test_trim.cpp
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/UpdateChecker.cpp
    src/PresetManager.cpp
    src/PluginEditor.cpp
    src/ui/SlotComponent.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp
)
target_compile_definitions(orbitcab_dsp_test PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="OrbitCab"
    JucePlugin_VersionString="1.0.0"   # plugin build gets this from juce_add_plugin; console app needs it for UpdateChecker
)
target_link_libraries(orbitcab_dsp_test PRIVATE
    OrbitCabData
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# Headless unit tests for the cab:: DSP core (JUCE UnitTest). Links core/ ONLY —
# no GUI, no juce_audio_processors, no host — which is the whole point of the
# core/adapter split. Run it as a CI gate (returns non-zero on any failure).
# ----------------------------------------------------------------------------
juce_add_console_app(OrbitCab_Tests PRODUCT_NAME "OrbitCabTests")
target_sources(OrbitCab_Tests PRIVATE
    tests/TestMain.cpp
    tests/CabEngineTests.cpp
    tests/IRSlotTests.cpp
    tests/AutoLevelerTests.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp
)
target_include_directories(OrbitCab_Tests PRIVATE src)   # so tests/ can #include "core/..."
target_compile_definitions(OrbitCab_Tests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
)
target_link_libraries(OrbitCab_Tests PRIVATE
    juce::juce_dsp                # pulls juce_audio_basics / juce_audio_formats / juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
