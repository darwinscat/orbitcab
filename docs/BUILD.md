# Build

> Windows builds in CI (`.github/workflows/build.yml`); macOS is built + validated
> locally. The commands below are for local macOS dev (Apple clang, CMake 4.x).
> Releases (signed later) are cut by `.github/workflows/release.yml` on a `vX.Y.Z` tag.

## Toolchain

- **CMake** ≥ 3.22 — drives everything.
- **JUCE 8** — fetched by CMake (`FetchContent`, pinned to tag `8.0.13`). Not
  vendored, not system-installed. The version is the single knob `ORBITCAB_JUCE_TAG` in
  `CMakeLists.txt` — bump it there to upgrade JUCE.
- **Compiler:** Apple clang on macOS, MSVC on Windows. **C++20**.
- macOS: Xcode command-line tools (for `codesign`/`notarytool`/`auval`).

## Everyday commands

```bash
# 1. Configure (run once, or after editing CMakeLists.txt). The FIRST configure
#    downloads + builds JUCE — that's the slow ~minute+; it's cached afterwards.
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 2. Build all formats (VST3 + AU + Standalone). After edits to src/*, only the
#    changed files recompile — seconds.
#    ⚠️ Build the DEFAULT target (no --target). Do NOT use `--target OrbitCab`: that name
#    is the shared-code static lib only — it recompiles src/* and updates
#    libOrbitCab_SharedCode.a but does NOT relink the VST3/AU/Standalone bundles or run
#    COPY_PLUGIN_AFTER_BUILD, so every .vst3/.component/.app stays the OLD binary
#    (silent stale build — even auval/pluginval then test old code). To build/refresh
#    only the plugin, target the formats: OrbitCab_VST3 / OrbitCab_AU / OrbitCab_Standalone.
cmake --build build --config Release

# 3. Run the Standalone app to see/hear it without a DAW (macOS):
open build/OrbitCab_artefacts/Release/Standalone/OrbitCab.app

# Build + run in one go (run only if the build succeeds):
cmake --build build --config Release && open build/OrbitCab_artefacts/Release/Standalone/OrbitCab.app
```

> macOS universal binary (arm64 + x86_64) is the default — set in `CMakeLists.txt`.
> To build a single arch faster during dev, configure with e.g.
> `-DCMAKE_OSX_ARCHITECTURES=arm64`.

## Build outputs

Artefacts land under `build/OrbitCab_artefacts/Release/`:

- `VST3/OrbitCab.vst3`
- `AU/OrbitCab.component`
- `Standalone/OrbitCab.app`

`COPY_PLUGIN_AFTER_BUILD TRUE` also auto-installs the plug-ins to the user folders
during dev, so a DAW picks them up immediately:

- macOS VST3: `~/Library/Audio/Plug-Ins/VST3/OrbitCab.vst3`
- macOS AU:   `~/Library/Audio/Plug-Ins/Components/OrbitCab.component`
- Windows VST3 (via the Inno installer): `C:\Program Files\Common Files\VST3\OrbitCab.vst3`

## Validate

```bash
# macOS AU — Apple's validator. aufx = effect, Orbt = plugin code, Dcat = mfr code.
auval -v aufx Orbt Dcat

# Cross-platform plugin validator (Tracktion's pluginval — also run @10 in build.yml CI)
pluginval --strictness-level 10 --validate build/OrbitCab_artefacts/Release/VST3/OrbitCab.vst3
```

## Clean rebuild

```bash
rm -rf build       # nukes the JUCE cache too -> next configure is slow again
```

## ⚠️ Testing in a DAW after a rebuild — reload the plugin!

`COPY_PLUGIN_AFTER_BUILD` installs the new binary to the user plug-in folders, **but a
running host keeps the already-loaded instance** — so a rebuild does NOT update a plugin
that's open in a session. **Remove + re-add the plugin (or restart / rescan the host)**
to pick up the new build. (This cost real time once: "TRIM doesn't work" was just a stale
instance — `tools/dsp-test/` proved the DSP was correct: 1195 ms → 298 ms at 25% trim.)
The **Standalone** (`open build/OrbitCab_artefacts/Release/Standalone/OrbitCab.app`) always runs
the fresh binary, so it's the quickest sanity check.

## Signing / notarization / CI

**Wired:** `build.yml` (on-demand /
code-PR, mac+win + pluginval, no secrets) and `release.yml` (tag `vX.Y.Z` → `.pkg` + Inno
`.exe` + zips → rsync to the download server, secrets `SSH_PRIVATE_KEY`/`KNOWN_HOSTS`).
**Still unsigned** — Developer ID signing + notarization (macOS) and Windows code
signing are the beta gates; until then macOS needs right-click→Open + dequarantine
and Windows shows SmartScreen. See `installer/README.md`.
