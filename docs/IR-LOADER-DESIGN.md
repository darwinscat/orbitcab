# IR Loader — design (locked 2026-06-16)

> **OrbitCab is a purely *live* real-time plugin.** A companion web IR tool was used
> **only as a visual reference for the waveform (WF) canvas** — NOT as an architecture
> model. There is **no offline reprocess pipeline and no IR export** here. The web tool
> is an offline IR *shaper/exporter*; OrbitCab convolves the incoming guitar in real
> time. Don't import the web pipeline.

## Interaction principle — responsiveness > smoothness

**Every control is live and immediate, and audible artifacts during changes are
accepted (explicit user call):** switching the IR, dragging TRIM, and moving HPF/LPF
all take effect *now* — crackle / clicks / zipper noise during the change are fine.
**Do not add crossfades, ramps, or smoothing to hide them.**

- HPF/LPF are live filters *on the signal*, so they react instantly and can be
  click-free anyway — a bonus, not a requirement.
- IR-swap and TRIM are an instant impulse swap; **the click lives at the swap seam** —
  that's the accepted artifact.

One line stays: this is about *audible* glitches, not RT-safety. IR (re)loads run on
JUCE's loader thread, **never in `processBlock`**. "A click on swap" = fine; an
IO/alloc in the audio thread (xrun / hang) is a different failure and stays forbidden
(🔴). Same code, just off the audio thread — costs us nothing.

## Signal chain (live, RT-safe)

```
in → HPF → LPF → Convolution(IR) → Auto-level → Phase → Mix(dry/wet) → Output Gain → out
```

- Built with `juce::dsp::ProcessorChain`, modular (one stage per processor).
- **HPF / LPF / Phase / Mix / Output Gain are all APVTS params** — host-automatable,
  smoothed. `juce::dsp::Convolution` is the convolver.
- Keeping HPF/LPF as *live* filters (not baked into the IR like the web tool) is
  identical in tone — convolution is LTI, so `filter∘convolve = convolve∘filter` —
  but gives smooth host automation. That's the whole reason to do it live.

### Params (APVTS) — ranges mirror the web tool so a given IR sounds the same

| Param | Range / default | Notes |
|---|---|---|
| HPF (on + freq) | 30–400 Hz, freq def **80**, enable **off** | `juce::dsp::IIR` 2nd-order, Q=0.707 (Butterworth, 12 dB/oct). **Per slot.** |
| LPF (on + freq) | 2–12 kHz, freq def **7k**, enable **off** | same biquad. **Per slot.** |
| Phase | invert on/off, def **off** | polarity flip on the **wet** branch, *before* Mix — must invert only the convolved signal, never the dry, or it does nothing to the blend. Fixes cancellation / comb / thinness when blending dry+wet or stacking with a DI / 2nd mic |
| Mix (dry/wet) | 0–100 % wet, def **100** (= full wet, no blend) | blends the chain output (wet, post-Phase) with a **dry tap of the raw input** (taken *before* HPF). Recover attack/level. Per-slot **slider, hidden by default** (gear panel reveals it; auto-shown if a loaded state has mix ≠ 100 %). Plugin-specific — the web tool has no mix |
| Output Gain | −24…+24 dB, def **0** | final user trim *on top of* the Auto-level (below). |

### Defaults & where each control lives

**Defaults policy — *helpers on, shapers & view off*:** load an IR and you hear the
pure cab at matched loudness with nothing coloured.

- **On by default (invisible helpers):** Auto-level (`autoLevel`), HEAD trim (`headTrim`).
- **Off / opt-in (shapers + view):** HPF, LPF, Phase, TRIM, the Dry/Wet blend.

**HEAD trim is NOT a per-slot host param** — it's one **global, on-by-default session
setting**, the `headTrim` property on the APVTS state tree (rides save/load + A/B/C/D
snapshots + undo; not host-automatable). Toggled in the gear settings panel; flagging it
re-trims both slots off the audio thread (same reload path as TRIM). It snaps each IR to
its onset so dry/wet and A/B blends stay phase-aligned.

**View preferences are global, not per-session** — stored in the app-wide `PropertiesFile`
(per machine, every instance), owned by `AppPreferences`: `dryWetShown` (reveal the
Dry/Wet sliders, default **off**) and `spectrumOn` (the faint analyser, default **on**).
The gear panel captions these as *"this computer"* vs the session-scoped HEAD trim.

## Auto-level — wet→dry loudness match

A cab IR drops a guitar a lot (measured ~−17 dB on a real DI, and worse: the cab
removes the DI's dominant high-frequency energy). That can't be predicted from the
IR alone — it depends on the signal spectrum (an IR-energy / √Σh² estimate measured
*backwards* for guitar). So leveling is done **live**:

- Continuously measure the RMS of the **dry** input and the **wet** (convolved)
  signal with slow one-pole followers (TC ~150 ms), and apply a makeup gain of
  `√(dryMS / wetMS)` to the wet branch **before Mix**.
- Silence-gated (won't chase the noise floor), clamped [−24, +36] dB, smoothed — no
  pumping, no zipper.
- On by default; an **Auto Level** toggle (param `autoLevel`) turns it off to hear the
  raw cab — followers keep running while off, so re-enabling snaps back instantly. The
  IR is loaded **as-is** (`Normalise::no`); the match does all leveling.
- **This is what makes Mix usable:** wet and dry sit at the same loudness, so the
  blend is meaningful instead of a loud-dry / "mosquito-wet" imbalance.
- Output Gain is the final manual trim after the match.

## IR source & loading

- **Browse a folder:** pick a directory → list its `.wav` IRs → arrow ↑↓ to select.
- **Bundled CC0 packs** (Brutal/Emerald) appear in the **same list**,
  grouped by cabinet.
- **Selecting an IR = it becomes active immediately:**
  `convolution.loadImpulseResponse(File…)` loads on JUCE's **background thread** and
  swaps **atomically**. The WF redraws on select. (No separate "preview then commit"
  — auditioning *is* loading.)
- **Audition = live input only.** No bundled DI, no offline render. Silence in →
  silence out. Loop a section in the DAW to audition a row of IRs.
- **A click/glitch on IR swap is acceptable → no crossfade.** Keeps it simple.
  - ⚠️ This does **not** relax the 🔴 RT rule. The disk read + allocation happen on
    JUCE's loader thread, **never in `processBlock`**. A cosmetic click on swap is
    fine; an IO/alloc in the audio thread (xrun / priority inversion) is not.

## TRIM (IR length) — the only control that touches the IR

- A draggable handle on the WF cuts how much of the IR feeds the convolution
  (shorter = tighter, less tail).
- Dragging it = **reload a truncated IR** off-thread + atomic swap — same path as an
  IR change, and **live while you drag**: coalesce to the latest drag position via a
  generation counter and drop stale reloads (no artificial debounce delay). A short
  **~2 ms fade-out** on the cut avoids a *constant* hard click in the tail; transient
  crackle while dragging is fine and expected.
- Everything else (HPF/LPF/Phase/Mix/Gain) is the live chain and never re-touches the IR.

## Waveform (WF) canvas — look borrowed from the web tool

- Draws the IR waveform.
- Overlays: **HPF/LPF indicators** + the **draggable TRIM boundary**.
- The web canvas is the *look* reference only.

## Explicitly OUT of scope (do not build)

- ❌ IR export / save-processed-IR — that's the web tool's job.
- ❌ Offline reprocess pipeline (mono-convert / resample-to-target / bake filters into IR).
- ❌ Crossfade between IRs on swap.
