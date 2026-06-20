# OrbitCab

![OrbitCab — free guitar & bass cabinet IR loader (VST3 · AU · CLAP, Windows · macOS · Linux)](docs/banner.jpg)

Free, open-source impulse-response (IR) cabinet loader for electric guitar and bass.
Load a cabinet IR and hear your DI through it — in any DAW.

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL%20v3-blue.svg)](LICENSE)
[![Latest release](https://img.shields.io/github/v/release/darwinscat/orbitcab)](https://github.com/darwinscat/orbitcab/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/darwinscat/orbitcab/total)](https://github.com/darwinscat/orbitcab/releases)

## Features

- Two IR slots with A↔B blend
- Per-slot HPF, LPF, trim, head alignment, phase, and dry/wet
- A/B/C/D snapshots, undo/redo, auto-level, live spectrum
- File/folder browser and drag-and-drop loading; bundled cabinet packs
- Presets that export/import with the IR embedded
- Session state is versioned, so updates don't break old projects

## Install

Grab the latest build from the
[Releases](https://github.com/darwinscat/orbitcab/releases/latest) page:

- **macOS** — open `OrbitCab-<ver>-macOS.pkg` (signed + notarized — installs cleanly, no
  Gatekeeper prompt). Or take the `.zip` and copy the bundles into
  `~/Library/Audio/Plug-Ins/` (`VST3/`, `Components/`, `CLAP/`).
- **Windows** — run `OrbitCab-<ver>-Windows-Setup.exe` (x64). It isn't code-signed yet, so
  SmartScreen warns — click **More info → Run anyway**. On Arm (Snapdragon) machines use
  `OrbitCab-<ver>-Windows-arm64.zip` and copy the plugins into your VST3 / CLAP folders.
- **Linux** — extract `OrbitCab-<ver>-Linux-<arch>.tar.gz` and copy `OrbitCab.vst3` /
  `OrbitCab.clap` into `~/.vst3` / `~/.clap`.

Then rescan in your DAW. A **standalone** app is included for use without a DAW.

Verify a download against [`SHA256SUMS`](https://github.com/darwinscat/orbitcab/releases/latest)
(attached to each release): `shasum -a 256 -c SHA256SUMS` (macOS) or `sha256sum -c SHA256SUMS` (Linux).

| Format | Platforms |
|--------|-----------|
| VST3   | Windows, macOS, Linux |
| CLAP   | Windows, macOS, Linux |
| AU     | macOS |
| Standalone | Windows, macOS, Linux |
| AAX (Pro Tools) | Not supported |

macOS builds are universal (Apple Silicon + Intel); Windows ships x64 and arm64; Linux ships x86_64 and arm64.

> No AAX/Pro Tools build: the AAX SDK needs Avid approval and PACE/iLok signing, which
> can't be shipped with a free, open-source plugin.

## Usage

OrbitCab is a cabinet, not an amp. Place it after your amp sim, preamp, or amp-head
capture (e.g. Neural Amp Modeler): the amp shapes the gain, OrbitCab supplies the speaker
cabinet. Load an IR into a slot, optionally load a second, and blend the two.

## Build from source

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The first configure fetches and builds JUCE (pinned). Artefacts are written to
`build/OrbitCab_artefacts/`. See [docs/BUILD.md](docs/BUILD.md) for validation and packaging.

## License

[AGPL-3.0-or-later](LICENSE). You may use, modify, and redistribute OrbitCab; if you
distribute it, you must make the corresponding source available under the same license.
Third-party notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The "OrbitCab"
and "Darwin's Cat" names and logos are trademarks, not covered by the code license.

## Contributing

Bug reports and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

---

OrbitCab is part of the Felitronics line by [Darwin's Cat](https://darwinscat.com/orbitcab).
