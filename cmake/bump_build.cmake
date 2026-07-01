# SPDX-License-Identifier: AGPL-3.0-or-later
# Increments a persistent LOCAL build counter and (re)writes ${OUT} = generated/BuildInfo.h with the
# number + a HH:MM:SS stamp. Invoked on every build by the orbitcab_bump_build target (CMakeLists.txt),
# so the editor can show which build the host actually loaded. Not shipped in tagged release builds.
#
# Scope = PER REPO CHECKOUT: the counter lives in the source root (${SRCDIR}/.orbitcab_build_number,
# gitignored), so it's ONE monotonic number shared across build dirs (build/ + build-release/) and it
# survives a build-tree wipe. It's local to this machine + this clone (not committed, not shared).
set(counter "${SRCDIR}/.orbitcab_build_number")
set(n 0)
if(EXISTS "${counter}")
    file(READ "${counter}" n)
    string(STRIP "${n}" n)
endif()
if(NOT n MATCHES "^[0-9]+$")
    set(n 0)
endif()
math(EXPR n "${n} + 1")
file(WRITE "${counter}" "${n}")
string(TIMESTAMP ts "%H:%M:%S")
file(WRITE "${OUT}"
     "// GENERATED — local build id (cmake/bump_build.cmake). Do not edit / commit.\n"
     "#pragma once\n"
     "#define ORBITCAB_BUILD_NUMBER ${n}\n"
     "#define ORBITCAB_BUILD_STAMP \"${ts}\"\n")
