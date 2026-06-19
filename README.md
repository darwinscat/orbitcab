# OrbitCab — IR Cabinet Loader

A small, free **impulse-response (IR) cabinet loader** audio plugin — *"a cabinet on orbit."*
C++ / [JUCE](https://juce.com). Ships as **VST3 + AU** (CLAP later).

**OrbitCab** is part of the **Felitronics** plugin line, by **Darwin's Cat** — the
sound-utils ecosystem (`darwinscat.com/sound-utils/cabinet-ir-utility` → IR capture →
**this plugin in your DAW**).

> **Status:** v1 IR Loader is **built** — VST3 + AU + Standalone, validated (auval +
> pluginval @10).

---

## What it is

Load a guitar/bass cabinet impulse response (`.wav`) and hear your DI through it,
in any DAW. The DSP heavy-lifting is `juce::dsp::Convolution` (zero-latency
partitioned convolution, auto-resamples the IR to the host sample rate) — so the
"make a convolver" part is essentially free; the project is really about UI,
packaging, and distribution.

## Features

**v1 — IR Loader** (built):
load IR (file/folder browser + drag-drop) + bundled Brutal/Emerald packs · two IR
boxes (I/II) with per-box HPF/LPF/TRIM/HEAD/phase/dry-wet · A↔B blend · A/B/C/D
snapshots · auto-level · live spectrum · undo/redo · presets (export/import with the
IR embedded) · state saved in the DAW session.

More is planned later. The signal chain is built modular (`juce::dsp::ProcessorChain`)
and the saved state is **versioned from day one** so future versions don't break v1
sessions.

## Formats

| Format | v1? | License of SDK | Notes |
|--------|-----|----------------|-------|
| **VST3** | ✅ | MIT (since SDK 3.8, Oct 2025) | all DAWs, Win/Mac/Linux |
| **AU**   | ✅ | Apple, Apache-2.0 SDK | macOS only (Logic/GarageBand) |
| **CLAP** | later | MIT (`clap-juce-extensions`, unofficial; native in JUCE 9) | Reaper/Bitwig/FL/Studio One |
| **AAX**  | not planned | Avid + PACE + iLok | Pro Tools only |

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
- [`docs/IR-LOADER-DESIGN.md`](docs/IR-LOADER-DESIGN.md) — v1 IR loader design
- [`docs/BUILD.md`](docs/BUILD.md) — how to build, validate, and cut a release
- [`docs/CODING-STYLE.md`](docs/CODING-STYLE.md) — style conventions + `.clang-format`
- [`docs/ASSET-LICENSES.md`](docs/ASSET-LICENSES.md) — per-asset ledger for bundled IRs + test audio
