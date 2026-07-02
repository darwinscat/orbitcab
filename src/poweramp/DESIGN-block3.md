# Block 3 — the "FEEL" layer: Sag + Presence/Depth — DESIGN (locked)

Synthesised from a 3-way consilium: codex `gpt-5.5`/xhigh + deepseek-v4-pro/max + an independent
blind-pass Claude agent. Strong consensus. Implementation spec for block 3.

> AS-BUILT deltas will live in [STATUS.md](STATUS.md).

## Scope (all three agree)
Ship **Sag + Presence + Depth**. **Defer virtual speaker-load + output-transformer LF-sat to Block 4.**
Rationale (blind-pass, best framing): it's a *dependency order*, not a preference — the NFB reference
node (output/OT) doesn't exist yet, and a *static* virtual load in front of a user cab IR (which already
imposes the speaker magnitude curve) is double-filtering / an A-B trap. Sag + NFB-voicing form one
coherent, unconditionally-stable, **zero-added-latency**, min-phase scope. Block 4's load then ties INTO
the NFB node built here. **Superset invariant: Sag=Presence=Depth=0 ⇒ byte-identical to block 2** (honest
A/B + backward compat). Latency stays exactly **31** (everything is memoryless-gain / per-block coeff /
min-phase IIR — no FIR, nothing reportable).

## Sag DSP
- **Detector — FEEDFORWARD on rectified demand, mono-linked.** Detect on `|G·x|` (post-Drive, pre-upsample
  baseband), mono-summed/max across channels (shared power supply → ONE detector, ONE droop, applied to
  both channels). Feedforward (not post-output feedback) is deliberate: (a) cannot self-oscillate
  (stability for free), (b) gives *touch* — the pick transient punches through at full headroom for the
  first few ms before the supply droops, then squishes.
- **Dual-time-constant one-pole (hand-rolled, a pure `SagEnvelope` unit):**
  - fast attack **τ ≈ 5–20 ms** (rectifier R + first filter-cap depletion — how fast B+ collapses),
  - slow recovery **τ ≈ 150–400 ms** (reservoir recharge — the *bloom*; slow recovery is the whole game —
    fast recovery = a pumping compressor).
  - branch: fast coeff when demand rises, slow coeff when it falls.
- **Modulation — a shrinking "B+ rail" around the shaper:** `y = s · shaper(u / s)`, `s = 1 − droop`,
  `droop ∈ [0, dmax]`. Small signal `u/s≈u`, gain≈unity → clean stays clean. Large signal: pushed deeper
  into the curve (`1/s`) AND ceiling lowered (`·s`) → **earlier breakup + level drop = tube squish**, not a
  clean VCA duck. **Cost: fold `1/s` into the existing per-sample Drive input ramp and `s` into the existing
  per-sample Output ramp — NO new per-OS-sample multiplies.** Per-sample `s` computed in the baseband
  pre-shaper loop, stashed in a preallocated `float[maxBlock]`, reused in the output loop (sub-block
  resolution → the 5 ms attack survives 512-sample blocks).
- **Bias shift (secondary, per-block character):** as B+ droops the class-AB operating point shifts →
  crossover changes. Modulate `Vb`(PP)/`bSE`(SE) by a small `+kBias·droop` via the existing per-block
  `stage.configure()`. This is the "gets gnarlier as it sags" *character* (reads tube, not compressor).
  Small (±0.05 on Vb), clamped so it can't invert bias. `droop=0` ⇒ exact block-2 kernel.
- **Sag knob ∈ [0,1]** scales `dmax` (~0.3–0.35 ≈ −3–4 dB headroom loss) + lengthens recovery.
- **Per-tube `sagStiffness`** (fixed in `kTubeVoicings`): 6L6/KT88 stiff (usually SS-rectified), EL84/EL34
  spongy (chime/vintage). A given Sag setting *feels* right per tube.
- Baseband detection + gain; per-block bias. **Zero added latency → total stays 31.**

## Presence + Depth — NFB voicing (defer the true loop)
- The net *linear* effect of a stable global NFB loop is a frequency-dependent gain = a filter. Model it as
  a **min-phase shelf pair on the output node** (post-downsample, pre-drive-comp — the conceptual NFB tap),
  and make it **dynamic** so it reads as feedback, not tone control.
- **Presence** = high-shelf, corner **~2–3 kHz** (per-tube-fixed), **0..+6 dB**. **Depth** = low-shelf,
  corner **~90–120 Hz**, **0..+6 dB**, with a small resonance/Q bump only at the top of the range.
