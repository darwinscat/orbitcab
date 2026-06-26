<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# Preamp NAM models (local-only)

This folder holds the **Neural Amp Modeler (`.nam`) preamp captures** that the build embeds as
**factory** models for the editor's **PREAMP** stage — the second neural stage, which runs *in front
of* the poweramp: **input → PREAMP → POWERAMP → cab**.

**The `.nam` files here are gitignored — they are NOT in the public repo, on purpose.** The amp-stage
*code* (`cab::AmpStage`, NAM core, the UI, rate-match) is open-source AGPL and ships freely. The model
*content* does not: captures can be of third-party / commercial gear, so — like the official Neural Amp
Modeler plugin — the open-source build ships **without** bundled captures. You supply your own.

A fresh clone builds fine with this folder empty (this README is the always-present placeholder source
for the binary-data target); the PREAMP feature simply hides itself when there are no models to load.

## Two sources, one selector

The PREAMP selector merges two libraries (kept separate from the poweramp's):

* **Factory** — `.nam` files in *this* folder, embedded into the build under a dedicated
  `PreampBinaryData` namespace so they never mix with the poweramp's factory models.
* **User** — a per-machine, system-wide folder shared by every plugin instance, managed from the gear
  panel (Add… / Remove). On macOS: `~/Library/Application Support/Darwin's Cat/OrbitCab/Preamps/`.

## Naming → channel + gain + boost

A preamp has more selectable dimensions than the poweramp. Each is an OPTIONAL whole-word token in the
filename, dropped from the shown name; a control only appears when the current choice has ≥2 values for
that dimension:

```
Voltage ch2 12h boost.nam → name "Voltage", channel 2, gain 12h, boost on
Voltage 16h.nam           → name "Voltage", gain 16h         (only a gain slider)
Studio Pre.nam            → name "Studio Pre"                (just the name)
```

* **`chN`** (N = 1..3) → the channel switch (a 3-way mode toggle).
* **`<N>h`** (e.g. `12h`, clock position 7h…17h) → the **gain** slider.
* **`boost`** → boost on (absent = off); shown as a toggle when both an on- and off-capture exist.

Drop files here and rebuild — CMake re-globs this folder (`CONFIGURE_DEPENDS`) and embeds whatever's
present. All factory captures are 48 kHz (the rate-matcher runs them at native rate on any host SR).
Whatever you put here stays on your machine — it never leaves in a commit.
