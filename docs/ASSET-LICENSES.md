# Asset licenses

> Code license ≠ content license. This file is the per-asset ledger for every
> bit of **audio content** we bundle, ship, or use to test/demo OrbitCab. When we add an
> IR or a sample, add a row here with its license + source. The user-facing
> `THIRD_PARTY_NOTICES.md` is generated from this + the SDK notices.

## Impulse responses (bundled in the plugin)

| Pack | License | Source / proof | Status |
|------|---------|----------------|--------|
| **Jesters Brutal Pack 1.0** | **CC0** (public domain) | Pack handbook PDF: *"LICENSED 2022 UNDER: CC0 … put their works into the worldwide public domain … completely free to use in your private and commercial Projects."* | ✅ cleared to bundle + redistribute (commercial OK) |
| **Jesters Emerald Pack** | **CC0** (public domain) | Same Jesters series as the Brutal pack, released under the same CC0 grant. | ✅ cleared to bundle |

CC0 imposes **no attribution requirement**, but we still credit the source as a
courtesy in `THIRD_PARTY_NOTICES.md`.

**Embedded in the binary (`juce_add_binary_data`) — all CC0:**

| Files | Pack | Rate / format | Cab |
|------|------|---------------|-----|
| `resources/ir/01-…` … `15-…` (15 IRs) | Brutal | 48 kHz, 24-bit, mono | mod. Behringer BG412S — V30 / Rockdriver Jr / DV-77 |
| `resources/ir/16-…` … `21-…` (6 IRs) | Emerald | 48 kHz, 24-bit, mono | Marshall 1960AX — Greenback G12M 25W |

21 IRs total (~2.8 MB embedded). The **48 kHz** variant of each is bundled — the
convolver resamples the IR to the host rate anyway (`loadImpulseResponse`), so one
rate is enough and halves the binary cost.

## Reverb impulse response (bundled in the plugin)

One spring-reverb IR drives the in-amp reverb (post-EQ, pre-poweramp + cab). Sourced from
[Freesound](https://freesound.org) — see `resources/reverb-ir/README.md`.

| Pack | License | Source / proof | Status |
|------|---------|----------------|--------|
| **Tube spring** (1 IR) | **CC0** (public domain) | Freesound upload by *derickgtwk* (528139, "Fisher K10 Stereo"). *"copy, modify, distribute and perform … even for commercial purposes, all without the need of asking permission."* | ✅ cleared to bundle + redistribute (commercial OK) |

**Embedded in the binary (`juce_add_binary_data`), mono, 48 kHz / 24-bit:**

| Files | UI name | License | Source unit |
|------|---------|---------|-------------|
| `resources/reverb-ir/01-tube.wav` | Tube (`REV`) | CC0 | Fisher K-10 tube spring reverb (1960s) |

The reverb wet level (a fixed 0.04) is calibrated **in code**, not baked into the IR. CC0 needs no
attribution — credited as a courtesy in `resources/reverb-ir/README.md` + `THIRD_PARTY_NOTICES.md`.

## Fonts & brand (bundled in the plugin)

| Asset | License | Source / proof | Status |
|-------|---------|----------------|--------|
| **Michroma** (`resources/fonts/Michroma-Regular.ttf`) | **SIL Open Font License 1.1** | Google Fonts / github.com/googlefonts/Michroma-font; OFL text archived at `resources/fonts/Michroma-OFL.txt` | ✅ cleared to embed in the binary (OFL §1 allows bundling in software, incl. closed-source) |
| **Darwin's Cat logo** (`resources/brand/logo-darwinscat.svg`) | © Darwin's Cat (own mark) | First-party brand asset | ✅ our own |
| **OrbitCab app icon** (`resources/icon/OrbitCab.svg` + `OrbitCab-1024.png`) | © Darwin's Cat — reserved trademark (not under the code license) | First-party brand asset — the orbit mark rendered as the bundle/executable icon (`ICON_BIG`) | ✅ our own |

The **orbit "O" mark** in the header is drawn programmatically (no asset); the same mark is
committed as an SVG + rendered PNG under `resources/icon/` purely to serve as the app/bundle
icon fed to JUCE's `ICON_BIG`.

The Michroma **OFL** only forbids selling the font *by itself* and reserving the name —
embedding it in the plugin is fine; credit it in `THIRD_PARTY_NOTICES.md`.

## Test / demo audio (NOT shipped in the plugin)

Dry-DI guitar recordings used during development to A/B the cab sound are **not**
embedded in OrbitCab — they're inputs run *through* the plugin while developing, owned
by their authors. If any audio ever ships as a bundled demo, add a row here with its
license + source first.