- **The "feels like feedback" trick:** in a real amp loop gain collapses as the output saturates → NFB
  *releases* → brighter + looser when pushed. Reproduce by **conditioning the effective shelf gains on the
  sag/drive "open" amount** (reuse the sag `droop` as the "how-hard-pushed" signal — elegant coupling: when
  it sags, it also opens): `presenceDb_eff = presenceKnob·presenceMax·(1 + kOpen·openAmt)`, same for Depth,
  `openAmt ∈ [0,1]` from `droop`. **Clamp** (a brightness runaway is the one instability risk in an otherwise
  linear block). Quiet = tight/controlled; dug-in = opens/loosens.
- Filters: `felitronics::eq::Svf` (TPT zero-delay, min-phase → zero latency) if suitable, else a pure
  `ToneShelf` one-pole unit. Per channel (dual-mono). Gains smoothed (~10 ms).
- **User knobs = presence/depth gains (0..1); corners, presenceMax/depthMax, kOpen, LF-Q = per-tube-fixed.**

## Params (minimal diff)
- `cab::TubeParams` (POD, JUCE-free) += `float sag=0, presence=0, depth=0;` ([0,1] amounts, like `autoComp`).
  Sanitize each with `std::isfinite`+clamp at the `setParams` gate (block-2 pattern — a non-finite param must
  not poison the new IIR/envelope state).
- `TubeVoicing` (TubeKernel.h) += per-tube-fixed: `sagStiffness, sagFastMs, sagRecoveryMs, sagBiasDepth,
  presenceHz, presenceMaxDb, depthHz, depthMaxDb, nfbOpen`. Keep `evenLeak=0`.
- APVTS: 3 `AudioParameterFloat` (`tubeSag`/`tubePresence`/`tubeDepth`, 0–100 %) at `kParamVersion`; cache
  ptrs, read in `packParams()`. No latency depends on them → **no new listeners**.

## RT-safety + embedded
All new state (sag detector, `float[maxBlock]` s-scratch, 2×shelf/channel, smoothers) allocated in
`prepare()`; nothing allocates in `process()`. std + felitronics only, no JUCE in the stage; behind the
existing pImpl heap (MSVC 1 MB-stack rule untouched). Guard the NEW state every block: denormal flush +
finite-guard on the sag envelope + shelf state (like the block-2 DC-block flush); clamp `droop` and the
effective shelf dB and `kOpen` term so a hostile param/transient can't blow the dynamic-opening. drive-comp
(`autoComp`) is computed from `slopeAtZero(gCur)` (drive only, NOT sag) — at small signal `s≈1` no conflict;
at large signal sag rides on top of the fixed per-block comp and stays audible by design (don't "fix" it).

## Maniacal test plan (finalised by a dedicated test-design consilium, block-2 style)
Modules unit-tested in isolation: **`SagEnvelope`** (dual-TC: demand → droop; attack/release timing,
compression law, no-pump), the **shelves** (FR: corner+dB). Integration + regression:
- **Superset/off:** Sag=Presence=Depth=0 ⇒ == block-2 (identity anchor).
- **Sag:** step/burst dual-TC timing (fast attack, slow recovery) + transient punch-through; amount
  monotonicity; touch-vs-level (GR + THD rise with level); NOT-pumping (staccato-gate: gain has no strong
  component at the chord rate); bias-shift character (crossover/2nd-harmonic shift); per-tube stiffness
  ordering (EL84>EL34>6L6>KT88); feedforward stability (Sag=1 + full-scale/NaN → no oscillation, finite).
- **Presence/Depth:** FR delta (corner+dB); zero-amount flat; **drive-interaction "feels like feedback"
  gate** (effective HF/LF gain increases with drive/sag, monotonic); depth-looseness bounded; worst-case
  stability (max drive+presence+depth, long hostile signal → bounded, no NaN).
- **Block-2 regression:** latency == 31 invariant; RT no-alloc (operator-new counter == 0); block-size
  determinism (bit-exact); alias floor unchanged (~-73 dBc guitar range); pathological numerics recover;
  mono-linked sag stereo (L-only still sags R); zipper bounded.

## 80/20 + top risks
80/20: sag (feedforward dual-TC → rail-shrink folded into existing ramps + small per-block bias) is the
highest-value add — most of the "power-amp feel" for a fraction of the effort, zero new per-OS cost,
unconditionally stable. Presence/Depth as drive-conditioned shelves deliver the voicing AND the dynamic
"loosening." 3 knobs, +0 latency, provably stable, large perceptual return.
- **R1 — sag becomes a pumping compressor** → slow feedforward recovery + soft knee + transient
  punch-through + bias-shift for character + the staccato-gate anti-pump metric as a hard gate.
- **R2 — dishonest A/B ("louder/duller wins")** → the all-off ⇒ block-2 identity anchor + level-matched
  listening + document that sag reducing level *is* the effect.
- **R3 — new IIR/feedback state regresses stability/NaN** → feedforward-only sag (no loop), clamp
  droop/shelf-dB/kOpen, flushDenormals + finite-guard every block, worst-case combined-settings test.
