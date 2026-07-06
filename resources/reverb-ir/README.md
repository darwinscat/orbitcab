# Spring-reverb impulse responses

Four **spring-reverb** IRs bundled into OrbitCab's in-amp reverb. The reverb sits
**after the tone stack (EQ) and before the poweramp + cab**, exactly like a real
amp's built-in spring tank — so the reverb is driven by the amped guitar, summed
with it, and then coloured by the poweramp and speaker (cab IR). It is *not* a clean
stereo studio reverb; it's the boing that lives inside the box.

The reverb runs on the **mono amp lane**, so every IR here is **mono** — the left
channel of the source is taken as-is (no L+R downmix, which would comb-filter the
decorrelated tail).

## The reverbs

| UI name | File | Character | Source unit |
|---------|------|-----------|-------------|
| **Vintage** | `01-vintage.wav` | Lo-fi 1960s garage / dub boing | A 1960s spring tank |
| **Smooth**  | `02-smooth.wav`  | Refined, desk-fed, even tail   | Roland RE-301 Chorus Echo (1977) spring, through a Mackie 8-bus |
| **Tube**    | `03-tube.wav`    | Warm, valve-driven             | Fisher K-10 tube spring reverb (1960s) |
| **Studio**  | `04-studio.wav`  | Clean, sine-sweep deconvolved  | Fostex 3180 studio spring (1980s) |
| **Small**   | `05-small.wav`   | Plate — tight, short           | American stainless-steel analog plate |

## Sources & licenses

**CC0** (public domain — no attribution required, credited as a courtesy): the 4 springs.
**CC-BY 4.0** (Attribution **required**, IR **modified**): **Small** plate (`recordinghopkins`) —
flows into `THIRD_PARTY_NOTICES.md`.

| File | Original | Author | License | Freesound |
|------|----------|--------|---------|-----------|
| `01-vintage.wav` | "60s Spring Reverb" | **derickgtwk** | CC0 | https://freesound.org/people/derickgtwk/sounds/530378/ |
| `02-smooth.wav`  | "Roland RE-301 Spring Reverb Impulse" | **0e0** | CC0 | https://freesound.org/people/0e0/sounds/131034/ |
| `03-tube.wav`    | "Fisher K10 Stereo" | **derickgtwk** | CC0 | https://freesound.org/people/derickgtwk/sounds/528139/ |
| `04-studio.wav`  | "Fostex 3180 (Impulse Response)" | **KenMix** | CC0 | https://freesound.org/people/KenMix/sounds/643937/ |
| `05-small.wav`   | "Small Plate 05" | **recordinghopkins** | **CC-BY 4.0** | https://freesound.org/people/recordinghopkins/sounds/175307/ |

## Processing applied to the source files

Each source was rendered to the bundled asset with ffmpeg:

- **Mono** — left channel only (`pan=mono|c0=c0`); no L+R sum.
- **48 kHz / 24-bit WAV PCM** — one rate is enough; the convolver resamples the IR to
  the host rate on load. (Fisher K-10 was 96 kHz; the others 44.1 kHz.)
- **Trimmed to ≤ 3.5 s** with a **100 ms raised-cosine fade-out** — a spring tail is
  long-decayed noise past that, and it keeps the IR under the convolver's 4 s budget.
- **Peak-normalized to −1 dBFS** — so the runtime skips its guitar-band RMS
  normalization (which is for cab IRs, not reverb) and the Reverb Mix knob sets level.

The canonical per-asset license ledger is `docs/ASSET-LICENSES.md`; this file is the
human-facing source note that the user asked to keep next to the audio.
