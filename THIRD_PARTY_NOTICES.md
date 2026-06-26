# Third-party notices — OrbitCab

OrbitCab itself is licensed under the **GNU Affero General Public License v3.0 or
later** (see [`LICENSE`](LICENSE)). It links and/or bundles the components below,
each under its own license. This file collects the notices those licenses require.
The bundled content (IRs, font) is ledgered in
[`docs/ASSET-LICENSES.md`](docs/ASSET-LICENSES.md).

---

## Frameworks & SDKs (linked)

### JUCE (audio framework)
- **License used here:** GNU AGPLv3. JUCE is **dual-licensed** under the commercial
  JUCE licence *and* the GNU AGPLv3; OrbitCab uses the **AGPLv3** branch (free, no
  revenue cap). Distributing OrbitCab under the AGPLv3 satisfies JUCE's AGPLv3 terms.
- **Copyright:** © Raw Material Software Limited.
- **Source:** https://github.com/juce-framework/JUCE — fetched via CMake
  `FetchContent` at a pinned tag (see [`CMakeLists.txt`](CMakeLists.txt)); not vendored.
- JUCE 8 imposes **no "Made with JUCE" splash-screen** requirement on any tier
  (removed in JUCE 8), so OrbitCab sets `JUCE_DISPLAY_SPLASH_SCREEN=0`.

### Steinberg VST3 SDK
- **License:** MIT (since SDK 3.8.0, 2025-10-20).
- **Copyright:** © Steinberg Media Technologies GmbH.
- **Source:** https://github.com/steinbergmedia/vst3sdk
- "VST" is a trademark of Steinberg Media Technologies GmbH. OrbitCab is "VST3
  compatible"; it does not use "VST" in its product or company name.

### Apple AudioUnit SDK
- **License:** Apache License 2.0.
- **Copyright:** © Apple Inc.
- **Source:** https://github.com/apple/AudioUnitSDK

### CLAP format — clap-juce-extensions + the CLAP SDK
- **License:** MIT (both).
- **Copyright:** © the clap-juce-extensions authors; © the CLAP / free-audio authors.
- **Source:** https://github.com/free-audio/clap-juce-extensions (wraps the JUCE plugin as
  a CLAP) which pulls in https://github.com/free-audio/clap (the CLAP SDK). Fetched via
  CMake `FetchContent` at a pinned tag; not vendored.

### NeuralAmpModelerCore (NAM — neural amp/poweramp inference)
- **License:** MIT.
- **Copyright:** © Steven Atkinson and the Neural Amp Modeler contributors.
- **Source:** https://github.com/sdatkinson/NeuralAmpModelerCore — fetched via CMake
  `FetchContent` at a pinned commit (see `ORBITCAB_NAM_TAG` in [`CMakeLists.txt`](CMakeLists.txt));
  the `NAM/*.cpp` sources are compiled into a `nam_core` static library. Used for the
  optional amp stage that runs in front of the cab. Bundles no `.nam` models — those are
  user-supplied captures with their own licenses.
- **Bundled deps it pulls in:**
  - **Eigen** (linear algebra, header-only) — **MPL-2.0** — © the Eigen authors —
    https://gitlab.com/libeigen/eigen (NAM git submodule).
  - **nlohmann/json** (`json.hpp`, header-only) — **MIT** — © Niels Lohmann —
    https://github.com/nlohmann/json (vendored in the NAM repo).

---

## Bundled content (embedded in the binary)

### Impulse responses — Jesters Brutal Pack 1.0 & Jesters Emerald Pack
- **License:** **CC0 1.0** (public domain dedication) — no attribution required; we
  credit the source as a courtesy.
- 21 IRs total: `01–15` Brutal (mod. Behringer BG412S — Celestion V30 / Rockdriver Jr
  / Eminence DV-77), `16–21` Emerald (Marshall 1960AX — Celestion Greenback G12M).
- See [`docs/ASSET-LICENSES.md`](docs/ASSET-LICENSES.md) for proof of the CC0 grant
  and full capture details.

### Michroma (font, header wordmark)
- **License:** SIL Open Font License 1.1.
- **Copyright:** © The Michroma Project Authors
  (https://github.com/googlefonts/Michroma-font).
- OFL text bundled at `resources/fonts/Michroma-OFL.txt`. OFL §1 permits embedding in
  software (including under a different license such as AGPLv3); the font's own terms
  remain OFL.

---

## First-party

The **Darwin's Cat** logo (`resources/brand/logo-darwinscat.svg`) and the OrbitCab
name/marks are © Darwin's Cat / Oleh Tsymaienko & Alisa. The orbit "O" header mark is drawn
programmatically (no bundled asset). These are **not** covered by the AGPLv3 grant on
the source code — trademarks and brand assets are reserved.
