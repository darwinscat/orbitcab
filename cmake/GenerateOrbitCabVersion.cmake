# SPDX-License-Identifier: AGPL-3.0-or-later
# ----------------------------------------------------------------------------
# OrbitCabVersion.h build-time stamp generator — invoked via `cmake -P` by the OrbitCabVersionGen
# custom target on EVERY build (that target is always out-of-date on purpose), so:
#   • kBuildNumber is a FRESH 14-digit UTC timestamp each build (later build => bigger number;
#     unique across machines with no coordination), and
#   • the git describe / hash / dirty flag reflect the working tree AT BUILD TIME — never the
#     stale value a configure-time snapshot would freeze in.
# End users have no repo, so the header is baked into the binary; this script is the only place git
# is ever consulted. It must NEVER fail the build: any missing git / failing command degrades
# gracefully to a sane literal ("unknown", "0.0.0-dev"). Replaces the old local build-counter.
#
# Expected -D inputs (all optional; each has a graceful default):
#   GIT_EXECUTABLE   path to git (empty => no git available)         SRC_DIR   this repo's root
#   OUT_FILE         absolute path of the header to (re)write         ARCH      build arch label
#   FCORE_LOCAL      ON when the sibling felitronics-core checkout is used, OFF when the pin is fetched
#   FCORE_DIR        the sibling checkout path (only read when FCORE_LOCAL)
#   FCORE_TAG        the pinned felitronics-core tag (used when fetched, or as a local fallback)
# ----------------------------------------------------------------------------

# --- kBuildNumber: 14-digit UTC YYYYMMDDHHMMSS -----------------------------------------------
string(TIMESTAMP _build_number "%Y%m%d%H%M%S" UTC)

# --- kBuilder: env ORBITCAB_BUILDER (CI sets =ci) else the local username --------------------
if(DEFINED ENV{ORBITCAB_BUILDER} AND NOT "$ENV{ORBITCAB_BUILDER}" STREQUAL "")
    set(_builder "$ENV{ORBITCAB_BUILDER}")
elseif(DEFINED ENV{USER} AND NOT "$ENV{USER}" STREQUAL "")
    set(_builder "$ENV{USER}")           # POSIX
elseif(DEFINED ENV{USERNAME} AND NOT "$ENV{USERNAME}" STREQUAL "")
    set(_builder "$ENV{USERNAME}")       # Windows
else()
    set(_builder "unknown")
endif()

# --- kDescribe / kGitHash / kGitDirty / kBuildCount (build-time; graceful without git) -------
set(_describe   "0.0.0-dev")   # default when there is no git or no repo
set(_hash       "unknown")
set(_dirty      "false")
set(_buildcount "-1")          # commits since the last tag; -1 = unknown (no git / no tag)
if(GIT_EXECUTABLE AND SRC_DIR)
    # Does the repo carry ANY tag? (describe --always silently falls back to a bare hash, which
    # is indistinguishable from a real describe — so we probe for a tag explicitly.)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
        WORKING_DIRECTORY "${SRC_DIR}"
        RESULT_VARIABLE _tag_rc OUTPUT_QUIET ERROR_QUIET)

    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
        WORKING_DIRECTORY "${SRC_DIR}"
        RESULT_VARIABLE _desc_rc OUTPUT_VARIABLE _desc_out
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        RESULT_VARIABLE _hash_rc OUTPUT_VARIABLE _hash_out
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

    if(_hash_rc EQUAL 0 AND NOT _hash_out STREQUAL "")
        set(_hash "${_hash_out}")
    endif()

    if(_desc_rc EQUAL 0 AND NOT _desc_out STREQUAL "")
        # dirty flag: derived from describe's own "-dirty" suffix (tracked-file changes), so it
        # stays consistent with what kDescribe shows.
        if(_desc_out MATCHES "-dirty$")
            set(_dirty "true")
        endif()
        if(_tag_rc EQUAL 0)
            set(_describe "${_desc_out}")     # real tag-based describe, e.g. v1.6.0-18-g6550266[-dirty]
            # kBuildCount = the "-N-" in the describe (commits past the tag); 0 when exactly on a tag.
            if(_desc_out MATCHES "-([0-9]+)-g[0-9a-fA-F]+")
                set(_buildcount "${CMAKE_MATCH_1}")
            else()
                set(_buildcount "0")
            endif()
        else()
            set(_describe "0.0.0-dev")         # no tags yet: describe only had a bare hash to give
        endif()
    endif()
