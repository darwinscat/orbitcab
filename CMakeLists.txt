cmake_minimum_required(VERSION 3.22)

# ----------------------------------------------------------------------------
# OrbitCab — IR Cabinet Loader plugin (C++/JUCE). Felitronics line, by Darwin's Cat.
#
# One CMake config builds VST3 + AU + CLAP on macOS and Windows. JUCE 8 is fetched by
# CMake (FetchContent, pinned tag) — NOT vendored, NOT system-installed.
# See docs/BUILD.md and docs/ARCHITECTURE.md.
# ----------------------------------------------------------------------------

# Version = numeric MAJOR.MINOR.PATCH, no prerelease labels. Keep in sync with the
# release git tag vX.Y.Z. This feeds JucePlugin_VersionString (what the update check reports).
project(OrbitCab VERSION 2.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ----------------------------------------------------------------------------
# Generated headers dir. The build-version stamp (OrbitCabVersion.h) lands here — its generator is
# wired below, AFTER felitronics-core is resolved, so the stamp can also name the core version.
# ----------------------------------------------------------------------------
set(ORBITCAB_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${ORBITCAB_GENERATED_DIR}")
include_directories(${ORBITCAB_GENERATED_DIR})

if(MSVC)
    # 8 MB stack (the Unix default). The dsp-test console apps put the whole OrbitCabAudioProcessor on the
    # stack; MSVC's 1 MB default overflows on construction → an instant, output-less crash. Match Unix.
    add_link_options(/STACK:8388608)
endif()

# Universal binary on Apple Silicon + Intel. Set at configure time only (it has
# no effect after the cache is written). Skipped if the caller already chose.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS build archs" FORCE)
endif()

# ----------------------------------------------------------------------------
# JUCE — fetched, pinned. Bump ORBITCAB_JUCE_TAG deliberately (it's the only knob).
# ----------------------------------------------------------------------------
include(FetchContent)

set(ORBITCAB_JUCE_TAG "8.0.14" CACHE STRING "JUCE git tag to build against")

FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${ORBITCAB_JUCE_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(juce)

# CLAP format via the MIT clap-juce-extensions wrapper (native CLAP is slated for JUCE 9).
# Pinned like JUCE — bump ORBITCAB_CLAP_EXT_TAG deliberately. Upstream stopped tagging in
# 2022 (last tag 0.26.0 predates CLAP 1.0 / JUCE 6), so main is their de-facto release
# channel; we pin a specific main commit (51a9359, 2026-06-01) for reproducible builds
# instead of tracking the moving branch.
set(ORBITCAB_CLAP_EXT_TAG "51a9359315298de632cf44e9d7524940868441e6" CACHE STRING "clap-juce-extensions git commit to build against")
FetchContent_Declare(
    clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        ${ORBITCAB_CLAP_EXT_TAG}
)
FetchContent_MakeAvailable(clap-juce-extensions)

# ----------------------------------------------------------------------------
# NeuralAmpModelerCore (NAM) — neural amp inference for the two stages in front of the cab
# (input → PREAMP → POWERAMP → cab). Pinned to a commit like
# JUCE/CLAP. Only the `eigen` submodule is needed — AudioDSPTools is plugin-only and never
# referenced by the core sources; nlohmann/json is vendored in the repo. The repo ships no
# usable library target, so we compile NAM/*.cpp into our own static lib `nam_core`.
# NAM_SAMPLE_FLOAT makes NAM run in float (matches JUCE's buffers — no double conversion).
# Licences are all AGPL-compatible: NAM = MIT, Eigen = MPL-2.0, nlohmann/json = MIT
# (recorded in THIRD_PARTY_NOTICES.md).
# ----------------------------------------------------------------------------
set(ORBITCAB_NAM_TAG "b5a68c3ebed5035a91d9207219346c81e8e3ce8e" CACHE STRING "NeuralAmpModelerCore commit to build against")
FetchContent_Declare(
    nam_core_src
    GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
    GIT_TAG        ${ORBITCAB_NAM_TAG}
    GIT_SUBMODULES "Dependencies/eigen"        # eigen only — skip the AudioDSPTools submodule
    GIT_SHALLOW    FALSE                        # a SHA checkout + submodule needs full history
    SOURCE_SUBDIR  Dependencies/nlohmann       # no CMakeLists there → populate sources WITHOUT building NAM's own project
)
FetchContent_MakeAvailable(nam_core_src)

# namz — the lossless .nam<->.namz codec (github.com/darwinscat/namz, MIT), extracted from OrbitCab's own
# NamCodec so the bytes are identical. Header-only; SOURCE_SUBDIR points at include/ (no CMakeLists there)
# so we populate the header WITHOUT building namz's project (which would fetch a second nlohmann). OrbitCab's
# src/core/NamCodec is now a thin juce adapter over it; nlohmann comes from nam_core's vendored copy.
FetchContent_Declare(
    namz_src
    GIT_REPOSITORY https://github.com/darwinscat/namz.git
    GIT_TAG        v1.0.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  include        # no CMakeLists there → populate the header WITHOUT building namz's project
)
FetchContent_MakeAvailable(namz_src)

add_library(nam_core STATIC
    ${nam_core_src_SOURCE_DIR}/NAM/activations.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/container.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/conv1d.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/convnet.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/dsp.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/get_dsp.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/linear.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/lstm.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/ring_buffer.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/util.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/wavenet/a2_fast.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/wavenet/model.cpp
    ${nam_core_src_SOURCE_DIR}/NAM/wavenet/slimmable.cpp
)
target_compile_features(nam_core PUBLIC cxx_std_20)
set_target_properties(nam_core PROPERTIES POSITION_INDEPENDENT_CODE ON)   # linked into the shared plugin
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT)              # float pipeline, propagated to consumers
# SYSTEM so OrbitCab's strict warning flags don't fire inside the Eigen / NAM headers.
target_include_directories(nam_core SYSTEM PUBLIC
    ${nam_core_src_SOURCE_DIR}
    ${nam_core_src_SOURCE_DIR}/NAM
    ${nam_core_src_SOURCE_DIR}/NAM/wavenet
    ${nam_core_src_SOURCE_DIR}/Dependencies/eigen
    ${nam_core_src_SOURCE_DIR}/Dependencies/nlohmann
    ${nam_core_src_SOURCE_DIR}/Dependencies         # parent → lets namz.h resolve <nlohmann/json.hpp>
    ${namz_src_SOURCE_DIR}/include                  # namz.h — the codec behind src/core/NamCodec (juce adapter)
)

# ----------------------------------------------------------------------------
# felitronics-core — the shared, JUCE-free, header-only DSP core. OrbitCab consumes `teq::core`
# (the amp tone stack cab::AmpEq, via the transitional `<teq/*.h>` compat shim — src/ unchanged) and
# `felitronics::analysis` (the cab::CabEngine SpectrumTap). Public, AGPL, pinned by tag like JUCE.
# HYBRID: a sibling ../felitronics-core checkout if present (core co-development), else the pinned
# public release is fetched — so a clean clone / CI is self-contained. In THIRD_PARTY_NOTICES.md.
# ----------------------------------------------------------------------------
set(ORBITCAB_FCORE_TAG "v0.5.0" CACHE STRING "felitronics-core release tag (fetched when no local checkout)")
set(ORBITCAB_FCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../felitronics-core" CACHE PATH "Local felitronics-core checkout (used if present); else the pinned release is fetched")
if(ORBITCAB_FCORE_DIR AND EXISTS "${ORBITCAB_FCORE_DIR}/CMakeLists.txt")
    message(STATUS "felitronics-core: local checkout — ${ORBITCAB_FCORE_DIR}")
    FetchContent_Declare(felitronics_core SOURCE_DIR "${ORBITCAB_FCORE_DIR}")
else()
    message(STATUS "felitronics-core: fetching ${ORBITCAB_FCORE_TAG} from GitHub")
    FetchContent_Declare(felitronics_core
        GIT_REPOSITORY https://github.com/darwinscat/felitronics-core.git
        GIT_TAG        ${ORBITCAB_FCORE_TAG}
        GIT_SHALLOW    TRUE)
endif()
# Opt into the compiled pffft SIMD FFT backend (felitronics::fftpffft, BSD) BEFORE the subproject is
# configured — the cab convolver (cab::Convolver → MatrixConvolverNupc) runs its audio FFT on it. Must be
# set before MakeAvailable so modules/fftpffft is actually built and the felitronics::fftpffft alias exists.
set(FELITRONICS_WITH_PFFFT ON CACHE BOOL "OrbitCab uses the pffft SIMD FFT backend for the cab convolver" FORCE)
FetchContent_MakeAvailable(felitronics_core)
# Treat teq's headers as SYSTEM so OrbitCab's strict warning flags don't fire inside them
# (same posture as nam_core / Eigen above).
get_target_property(_teq_inc teq_core INTERFACE_INCLUDE_DIRECTORIES)
if(_teq_inc)
    set_target_properties(teq_core PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_teq_inc}")
endif()

# ----------------------------------------------------------------------------
# OrbitCabVersion.h — git-describe build stamp, baked into the binary (end users have no repo).
# Regenerated on EVERY build (the target is always out-of-date) so the stamp reflects the tree AT
# BUILD TIME; also generated once at configure so clangd + the first compile have the header. Never
# fails the build (missing git => "0.0.0-dev"/"unknown"). Replaces the old local build-counter.
# ----------------------------------------------------------------------------
find_package(Git QUIET)
set(ORBITCAB_VERSION_HEADER "${ORBITCAB_GENERATED_DIR}/OrbitCabVersion.h")

# Build architecture label for the stamp (Universal when a fat binary, else the single arch).
if(APPLE)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _oc_narch)
    if(_oc_narch GREATER 1)
        set(_oc_arch "Universal")
    elseif(CMAKE_OSX_ARCHITECTURES)
        set(_oc_arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(_oc_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
else()
    set(_oc_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Build OS label for the stamp.
if(APPLE)
    set(_oc_os "macOS")
elseif(WIN32)
    set(_oc_os "Windows")
elseif(UNIX)
    set(_oc_os "Linux")
else()
    set(_oc_os "${CMAKE_SYSTEM_NAME}")
endif()

# Mirror the felitronics-core FetchContent decision above so kCoreVersion names the checkout used.
if(ORBITCAB_FCORE_DIR AND EXISTS "${ORBITCAB_FCORE_DIR}/CMakeLists.txt")
    set(_oc_fcore_local ON)
else()
    set(_oc_fcore_local OFF)
endif()

set(_oc_ver_args
    -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
    -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DOUT_FILE=${ORBITCAB_VERSION_HEADER}
    -DARCH=${_oc_arch}
    -DOS=${_oc_os}
    -DFCORE_LOCAL=${_oc_fcore_local}
    -DFCORE_DIR=${ORBITCAB_FCORE_DIR}
    -DFCORE_TAG=${ORBITCAB_FCORE_TAG})

# Generate once at configure time too, so the header exists for IDEs / clangd before the first build.
execute_process(COMMAND ${CMAKE_COMMAND} ${_oc_ver_args}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateOrbitCabVersion.cmake")

add_custom_target(OrbitCabVersionGen ALL
    COMMAND ${CMAKE_COMMAND} ${_oc_ver_args}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateOrbitCabVersion.cmake"
    BYPRODUCTS "${ORBITCAB_VERSION_HEADER}"
    COMMENT "OrbitCab: baking OrbitCabVersion.h build stamp"
    VERBATIM)

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
    # Bundle / executable icon — the orbit mark (resources/icon/OrbitCab.svg → …-1024.png).
    # JUCE feeds this square PNG to juceaide, which builds the macOS AppIcon.icns (into every
    # .app / .vst3 / .component bundle, wired via Info.plist) and the Windows icon.ico (embedded
    # in the .exe / .vst3 through a generated .rc). One 1024² source is enough — juceaide
    # downscales it for all icon sizes (16→1024). No ICON_SMALL on purpose: JUCEUtils appends
    # BIG-then-SMALL but juceaide reads them small-then-big, so passing both can swap the slots.
    # NOTE: JUCE has no Linux icon step (launcher icons there are a .desktop + hicolor-theme
    # concern, handled at packaging). The CLAP wrapper gets its own clap.icns from JUCE — we
    # overwrite it with the orbit mark in a POST_BUILD below (see OrbitCab_CLAP). Net: all four
    # formats are branded on macOS; VST3 + Standalone also carry the .ico on Windows.
    ICON_BIG                    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/OrbitCab-1024.png"
    FORMATS                     VST3 AU Standalone
    VST3_CATEGORIES             "Fx" "Distortion"
    AU_MAIN_TYPE                "kAudioUnitType_Effect"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    # Standalone is an effect → it needs the system audio INPUT. Under macOS hardened runtime
    # (required for notarization) that means an NSMicrophoneUsageDescription in the app's Info.plist
    # AND the com.apple.security.device.audio-input entitlement (added at signing time — see
    # installer/standalone.entitlements). The plug-in formats don't need this (they run in the host).
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT  "OrbitCab needs audio input to process your signal."
    COPY_PLUGIN_AFTER_BUILD     TRUE   # auto-install to the user plug-in folders during dev
)

# Wrap the same JUCE plugin as a CLAP (.clap) — one extra format, no source changes.
clap_juce_extensions_plugin(TARGET OrbitCab
    CLAP_ID       "com.darwinscat.orbitcab"
    CLAP_FEATURES audio-effect "guitar" "cabinet")

# The CLAP wrapper ships its own default clap.icns (the generic CLAP logo) and JUCE's ICON_BIG
# does not reach it. Overwrite it in place with the orbit-mark AppIcon.icns that juceaide already
# generated for the JUCE formats (configure-time execute_process, so JUCE_ICON_FILE exists on disk
# before any build step), keeping the wrapper's existing Info.plist CFBundleIconFile=clap.icns
# reference valid. macOS only — the CLAP on Windows/Linux is a plain shared library, no icon resource.
if(APPLE)
    add_custom_command(TARGET OrbitCab_CLAP POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy
                "$<TARGET_PROPERTY:OrbitCab,JUCE_ICON_FILE>"
                "$<TARGET_BUNDLE_DIR:OrbitCab_CLAP>/Contents/Resources/clap.icns"
        VERBATIM)
endif()

target_sources(OrbitCab PRIVATE
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/UpdateChecker.cpp
    src/PresetManager.cpp
    src/PluginEditor.cpp
    src/ui/SlotComponent.cpp
    src/ui/WaveformDisplay.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp        # headless DSP core (cab::) — D24
    src/core/AmpStage.cpp      # NAM amp/poweramp stage (prototype)
    src/core/NamCodec.cpp      # .nam <-> .namz — juce adapter over the namz lib (weights as float32, store)
    src/poweramp/TubePowerAmp.cpp    # white-box tube poweramp (scaffold) — src/poweramp/PLAN.md
    src/poweramp/PowerAmpRouter.cpp  # poweramp seam: NAM capture <-> tube, click-free
)
add_dependencies(OrbitCab OrbitCabVersionGen)   # bake OrbitCabVersion.h (build stamp) before each build
# ...and force the editor TU to recompile whenever the stamp changes (every build), so the shown build
# number is always CURRENT — not frozen at PluginEditor.cpp's last source edit (which lagged it badly).
set_source_files_properties(src/PluginEditor.cpp PROPERTIES OBJECT_DEPENDS "${ORBITCAB_VERSION_HEADER}")

# ----------------------------------------------------------------------------
# Bundled IRs — embedded into the binary as a C++ byte array (no disk reads).
# juce_add_binary_data generates an `OrbitCabData` target exposing `BinaryData::*`.
# The full CC0 Brutal (01-15) + Emerald (16-21) packs — 48 kHz/24-bit
# variants (the convolver resamples to host rate anyway, so one rate is enough).
# All bundled IRs are CC0 — see ASSET-LICENSES.md / THIRD_PARTY_NOTICES.
# The editor enumerates these at runtime via BinaryData::namedResourceList — adding
# a row here is all it takes to surface a new IR in the browser (no code change).
# ----------------------------------------------------------------------------
# Factory presets — every .orbitcab in resources/presets/ is embedded + shipped read-only
# (enumerated at runtime via BinaryData; the default first-start preset is "Roche Limit").
# CONFIGURE_DEPENDS re-globs on build, so dropping a new preset in that folder ships it.
file(GLOB ORBITCAB_PRESET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/presets/*.orbitcab")

# Bundled factory power-amp NAM models (prototype): every .namz (packed .nam — weights as float32,
# store; ~5.5x smaller; see src/core/NamCodec.h) in resources/poweramps/ is embedded + exposed via
# BinaryData, selectable from the editor's poweramp radio buttons. CONFIGURE_DEPENDS re-globs on build,
# so dropping in packed .namz files ships them. (Pack a raw capture with the `orbitcab_namz` tool.)
file(GLOB ORBITCAB_POWERAMP_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/poweramps/*.namz")

# Bundled factory PRE-amp NAM models — the SECOND neural stage (input → preamp → poweramp → cab). Two
# sources, both globbed: resources/preamps/ is the tracked, shipped factory set; resources/preamps-local/
# is a gitignored drop zone for private/experimental captures (embedded on a local build, never shipped).
# These go into a SEPARATE binary-data target + namespace (PreampBinaryData), so they never bleed into the
# poweramp's BinaryData enumeration.
file(GLOB ORBITCAB_PREAMP_FILES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/preamps/*.namz"
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/preamps-local/*.namz")

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
        # Factory presets (.orbitcab) — see the glob above
        ${ORBITCAB_PRESET_FILES}
        # Factory power-amp NAM models (.nam) — prototype, see the glob above
        ${ORBITCAB_POWERAMP_FILES}
)

# Factory PRE-amp NAM models — a SEPARATE binary-data lib + namespace (PreampBinaryData) so the
# preamp factory set is enumerated independently of the poweramp's BinaryData. The README.md is
# the always-present placeholder source (juce_add_binary_data needs ≥1 file); the runtime only reads
# ".namz" entries (packed .nam), so the README is ignored by the selector.
juce_add_binary_data(OrbitCabPreampData
    NAMESPACE   PreampBinaryData
    HEADER_NAME PreampBinaryData.h
    SOURCES
        resources/preamps/README.md
        ${ORBITCAB_PREAMP_FILES}
)

# Factory spring-reverb IRs (CC0, mono, 48k/24) — a SEPARATE binary-data lib + namespace
# (ReverbBinaryData) so they never bleed into the cab's BinaryData IR enumeration
# (orbitcab::bundledIRs walks EVERY .wav in BinaryData). reverbType index 1..N maps to these
# natural-sorted by filename. See resources/reverb-ir/README.md + docs/ASSET-LICENSES.md.
juce_add_binary_data(OrbitCabReverbData
    NAMESPACE   ReverbBinaryData
    HEADER_NAME ReverbBinaryData.h
    SOURCES
        resources/reverb-ir/01-tube.wav
)

target_compile_definitions(OrbitCab PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0   # JUCE 8 has no splash requirement on any tier, incl. AGPLv3
    FELITRONICS_WITH_PFFFT=1       # cab::Convolver picks the pffft SIMD FFT backend (must match the link above)
)

target_link_libraries(OrbitCab PRIVATE
    OrbitCabData                # the embedded IR(s) + factory poweramps — BinaryData::* symbols
    OrbitCabPreampData          # the embedded factory preamps — PreampBinaryData::* symbols
    OrbitCabReverbData          # the embedded spring-reverb IRs — ReverbBinaryData::* symbols
    # NAM (prototype amp stage). WHOLE_ARCHIVE keeps NAM's architecture self-registration:
    # wavenet/lstm/convnet register via static initializers, and a plain static-lib link drops
    # those (unreferenced) objects, so get_dsp() finds no architecture and every load fails.
    # CMake 3.24+ maps this to -force_load (Apple) / --whole-archive (GNU) / /WHOLEARCHIVE (MSVC).
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>"
    teq::core                   # matched-EQ DSP core for the amp tone stack (cab::AmpEq)
    felitronics::analysis       # SPSC SpectrumTap capture in cab::CabEngine
    felitronics::convolution    # JUCE-free cab IR convolution (cab::Convolver → MatrixConvolverNupc)
    felitronics::fftpffft       # SIMD (pffft/BSD) audio FFT backend for the cab convolver
    felitronics::saturation     # tube waveshaper curves (cab::poweramp::TubePowerAmp)
    felitronics::oversampling   # alias-free 4x OS for the tube nonlinearity
    felitronics::eq             # min-phase presence/depth shelves (block 3)
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
if(MSVC)
    target_link_options(orbitcab_dsp_test PRIVATE /STACK:8388608)   # large processor on the stack — 1 MB default overflows
endif()
target_sources(orbitcab_dsp_test PRIVATE
    tools/dsp-test/test_trim.cpp
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/UpdateChecker.cpp
    src/PresetManager.cpp
    src/PluginEditor.cpp
    src/ui/SlotComponent.cpp
    src/ui/WaveformDisplay.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp
    src/core/AmpStage.cpp
    src/core/NamCodec.cpp
    src/poweramp/TubePowerAmp.cpp
    src/poweramp/PowerAmpRouter.cpp
)
target_compile_definitions(orbitcab_dsp_test PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="OrbitCab"
    JucePlugin_VersionString="1.0.0"   # plugin build gets this from juce_add_plugin; console app needs it for UpdateChecker
)
target_link_libraries(orbitcab_dsp_test PRIVATE
    OrbitCabData
    OrbitCabPreampData                          # factory preamps (PreampBinaryData) — preamp-embed test
    OrbitCabReverbData                          # embedded spring-reverb IRs (ReverbBinaryData)
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>"   # keep NAM arch self-registration (SlimmableContainer) — matches the plugin link, so the poweramp-embed test can load a .nam
    teq::core
    felitronics::analysis
    felitronics::convolution
    felitronics::saturation
    felitronics::oversampling
    felitronics::eq
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# Fade/auto-level envelope probe (#45) — prints output RMS across a MUTE toggle so the
# mute<->auto-level interaction is measured, not ear-guessed. Dev tool; not a CI gate.
# ----------------------------------------------------------------------------
juce_add_console_app(orbitcab_fade_measure PRODUCT_NAME "OrbitCabFadeMeasure")
if(MSVC)
    target_link_options(orbitcab_fade_measure PRIVATE /STACK:8388608)
endif()
target_sources(orbitcab_fade_measure PRIVATE
    tools/dsp-test/measure_fades.cpp
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/UpdateChecker.cpp
    src/PresetManager.cpp
    src/PluginEditor.cpp
    src/ui/SlotComponent.cpp
    src/ui/WaveformDisplay.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp
    src/core/AmpStage.cpp
    src/core/NamCodec.cpp
    src/poweramp/TubePowerAmp.cpp
    src/poweramp/PowerAmpRouter.cpp
)
target_compile_definitions(orbitcab_fade_measure PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="OrbitCab"
    JucePlugin_VersionString="1.0.0"
)
target_link_libraries(orbitcab_fade_measure PRIVATE
    OrbitCabData
    OrbitCabPreampData
    OrbitCabReverbData
    nam_core
    teq::core
    felitronics::analysis
    felitronics::convolution
    felitronics::saturation
    felitronics::oversampling
    felitronics::eq
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# Offline AUDITION renderer for the "Tube" power-amp: a DI wav -> Tube mode -> cab,
# writing one labelled .wav per voicing/feel setting so the sound is judged by ear.
# Dev tool; not a CI gate.  Run: orbitcab_tube_audition <DI.wav> [cabIR.wav] [outDir]
# ----------------------------------------------------------------------------
juce_add_console_app(orbitcab_tube_audition PRODUCT_NAME "OrbitCabTubeAudition")
if(MSVC)
    target_link_options(orbitcab_tube_audition PRIVATE /STACK:8388608)
endif()
target_sources(orbitcab_tube_audition PRIVATE
    tools/dsp-test/tube_audition.cpp
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/UpdateChecker.cpp
    src/PresetManager.cpp
    src/PluginEditor.cpp
    src/ui/SlotComponent.cpp
    src/ui/WaveformDisplay.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp
    src/core/AmpStage.cpp
    src/core/NamCodec.cpp
    src/poweramp/TubePowerAmp.cpp
    src/poweramp/PowerAmpRouter.cpp
)
target_compile_definitions(orbitcab_tube_audition PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="OrbitCab"
    JucePlugin_VersionString="1.0.0"
)
target_link_libraries(orbitcab_tube_audition PRIVATE
    OrbitCabData
    OrbitCabPreampData
    OrbitCabReverbData
    nam_core
    teq::core
    felitronics::analysis
    felitronics::convolution
    felitronics::saturation
    felitronics::oversampling
    felitronics::eq
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# Offline reproduction of the poweramp enable/disable KICK: toggles the tube on/off on a steady
# sine through the real processor and prints the output RMS envelope. Dev diagnostic; not a CI gate.
# ----------------------------------------------------------------------------
juce_add_console_app(orbitcab_poweramp_toggle_probe PRODUCT_NAME "OrbitCabPowerampToggleProbe")
if(MSVC)
    target_link_options(orbitcab_poweramp_toggle_probe PRIVATE /STACK:8388608)
endif()
target_sources(orbitcab_poweramp_toggle_probe PRIVATE
    tools/dsp-test/poweramp_toggle_probe.cpp
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/UpdateChecker.cpp
    src/PresetManager.cpp
    src/PluginEditor.cpp
    src/ui/SlotComponent.cpp
    src/ui/WaveformDisplay.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp
    src/core/AmpStage.cpp
    src/core/NamCodec.cpp
    src/poweramp/TubePowerAmp.cpp
    src/poweramp/PowerAmpRouter.cpp
)
target_compile_definitions(orbitcab_poweramp_toggle_probe PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="OrbitCab"
    JucePlugin_VersionString="1.0.0"
)
target_link_libraries(orbitcab_poweramp_toggle_probe PRIVATE
    OrbitCabData
    OrbitCabPreampData
    OrbitCabReverbData
    nam_core
    teq::core
    felitronics::analysis
    felitronics::convolution
    felitronics::saturation
    felitronics::oversampling
    felitronics::eq
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# Offline DIAGNOSTIC for the capture<->tube switch cleanliness: decomposes the audible switch
# transient (post-amp A/B step vs leveler vs frozen-stage state) into TSV sections. Dev tool;
# not a CI gate.  Run: orbitcab_switch_probe <model.nam> <cab.wav>
# ----------------------------------------------------------------------------
juce_add_console_app(orbitcab_switch_probe PRODUCT_NAME "OrbitCabSwitchProbe")
if(MSVC)
    target_link_options(orbitcab_switch_probe PRIVATE /STACK:8388608)
endif()
target_sources(orbitcab_switch_probe PRIVATE
    tools/dsp-test/switch_probe.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp
    src/core/AmpStage.cpp
    src/core/NamCodec.cpp
    src/poweramp/TubePowerAmp.cpp
    src/poweramp/PowerAmpRouter.cpp
)
target_include_directories(orbitcab_switch_probe PRIVATE src)
target_compile_definitions(orbitcab_switch_probe PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
)
target_link_libraries(orbitcab_switch_probe PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>"   # NAM arch self-registration (static-init TUs)
    teq::core
    felitronics::analysis
    felitronics::convolution
    felitronics::saturation
    felitronics::oversampling
    felitronics::eq
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# namz codec — offline CLI packer + a focused unit-test gate. Both link ONLY nam_core
# (for the vendored nlohmann/json include) + juce_core, so they build the `.nam`<->`.namz`
# codec in isolation — independent of the cab DSP / felitronics-core.
#   orbitcab_namz            : the CLI (tools/namz) — pack/unpack/pack-dir/verify a library
#   orbitcab_namz_test       : NamCodecTests round-trip (fast CI gate for the codec)
# ----------------------------------------------------------------------------
juce_add_console_app(orbitcab_namz PRODUCT_NAME "orbitcab-namz")
target_sources(orbitcab_namz PRIVATE
    tools/namz/namzpack.cpp
    src/core/NamCodec.cpp
)
target_include_directories(orbitcab_namz PRIVATE src)
target_compile_definitions(orbitcab_namz PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
)
target_link_libraries(orbitcab_namz PRIVATE
    nam_core                       # vendored nlohmann/json include (header-only use)
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

juce_add_console_app(orbitcab_namz_test PRODUCT_NAME "orbitcab-namz-test")
target_sources(orbitcab_namz_test PRIVATE
    tests/TestMain.cpp
    tests/NamCodecTests.cpp
    src/core/NamCodec.cpp
)
target_include_directories(orbitcab_namz_test PRIVATE src)
target_compile_definitions(orbitcab_namz_test PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
)
target_link_libraries(orbitcab_namz_test PRIVATE
    nam_core                       # vendored nlohmann/json include
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# Processor-level PDC regression GATE: at 96 kHz (NAM stages rate-match), toggling a stage's power must
# NOT change the reported latency — the invariant that, when broken, gave the enable/disable gap. Loads
# a real factory preamp so the model's rate-match latency is exercised (and must persist while powered
# off). Returns non-zero on failure. Same processor-linking recipe as the toggle probe above.
# ----------------------------------------------------------------------------
juce_add_console_app(orbitcab_latency_pdc_test PRODUCT_NAME "OrbitCabLatencyPdcTest")
if(MSVC)
    target_link_options(orbitcab_latency_pdc_test PRIVATE /STACK:8388608)
endif()
target_sources(orbitcab_latency_pdc_test PRIVATE
    tests/LatencyPdcTest.cpp
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/UpdateChecker.cpp
    src/PresetManager.cpp
    src/PluginEditor.cpp
    src/ui/SlotComponent.cpp
    src/ui/WaveformDisplay.cpp
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp
    src/core/AmpStage.cpp
    src/core/NamCodec.cpp
    src/poweramp/TubePowerAmp.cpp
    src/poweramp/PowerAmpRouter.cpp
)
target_compile_definitions(orbitcab_latency_pdc_test PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="OrbitCab"
    JucePlugin_VersionString="1.0.0"
)
target_link_libraries(orbitcab_latency_pdc_test PRIVATE
    OrbitCabData
    OrbitCabPreampData
    OrbitCabReverbData
    nam_core
    teq::core
    felitronics::analysis
    felitronics::convolution
    felitronics::saturation
    felitronics::oversampling
    felitronics::eq
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
if(MSVC)
    target_link_options(OrbitCab_Tests PRIVATE /STACK:8388608)
endif()
target_sources(OrbitCab_Tests PRIVATE
    tests/TestMain.cpp
    tests/CabEngineTests.cpp
    tests/IRSlotTests.cpp
    tests/SvfSmootherNull.cpp     # A/B null gate: felitronics eq::Svf / core::LinearSmoother vs the juce originals
    tests/AutoLevelerTests.cpp
    tests/AmpStageTests.cpp       # cab::AmpStage + StreamResampler (rate-match) — CI gate
    tests/PowerAmpRouterAlignTests.cpp # PowerAmpRouter dry-path latency alignment (no gap/jump on toggle) — CI gate
    tests/PowerAmpSwitchLevelTest.cpp  # capture<->tube mode switch: AutoLeveler must not swell (~0.5 s volume rise) — CI gate
    tests/AmpEqTests.cpp          # cab::AmpEq (teq tone stack) — passthrough / boost / cut — CI gate
    tests/PowerampLibraryTests.cpp # orbitcab::PowerampLibrary — filename → mode/tubes parse + scan
    tests/PreampLibraryTests.cpp   # orbitcab::PreampLibrary — filename → channel/gain/boost parse + scan
    tests/PreampSelectorTests.cpp  # orbitcab::PreampSelector — contextual resolve/visibility (≥2-rule, keep-else-default)
    tests/WaveformMathTests.cpp
    tests/MetadataTests.cpp     # orbitcab::Metadata — preset <meta> schema (juce_core only)
    tests/StateModelTests.cpp   # orbitcab::state — slot/sound/workspace model + (de)serialise (juce_core only)
    tests/NamCodecTests.cpp     # namz:: — .nam <-> .namz lossless codec round-trip (juce_core + nlohmann)
    tests/IrNormalizationTests.cpp # load-time IR reference-unity normalization — factory-cab acceptance gate
    src/core/CabEngine.cpp
    src/core/IRSlot.cpp
    src/core/AmpStage.cpp
    src/core/NamCodec.cpp
    src/poweramp/TubePowerAmp.cpp
    src/poweramp/PowerAmpRouter.cpp
)
target_include_directories(OrbitCab_Tests PRIVATE src)   # so tests/ can #include "core/..." and "Metadata.h"
target_compile_definitions(OrbitCab_Tests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    ORBITCAB_RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"   # real .nam (router align) + factory IRs (IR norm)
)
target_link_libraries(OrbitCab_Tests PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>"   # AmpStage.cpp + NAM arch self-registration
    teq::core                     # cab::AmpEq (AmpEqTests)
    felitronics::analysis         # cab::CabEngine SpectrumTap capture
    felitronics::convolution      # cab::Convolver
    felitronics::saturation       # cab::poweramp::TubePowerAmp curves
    felitronics::oversampling     # cab::poweramp::TubePowerAmp 4x OS
    felitronics::eq               # block-3 presence/depth shelves
    juce::juce_dsp                # pulls juce_audio_basics / juce_audio_formats / juce_core
    juce::juce_data_structures    # juce::ValueTree for orbitcab::state (StateModel.h)
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# ----------------------------------------------------------------------------
# Headless golden test for the cab convolution backend (cab::Convolver) vs a direct-convolution
# reference: impulse->normalized-IR, noise==direct-conv, reference-unity gain, mono broadcast, file-
# scale invariance, silent-IR floor, true-stereo imaging. CI gate (non-zero on failure).
# NOTE: cab::Convolver is now backed by juce::dsp::Convolution, so this target is no longer JUCE-free
# — it links juce::juce_dsp (which pulls juce_core for the async-loader pump). The convolution CORE is
# JUCE-dependent again (accepted tradeoff; the IR reference-unity analysis stays felitronics-only).
# ----------------------------------------------------------------------------
add_executable(orbitcab_coreconv_golden tests/CoreConvolverGolden.cpp)
target_include_directories(orbitcab_coreconv_golden PRIVATE src)
target_compile_definitions(orbitcab_coreconv_golden PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    FELITRONICS_WITH_PFFFT=1)               # validate the SHIPPING backend (cab::Convolver → pffft), not the scalar fallback
target_link_libraries(orbitcab_coreconv_golden PRIVATE
    felitronics::convolution               # cab::Convolver backend (MatrixConvolverNupc) + resample-on-load
    felitronics::fftpffft                   # pffft SIMD audio FFT — the backend OrbitCab ships
    juce::juce_dsp                          # juce_core (juce::Thread::sleep) for the reload poll
    juce::juce_recommended_config_flags)
target_compile_features(orbitcab_coreconv_golden PRIVATE cxx_std_20)

# ----------------------------------------------------------------------------
# Headless golden for the white-box tube poweramp CORE (block 2): cab::poweramp::TubePowerAmp —
# an oversampled PP/SE tube waveshaper. Asserts THD-vs-drive, PP even-harmonic cancellation vs SE
# even-rich, <-70 dBc aliasing @4x, near-identity at Drive=min, and the 31-sample invariant latency.
# JUCE-free (links only the felitronics header-only DSP). CI gate (non-zero on failure).
# ----------------------------------------------------------------------------
add_executable(orbitcab_poweramp_core_golden tests/PowerAmpCoreGolden.cpp src/poweramp/TubePowerAmp.cpp)
target_include_directories(orbitcab_poweramp_core_golden PRIVATE src)
target_link_libraries(orbitcab_poweramp_core_golden PRIVATE felitronics::saturation felitronics::oversampling felitronics::eq)
target_compile_features(orbitcab_poweramp_core_golden PRIVATE cxx_std_20)

# Real-material proof: JUCE (cab::Convolver) vs core (cab::CoreConvolver) nulled on REAL cab IRs + a
# real sweep. Takes WAV paths on argv (so it links juce_audio_formats + juce_dsp + the core convolver).
juce_add_console_app(orbitcab_realir PRODUCT_NAME "OrbitCabRealIr")
target_sources(orbitcab_realir PRIVATE tests/CoreConvolverRealIr.cpp)
target_include_directories(orbitcab_realir PRIVATE src)
target_compile_definitions(orbitcab_realir PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    FELITRONICS_WITH_PFFFT=1)               # validate the SHIPPING backend (cab::Convolver → pffft)
target_link_libraries(orbitcab_realir PRIVATE
    felitronics::convolution
    felitronics::fftpffft                   # pffft SIMD audio FFT — the backend OrbitCab ships
    juce::juce_dsp
    juce::juce_audio_formats
    juce::juce_recommended_config_flags)
