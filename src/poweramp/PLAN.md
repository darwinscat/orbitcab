# OrbitCab — Tube Power-Amp stage (white-box) — BUILD PLAN

> Status: **prototype / isolated**. Branch `feat/tube-poweramp` (worktree).
> Everything new lives under `src/poweramp/` (namespace `cab::poweramp`). The branch
> stays releasable: the stage is wired into `CabEngine` **behind an off-by-default flag**,
> minimal diff to existing files, until a working version exists.
>
> This is a technical build-spec. Strategic/backlog framing belongs in the **private**
> planning repo (`landings-darwinscat/.../orbitcab/_planning/`), not here.

## Why this exists (1 paragraph)
Two Notes' "TSM Power Amp" is a tube power-amp stage *before* the cab. Today OrbitCab's
power-amp seam is a **NAM capture** (`cab::AmpStage`) — black-box, knobs don't change the
modeled amp. Goal: add a **white-box analytic tube power amp** as a *second mode* in the
same seam, with **knobs that actually work** (Tube / Topology / Sag / Presence / Depth).
Not a clone of TSM — our own. Net result mirrors (and can exceed) Two Notes' own hybrid
**TSM-Ai** = AI captures **+** engineered power-amp: we already ship the neural half (NAM),
this adds the analytic half.

## Where to build it (decision)
**Prototype here in OrbitCab → extract to `felitronics-core` once stable.**
- New analytic DSP (sag, NFB loop, virtual load, PP topology, tube voicings) iterates
  fastest against the real plugin/chain — not through the pinned `felitronics-core v0.1.2`
  hybrid-fetch (every tweak would mean bumping a tag/checkout).
- The two building blocks we already have in core (`saturation`, `oversampling`) are just
  **linked**, not copied.
- When the DSP is proven + golden-tested, **promote `cab::poweramp` → a new core module**
  (`felitronics::poweramp` / `tubestage`), same pattern as `SpectrumTap → felitronics::analysis`.
  OrbitCab then consumes it and `src/poweramp/` is deleted.

## Isolation contract (keep the branch clean)
- All new code under `src/poweramp/` only.
- Existing-file edits limited to: (a) `CMakeLists.txt` link + sources, (b) one seam call in
  `CabEngine` guarded by a flag, (c) one param + APVTS line, (d) latency/DSP-load plumbing.
- Off by default. `cab::AmpStage` (NAM) power-amp path untouched and remains the default.
- Every block ships with a **headless golden test** (mirror `tests/CoreConvolver*` style).

---

# v1 — White-box analytic tube power amp (permissive-only)

## Position in the chain
```
… → PREAMP (NAM) → AMP EQ (teq) → [POWERAMP seam] → cab IR → …
                                        │
                                        ├─ mode "Capture": cab::AmpStage (NAM)   ← exists, default
                                        └─ mode "Tube":    cab::poweramp::TubePowerAmp  ← NEW
```
Cab IR stays **after** the power amp (it is acoustic/mic LTI response, not electrical load).

## Internal signal chain of `TubePowerAmp`
```
in
 → Virtual load / impedance pre-EQ   (LF resonance peak + HF inductive rise)      [IIR / felitronics lineareq]
 → Sag envelope → headroom/bias mod  (fast rectifier τ + slow recovery τ)         [NEW]
 → Oversample ×N (2/4/8)                                                          [felitronics::oversampling, LINK]
 → PP/SE nonlinear core              (asym waveshaper, tube voicing, crossover)   [felitronics::saturation base + NEW PP]
 → Output-transformer block          (DC-block + LF core sat + HF rolloff)        [NEW, simple]
 → Downsample ×N
 → NFB loop: Presence(HF)+Depth(LF)   (referenced from post-OT node, gain-clamped) [NEW]
 → out  → (existing) cab IR
```
RT-safe: all alloc/state in `prepare()`; nothing in `process()` allocates/locks/throws;
in-place on planar `float* const*` (matches `Saturator`/`PolyphaseOversampler`/`AmpStage`).

## Controls (params, smoothed)
`Drive`, `Output`, `Tube {6L6 / EL34 / EL84 / KT88}`, `Topology {PP-AB / SE-A}`, `Sag`,
`Depth`, `Presence`, `Damping/Contour`, plus `OS quality {2× eco / 4× default / 8× HQ}`.
Tube types = **coefficient/voicing presets** (transfer + headroom + transformer voicing),
not brand claims.

## Dependencies
- **Link (already in core, permissive):** `felitronics::saturation`, `felitronics::oversampling`,
  optionally `felitronics::lineareq` for the impedance/NFB filters.
- **Available (JUCE):** `juce::dsp::IIR`, `SmoothedValue`, `Oversampling` (fallback).
- **Optional later (BSD-3, permissive):** `chowdsp_wdf` if analytic IIR proves too coarse
  for OT / NFB loop.
