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
- **Source:** https://github.com/sdatkinson/NeuralAmpModelerCore — consumed transitively through
  felitronics-core v0.13.0's opt-in `felitronics::nam` module. felitronics-core fetches NAM at a
  pinned commit and compiles its sources into the backend used for the optional amp stages in front
  of the cab. OrbitCab bundles no user-supplied `.nam` models; captures retain their own licenses.
- **Dependencies fetched at felitronics-core's pins:**
  - **Eigen** (linear algebra, header-only) — **MPL-2.0** — © the Eigen authors —
    https://gitlab.com/libeigen/eigen (NAM git submodule).
  - **nlohmann/json** (`json.hpp`, header-only) — **MIT** — © Niels Lohmann —
    https://github.com/nlohmann/json (vendored in the NAM repo).

### felitronics-core (shared JUCE-free DSP core)
- **License:** GNU AGPLv3-or-later (same as OrbitCab; AGPL-compatible by identity).
- **Copyright:** © Darwin's Cat — Oleh Tsymaienko and Alisa Lafoks. First-party, but a
  **separately versioned** sibling library (the shared JUCE-free DSP core), so it's recorded here.
- **Source:** https://github.com/darwinscat/felitronics-core — fetched via CMake `FetchContent`
  at a pinned release tag (`ORBITCAB_FCORE_TAG` in [`CMakeLists.txt`](CMakeLists.txt)), or a local
  sibling checkout for core co-development. OrbitCab consumes v0.13.0's header-only DSP modules plus
  the opt-in compiled `felitronics::nam` module: `teq::core` (the matched-EQ engine used by
  `cab::AmpEq`), `felitronics::analysis` (the lock-free `SpectrumTap`),
  `felitronics::convolution` (`CabConvolver`, aliased as `cab::Convolver`), `felitronics::core`
  (`StreamResampler`), and `felitronics::nam` (`NamStage`, aliased as `cab::AmpStage`).

### namz (lossless .nam ↔ .namz codec)
- **License:** MIT.
- **Copyright:** © Darwin's Cat — Oleh Tsymaienko and Alisa Lafoks. First-party, but a
  **separately versioned** sibling library — extracted from OrbitCab's own codec (byte-identical) and
  released under **MIT** so other tools can reuse it, so it's recorded here.
- **Source:** https://github.com/darwinscat/namz — consumed transitively through felitronics-core
  v0.13.0's `felitronics::nam` module, which pins namz v1.1.1 by immutable commit. `NamStage.cpp` in
  that module compiles `NAMZ_IMPLEMENTATION` exactly once and exports the declarations and symbols to
  OrbitCab. It backs `src/core/NamCodec.{h,cpp}` and the rig readers: the lossless `.nam` (JSON
  weights) ↔ `.namz` (float32-packed) round-trip for bundled/imported captures.

### pffft (SIMD FFT backend for the cab convolution — vendored in felitronics-core)
- **License:** BSD-style (FFTPACK5 / UCAR) — AGPL-compatible.
- **Copyright:** © Julien Pommier (2013). Derived from FFTPACKv4 by Dr Paul Swarztrauber (NCAR, 1985).
- **Source:** vendored **pristine** inside felitronics-core at `modules/fftpffft/pffft/{pffft.c,pffft.h}`,
  upstream `https://bitbucket.org/jpommier/pffft` (commit `09796885cd5b9da5692242de2df0d81e5e1f3d21`).
- **Why it's here:** OrbitCab enables the SIMD path (`FELITRONICS_WITH_PFFFT=ON`) so the cab IR convolution
  (`cab::Convolver` → `MatrixConvolverNupc`) runs its audio FFT on `felitronics::fftpffft::PffftRealFft`.
  The FFTPACK/UCAR licence requires the copyright notice be reproduced in the documentation of binary
  distributions — this entry carries it. The two vendored files keep their upstream BSD/FFTPACK header
  verbatim (no SPDX rewrite), so upstream diffs stay byte-clean.

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
programmatically; the same mark also ships as the app/bundle icon (`resources/icon/`, fed to
JUCE's `ICON_BIG`). These are **not** covered by the AGPLv3 grant on
the source code — trademarks and brand assets are reserved.
