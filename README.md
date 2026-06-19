# OrbitCab — Free Open-Source IR Cabinet Loader (VST3 / AU / CLAP)

Load a guitar or bass cabinet impulse response (`.wav`) and hear your DI through it,
in any DAW. Free and open-source — no account, no trial, no lock-in. Zero-latency
convolution; VST3 · AU · CLAP, for Windows, macOS, and Linux. *A cabinet on orbit.*

**OrbitCab** is part of the **Felitronics** plugin line by **Darwin's Cat** — the
sound-utils ecosystem ([darwinscat.com/orbitcab](https://darwinscat.com/orbitcab)).

---

<!-- TODO @release: add a UI screenshot/GIF at the top here, e.g. ![OrbitCab](docs/screenshot.png) -->

## What it is

Drop in a cabinet impulse response and OrbitCab plays your signal through it — the
software version of micing a real cab. Run two IRs at once and blend between them,
shape each with HPF / LPF / trim / phase / dry-wet, A/B your settings, and save it all
in your DAW session. Bundled cabinet packs get you started; drag and drop your own
anytime.

OrbitCab is the cabinet, not the amp — place it **after** your amp sim, preamp, or
Neural Amp Modeler (NAM) amp-head capture. NAM gives you the amp; OrbitCab gives you
the cabinet.

## Features

- Load IRs by file/folder browser or drag-and-drop, plus bundled cabinet packs
- Two IR slots (I / II), each with HPF · LPF · trim · phase · dry/wet
- A↔B blend between the two slots
- A/B/C/D snapshots, undo/redo, auto-level, live spectrum
- Presets that export/import with the IR embedded
- State saved in the DAW session, and versioned so updates don't break old sessions

## Download

Get the VST3, AU, CLAP, and standalone builds for Windows, macOS, and Linux from the
[**Releases**](https://github.com/darwinscat/orbitcab/releases/latest) page, then drop
the plugin into your system plugin folder and rescan in your DAW.

## Build from source

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The first configure fetches and builds JUCE (pinned tag); artefacts land in
`build/OrbitCab_artefacts/`. See [`docs/BUILD.md`](docs/BUILD.md) for validation
(`auval`, `pluginval`) and cutting a release.

## Formats

| Format | Platforms | SDK license |
|--------|-----------|-------------|
| **VST3** | Windows · macOS · Linux — all DAWs | MIT |
| **AU**   | macOS — Logic / GarageBand | Apple, Apache-2.0 SDK |
| **CLAP** | Windows · macOS · Linux — Reaper / Bitwig / FL / Studio One / Ardour | MIT (`clap-juce-extensions`) |
| **Standalone** | Windows · macOS · Linux — no DAW needed | — |

## License

**GNU AGPLv3 or later** (see [`LICENSE`](LICENSE)). OrbitCab is free and open source;
if you distribute it — modified or not — you must pass the corresponding source along
under the same license (AGPL §13's network clause adds nothing for a locally-run
plugin). It links the **JUCE** framework under JUCE's AGPLv3 option (JUCE is
dual-licensed AGPLv3 / commercial). Third-party SDK, font, and bundled-IR notices are
in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). The OrbitCab / Darwin's Cat names
and logos are trademarks and are **not** covered by the code license.

## Docs

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — signal chain, state model, code layout
- [`docs/IR-LOADER-DESIGN.md`](docs/IR-LOADER-DESIGN.md) — IR loader design
- [`docs/BUILD.md`](docs/BUILD.md) — how to build, validate, and cut a release
- [`docs/CODING-STYLE.md`](docs/CODING-STYLE.md) — style conventions + `.clang-format`
- [`docs/ASSET-LICENSES.md`](docs/ASSET-LICENSES.md) — per-asset ledger for bundled IRs + test audio

## Contributing

Issues and PRs are welcome — see [`CONTRIBUTING.md`](CONTRIBUTING.md). Changes to `main`
go through a reviewed pull request.