endif()

# --- kArch: the build architecture label (passed in by the parent CMake) --------------------
set(_arch "unknown")
if(ARCH)
    set(_arch "${ARCH}")
endif()

# --- kCoreVersion: the resolved felitronics-core --------------------------------------------
# The parent CMake knows which FetchContent path it took (sibling checkout vs pinned fetch) and
# passes FCORE_LOCAL. Sibling => describe it live (it may have advanced); fetched => the pin.
set(_core "unknown")
if(FCORE_LOCAL AND FCORE_DIR AND EXISTS "${FCORE_DIR}/.git")
    if(GIT_EXECUTABLE)
        execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --always
            WORKING_DIRECTORY "${FCORE_DIR}"
            RESULT_VARIABLE _c_rc OUTPUT_VARIABLE _c_out
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_c_rc EQUAL 0 AND NOT _c_out STREQUAL "")
            set(_core "${_c_out} (local)")
        endif()
    endif()
    if(_core STREQUAL "unknown" AND FCORE_TAG)
        set(_core "${FCORE_TAG} (local)")    # sibling present but not a git repo
    endif()
elseif(FCORE_TAG)
    set(_core "${FCORE_TAG}")                 # the pinned public release was fetched
endif()

# --- escape the collected values for C++ double-quoted string literals -----------------------
# git tags may legally carry '"' or '\' (e.g. v1.2.3"x) and ORBITCAB_BUILDER / USER are arbitrary
# env text — unescaped they would break the emitted header's compilation. Backslash and quote are
# escaped; tab/CR/LF collapse to a space; UTF-8 bytes above ASCII pass through untouched.
function(_orbitcab_cxx_escape var)
    set(_v "${${var}}")
    string(REPLACE "\\" "\\\\" _v "${_v}")
    string(REPLACE "\"" "\\\"" _v "${_v}")
    string(REGEX REPLACE "[\t\r\n]" " " _v "${_v}")
    set(${var} "${_v}" PARENT_SCOPE)
endfunction()

_orbitcab_cxx_escape(_describe)
_orbitcab_cxx_escape(_hash)
_orbitcab_cxx_escape(_builder)
_orbitcab_cxx_escape(_core)
_orbitcab_cxx_escape(_arch)

# --- emit the header ------------------------------------------------------------------------
set(_content "// SPDX-License-Identifier: AGPL-3.0-or-later
// GENERATED at build time by cmake/GenerateOrbitCabVersion.cmake — DO NOT EDIT, DO NOT COMMIT.
// Baked into the binary so end users (who have no git repo) still get a precise build stamp.
#pragma once

namespace orbitcab::version
{
    // `git describe --tags --always --dirty`, or \"0.0.0-dev\" when the repo carries no tag yet,
    // or \"unknown\" when git is unavailable entirely.
    inline constexpr const char* kDescribe    = \"${_describe}\";

    // UTC build timestamp as YYYYMMDDHHMMSS (14 digits) — later build => bigger number.
    inline constexpr long long   kBuildNumber = ${_build_number}LL;

    // Commits since the last tag (the \"-N-\" in kDescribe); 0 when exactly on a tag, -1 if unknown.
    // The on-screen badge shows \"build N\" only when this is > 0 (i.e. a dev build past a release).
    inline constexpr int         kBuildCount  = ${_buildcount};

    inline constexpr const char* kGitHash     = \"${_hash}\";   // short HEAD hash (or \"unknown\")
    inline constexpr bool        kGitDirty    = ${_dirty};       // uncommitted tracked changes present
    inline constexpr const char* kBuilder     = \"${_builder}\"; // env ORBITCAB_BUILDER, else username
    inline constexpr const char* kCoreVersion = \"${_core}\";    // resolved felitronics-core
    inline constexpr const char* kArch        = \"${_arch}\";    // build arch (arm64 / x86_64 / Universal)
}
")

# Write only when the content actually changed, to avoid a needless recompile when the second
# resolution lands in the same UTC second (the build number is then identical). The timestamp
# advances every real second, so consecutive builds normally DO rewrite (that is the intent).
if(EXISTS "${OUT_FILE}")
    file(READ "${OUT_FILE}" _existing)
    if(_existing STREQUAL "${_content}")
        return()
    endif()
endif()
file(WRITE "${OUT_FILE}" "${_content}")