- **License rule:** v1 stays **permissive-only** so the combined binary remains clean AGPL.
  GPL refs (**SwankyAmp** for sag/crossover, Proteus, BYOD) are **read-only** references —
  do NOT link/copy into the tree. Do NOT bundle model/IR files (TONE3000 not redistributable).

## Task order (each independently testable)
1. **Scaffold.** `src/poweramp/` + pass-through `TubePowerAmp` wired at the seam behind an
   off-by-default flag; CMake links `saturation`+`oversampling`. **Test:** bypass == identity (null).
2. **Nonlinear core.** PP/SE asymmetric waveshaper @4× OS + tube voicing coeff sets.
   **Test:** even-harmonic cancellation in PP vs even-rich SE; THD-vs-drive curve; alias floor < −80 dB.
3. **Virtual load pre-EQ.** **Test:** magnitude response matches target LF peak + HF rise.
4. **Sag.** envelope → headroom/bias, two time constants. **Test:** step-response compression
   (attack ~5–30 ms, release ~80–500 ms); no zipper/pumping.
5. **Output-transformer block.** DC-block + LF sat + HF rolloff. **Test:** LF saturation onset, HF corner.
6. **NFB loop.** Presence(HF)+Depth(LF) from post-OT node, loop-gain clamp. **Test:** presence
   sweep == HF shelf change; stability under high drive.
7. **Integration.** APVTS params; OS latency → `setLatencySamples()`; DSP-load meter slot;
   A/B mode switch vs NAM power-amp (click-free).
8. **Validation.** level-matched bypass; listening + null vs NAM; CPU budget (target a few % @4× OS).
9. **Extract.** promote `cab::poweramp` → new `felitronics::poweramp` module; OrbitCab consumes it;
   delete `src/poweramp/`.

## RT / pitfalls checklist (from research)
- 4× OS default (8× for cold-bias crossover / hard settings); IR LPF does **not** undo folded aliasing.
- Load model **before** the nonlinearity; cab IR after. Never put cab IR before the power amp (double-filter).
- Amp is **mono** — decide stereo explicitly (dual-mono vs linked sag) before step 7.
- ADAA optional for the *memoryless* shaper; oversampling is the safe baseline for the *stateful* loop.
- Smooth every coefficient; clamp NFB loop gain; level-match on bypass (louder always "wins" in tests).

## Definition of done (v1) / ready-to-extract
- All 9 tasks green; golden tests pass headless on macOS + Windows CI.
- "Tube" mode A/Bs cleanly against "Capture" mode; knobs audibly + measurably do what they say.
- CPU within budget; no RT violations (no alloc/lock in `process`).
- Then, and only then: extract to core.

---

# v2 — Parametric / knob-conditioned neural power amp (HYPOTHESIS)

## ⚠️ READ FIRST — do not execute as written
**v2 must be fully re-scoped AFTER v1 ships.** Once we have measured + listened to the
white-box stage, revisit every assumption below with the new knowledge:
- What does white-box v1 **already nail** (so neural adds nothing)?
- Where does it specifically **fall short** (the only axes worth a neural model)?
- Is parametric-neural even **needed**, given CPU is shared with the convolver?
- Is the **capture dataset** (power-amp-into-reactive-load across the knob space) actually feasible?

This section is a **direction to test against**, not a backlog to grind. Do not implement
blindly — treat it as a decision gate, not a plan.

## Hypothesis sketch (to revisit)
- **PANAMA-style** (ETH-DISCO, MIT) parametric model conditioned on
  `{Drive, Master, Presence, Depth, Sag, Tube, Load}`, trained from captures of a real power
  amp into a **reactive** load across the knob space.
- **Runtime via RTNeural** (BSD-3) behind the existing abstraction
  `felitronics::neural::NeuralStage<Backend>` — no new seam, just a new backend.
- **White-box v1 stays** as the deterministic / eco mode **and** as the null-reference for
  validating the neural model (error metrics + listening).

## Open problems (decide at the gate)
- Dataset explosion: training WAVs multiply with knob count → which knobs are worth conditioning?
- Isolating a power-amp-into-reactive-load capture (vs full-rig captures that bake in the cab).
- Knob-interpolation smoothness (zipper/discontinuity between conditioned points).
- CPU vs the convolver on a shared budget; model-file licensing/provenance.
- **Gate:** pursue v2 only if v1 demonstrably cannot reach the target feel on specific, named axes.

## Pointers (permissive, for when the gate opens)
PANAMA (MIT) · RTNeural (BSD) · NeuralAudio (MIT) · NeuralSeed (MIT, multi-knob proof) ·
`felitronics::neural::NeuralStage<Backend>` (our abstraction, already present).
Dependency-review ledger: `orbit-poweramp-oss-research.md` (license matrix + open questions).
