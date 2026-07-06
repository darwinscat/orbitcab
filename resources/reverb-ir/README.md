# Spring-reverb impulse response

One **spring-reverb** IR drives OrbitCab's in-amp reverb. It sits **after the tone
stack (EQ) and before the poweramp + cab**, exactly like a real amp's built-in spring
tank — driven by the amped guitar, summed with it, then coloured by the poweramp and
speaker (cab IR). It is *not* a clean stereo studio reverb; it's the boing inside the box.

It runs on the **mono amp lane**, so the IR is **mono** — the left channel of the source
(no L+R downmix, which would comb-filter the decorrelated tail).

## The reverb

| UI name | File | Character | Source unit |
|---------|------|-----------|-------------|
| **Tube** (the `REV` knob) | `01-tube.wav` | Warm, valve-driven | Fisher K-10 tube spring reverb (1960s) |

This is the only reverb IR bundled.

## Source & license — CC0

**CC0** (public domain): free to copy, modify, distribute and perform, even commercially,
with no attribution required (credited as a courtesy).

| File | Original | Author | License | Freesound |
|------|----------|--------|---------|-----------|
| `01-tube.wav` | "Fisher K10 Stereo" | **derickgtwk** | CC0 | https://freesound.org/people/derickgtwk/sounds/528139/ |

## Processing

Rendered with ffmpeg: **mono** (left channel, `pan=mono|c0=c0`), **48 kHz / 24-bit WAV
PCM** (the convolver resamples to the host rate on load), ~3.5 s with a 100 ms fade-out,
**peak-normalized to −1 dBFS**. The wet level (a fixed 0.04) is calibrated in code, not
baked into the IR; the `REV` knob sets the amount (0 = off).

The canonical per-asset license ledger is `docs/ASSET-LICENSES.md`; this file is the
human-facing source note kept next to the audio.
