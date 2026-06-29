<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# Preamp NAM models

Neural Amp Modeler (`.nam`) preamp captures embedded as **factory** models for the editor's
**PREAMP** stage — the neural stage in front of the tone EQ and poweramp:
**input → PREAMP → tone EQ → POWERAMP → cab**.

The build bundles the author's own **V4KR** captures (below). Drop more `.nam` files into this folder
and rebuild — CMake re-globs it (`CONFIGURE_DEPENDS`) and embeds whatever's present. All factory
captures are 48 kHz (the rate-matcher runs them at the host's sample rate).

## V4KR — capture provenance

`V4KR` is a **two-channel valve preamp** — a **red** high-gain channel and a **green** lower-gain
channel — captured across its gain range: 11 captures per channel at gain clock positions 7h…17h.

**How it was captured** (electrical loop — no mic, no room):

```
Focusrite Clarett+ 8Pre (line OUT)
  → Radial Engineering Pro RMP (passive reamp: line → Hi-Z)
  → V4KR preamp (one channel; ALL knobs at noon, only GAIN rotated per capture; built-in cab-sim OFF)
  → Focusrite Clarett+ 8Pre (line IN)
```

- Only **Gain** is swept (7h…17h); every other knob stays at **noon**.
- The **tone stack is captured flat** and rebuilt in the `teq` software EQ — not baked into the model.
- The unit's **built-in cab-sim is OFF**; OrbitCab supplies the cabinet via its own IR.

## Two sources, one selector

The PREAMP selector merges two libraries (separate from the poweramp's):

* **Factory** — `.nam` files in *this* folder, embedded under a dedicated `PreampBinaryData` namespace
  so they never mix with the poweramp's factory models.
* **User** — a per-machine folder shared by every plugin instance, managed from the gear panel
  (Add… / Remove). On macOS: `~/Library/Application Support/Darwin's Cat/OrbitCab/Preamps/`.

## Naming → name / channel / gain / boost

Each dimension is an OPTIONAL whole-word token in the filename, dropped from the shown name; a control
appears only when the current choice has ≥2 values for that dimension:

```
V4KR-red-9h.nam       → name "V4KR",  channel "Red" (glows red),  gain 9h
V4KR-green-12h.nam    → name "V4KR",  channel "Green",            gain 12h
Amp ch2 16h boost.nam → name "Amp",   channel 2 ("Ch 2"), gain 16h, boost on
Studio.nam            → name "Studio"                             (just the name)
```

* **channel** — either **`chN`** (N = 1..4) → a numbered switch, or a **colour word**
  (`red` `green` `blue` `yellow` `orange` `purple` `white`) → a channel that **glows in that colour**.
  `chN` and colours don't mix within one model.
* **`<N>h`** (e.g. `12h`, clock position 7h…17h) → the **gain** dial.
* **`boost`** → boost on (absent = off); a toggle appears when both an on- and off-capture exist.
