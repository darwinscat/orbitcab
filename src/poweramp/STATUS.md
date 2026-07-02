# Tube Power-Amp — build status log

## Block 1 — Scaffold + integration — ✅ DONE (validated)
Branch `feat/tube-poweramp`, base = `main` `288e91e` (de-JUCE convolution + felitronics-core v0.1.2).
Protocol per RULES.md: consilium (codex `gpt-5.5`/xhigh + deepseek-v4-pro/max) → Claude grounding
self-call → implement → Claude after-review (3 agents). All green.

### What landed
- `src/poweramp/TubePowerAmp.{h,cpp}` — JUCE-free pImpl, **no-op passthrough** (the stable seam the
  future white-box DSP grows behind, without touching the header).
- `src/poweramp/PowerAmpRouter.{h,cpp}` — the SINGLE poweramp seam: routes `ampOn`/`mode` →
  `off | capture(NAM) | tube`, with a 30 ms constant-sum crossfade on a live **capture↔tube**
  switch. The `off` master gate stays an **instant hard switch** (= legacy `if (p.ampOn)`).
- `cab::Params` — `+ enum PowerAmpMode {capture,tube}` + a field; `ampOn` kept as the master gate.
- APVTS — `+ AudioParameterChoice "ampMode" {Capture,Tube}` (the project's FIRST choice param),
  wired in `packParams` + `updateLatency` + a latency-refresh listener.
- `CMakeLists.txt` — poweramp sources in all 4 targets that compile `CabEngine.cpp`; new pure
  golden target `orbitcab_poweramp_scaffold_golden`.
- `tests/PowerAmpScaffoldGolden.cpp` — JUCE-free bit-exact passthrough golden.

### Decisions / deviations from the consilium sketch
- **Enum lives in `core/Params.h`** (not a separate `PowerAmpTypes.h`) to avoid a core→poweramp
  dependency; the router forward-declares `cab::AmpStage`.
- **`ampOn` kept as master gate; `ampMode` is {Capture,Tube} only** (off = `!ampOn`) → zero
  behaviour change for existing `cab::Params` consumers / tests.
- **Pure golden tests `TubePowerAmp` alone** (JUCE-free). The router crossfade can't be meaningfully
  unit-tested until the tube stage is non-identity (a later block) — for now it's covered by the
  491 integration tests + the correctness review.

### After-review findings — all resolved
- **A (real):** `ampMode` didn't recompute PDC. → wired listener + `pendingLatencyRefresh` +
  `updateLatency`. **FIXED.**
- **B (minor):** off-gate could lag ≤30 ms mid-fade. → an `off` target is now an instant hard
  switch even mid-fade. **FIXED.**
- **C (latent):** capture↔tube crossfade isn't latency-aligned when the NAM rate-matches (non-48k);
  PDC leads audio by ≤ the fade length. Negligible while tube latency is 0. **DOCUMENTED** in
  `PowerAmpRouter.h` — revisit (delay-align the endpoints) at the oversampling block.

### Validation
- Build EXIT 0; **zero warnings in poweramp files** (only pre-existing `Convolver.h` float-equal).
- Scaffold golden: **4/4**. Core unit tests: **491/491** (CabEngineTests exercise the seam →
  backward compat confirmed). Targets built: golden + `OrbitCab_Tests` + `orbitcab_dsp_test`.

## Block 2 — Nonlinear core (oversampled PP/SE tube waveshaper) — ✅ DONE (validated, maniacal tests)
HEAVY block: full protocol — consilium (codex `gpt-5.5`/xhigh + deepseek-v4-pro/max) + **blind-pass**
Claude agent → grounding self-call (exact felitronics API) → implement → **maniacal adversarial test
design** (2nd consilium + blind) → implement tests → fix real bugs / document real limits.

### What landed
- `TubePowerAmp` is now an **oversampled PP/SE tube waveshaper** (was a no-op):
  `Drive pre-gain → up ×4 (felitronics PolyphaseOversampler, tpp=32) → TubeStage → OS-domain DC-block
   → down → drive-comp → Output`. Latency = tpp-1 = **31**, constant across drive/tube/topology/OS factor.
- `src/poweramp/TubeKernel.h` — the **pure, unit-testable transfer** (`TubeStage`, voicing presets):
  SE `g(u)`; PP `g(u+Vb)−g(−u+Vb)` (odd by construction ⇒ even harmonics cancel EXACTLY). std+felitronics
  only (embedded-safe). Extracted so the static curve is tested directly, not via a parallel reimpl.
- `cab::Params::TubeParams` POD + 4 APVTS params (`tubeDrive/tubeOutput/tubeType/tubeTopo`) wired through
  packParams → router → `TubePowerAmp::setParams`. Inert unless Tube mode. No new latency listeners.
- Voicings 6L6/EL34/EL84/KT88 as coefficient presets (`evenLeak=0` ⇒ exact PP cancellation).
- Test target renamed `orbitcab_poweramp_core_golden` (`tests/PowerAmpCoreGolden.cpp`); felitronics
  `saturation`+`oversampling` linked in all 5 targets. `oversampleFactor` is a `prepare()` arg (default 4)
  so the golden can null 4x vs a 32x reference.

### Testability refactor (no shipping-path perf cost)
Pure kernel free of OS; OS factor injectable via `prepare(sr, maxBlock, osFactor=4)`. Shipping callers
pass the default → identical codegen. Enables exact kernel asserts + the 4x-vs-32x aliasing method.

### Maniacal battery (30 checks, all green) — written to BREAK the idea, not to pass
Kernel: PP exactly odd (`max|f(x)+f(-x)| = 0.0`, even harmonics −172 dBc), SE asymmetric, bounded.
Stage: measured impulse latency == 31; block-size determinism **bit-exact (0.0)**; THD monotonic per
tube; PP even < −90 dBc & SE even present; drive-comp small-signal unity **within 0.002 dB**; near-identity
at Drive=min. Aliasing: 4x-vs-32x audible-band MAP over (freq×drive), gated in the guitar range, HF+hot
documented. Adversarial/cross-platform: NaN-burst recovery, pathological numerics (−0/denormal/huge/±Inf/NaN),
**lifecycle (process before prepare)**, **div-by-zero provocation** (maxBlock=1, n>maxBlock, silence),
cross-instance determinism, stereo isolation+symmetry, param-change zipper, PP-under-DC-offset physics.

### Real bugs found by the tests → FIXED in the shipping code
1. **NaN/Inf poisoned the stream forever** (persistent DC/OS state) → sanitize at the input gate.
2. **Huge input × gain overflowed to +Inf** → clamp the post-gain value (±1e6).
3. **`process()` before `prepare()`** (maxBlock==0) → **infinite loop + `1/0` div-by-zero** (silent inf
   on ARM, a trap on x86 with FP exceptions — macOS dev never sees it) → guard `maxBlock<=0` and `n<=0`.
4. **`numSamples > maxBlock` silently left the tail dry** → internal maxBlock chunking.
5. Denormal flush on the DC-block state; `(int)`-cast of `tubeType` guarded against non-finite (UB).

### Documented limits (honest — thresholds NOT relaxed to hide anything)
- The 4x-vs-32x comparison has a **~−76 dBc method floor** (passband-ripple mismatch between the two
  oversamplers), constant vs drive ⇒ it is the floor, not aliasing. Guitar range (fundamentals ≤ ~1.2 kHz,
  Drive ≤ 24 dB) sits AT the floor → clean. Aliasing rises above the floor only for HF fundamentals AND
  hot drive together — the documented **8× / hard-class-B** boundary, deferred to a later block.
- Topology PP↔SE blend is **linear** (the DESIGN's "equal-power" was aspirational; linear measured
  click-free, X5). PP even-cancellation is conditional on a zero-mean input (X6 — real amp physics).

### Validation
Whole project builds (plugin + dsp_test + fade_measure + tests + golden) EXIT 0, zero warnings in poweramp
files. Core unit tests **491/491**. Maniacal golden **30/30**. Backward compat preserved.

### After-review hardening (3 Claude review agents on the block-2 diff + a maniacal test-rigor audit)
The DSP core was confirmed correct; the review found + fixed:
- **Code bug:** the input gate sanitized the SIGNAL but not the GAIN — a non-finite `driveDb`/`outputDb`
  param made `g` NaN and `std::clamp(NaN)` passes NaN straight through → poison. Now sanitized at the
  `setParams` entry point + a post-gain clamp. (Regression-tested by X12.)
- **Test rigor:** added the **RT no-alloc assertion** (X11 — the 🔴 rule, via an operator-new counter:
  **0 allocations** across process()/setParams). Replaced the floor-limited 4x-vs-32x aliasing gate with a
  **reference-free non-harmonic** metric (no 32x reference) that reveals the honest **~-73 dBc 4x/tpp=32
  oversampler floor** — constant vs drive ⇒ it's the OS filter quality, and the stage adds NOTHING above it
  in the guitar range (the MAP proves it). Tightened S7 (~10x), added a non-finite-param test, honest
  renames, plus a linear-phase (IR-symmetry) and a sample-rate-independence (44.1/88.2/96/192 kHz) test.
  Battery now **30/30**.
- **CI:** the core golden was compiled but never RUN — added it as a hard gate in `build.yml`.
- **Rebased** onto `origin/main` (`22f0f93`, PR #90): the MSVC win-stack fixes + the felitronics-core
  **v0.1.3** pin — clean rebase (no conflicts), re-verified 491/491 + 30/30 on the new base.
- **Deferred (documented):** the capture↔tube crossfade isn't latency-aligned now that tube latency = 31
  (a ~0.65 ms comb during a *manual* mode switch; a delay-line align is a small later fix). Beating the
  -73 dBc OS floor needs a sharper OS (higher tpp/factor) → a CPU + latency tradeoff, unnecessary under a cab IR.

---

## Block 3 — the "feel" layer: SAG + PRESENCE/DEPTH  (2026-07-01)

Scope: dynamic power-supply **sag** + NFB-style **presence/depth** voicing (virtual-load / output-transformer
deferred to a later block). All isolated in `src/poweramp/`; feature branch, no push.

### What shipped
- **SagEnvelope.h** — pure, header-only, unit-testable dual-TC sag model. Feedforward follower on rectified
  DEMAND (|G·x|, mono-linked shared supply) → droop ∈ [0, maxDroop·amount]. Fast attack (rectifier), slow
  recovery (B+ bloom). Applied as a shrinking rail s=1−droop: `1/s` folds into the Drive input ramp, `s` into a
  post-downsample gain → **y = s·shaper(u/s)** (earlier breakup + lower ceiling = squish, not a VCA duck).
- **Presence/Depth** — two `felitronics::eq::Svf` min-phase shelves held at fully-open gain; the dynamic
  "NFB opens when pushed" is a **per-sample dry/wet blend** driven by the per-sample droop (per-sample, NOT
  per-block — that's what keeps it block-size-deterministic). `blendForNominal` sets the quiet shelf to the
  nominal knob gain; droop opens it toward Gmax.
- Params: `TubeParams` += sag/presence/depth [0,1]; `TubeVoicing` += per-tube sag/NFB constants; 3 APVTS floats
  (`tubeSag/tubePresence/tubeDepth`, 0–100 %) at kParamVersion 1; packParams wiring. `felitronics::eq` linked
  into all 5 targets compiling TubePowerAmp.cpp.
- **FEEL GATE:** sag=presence=depth=0 ⇒ every block-3 path is skipped ⇒ **byte-identical to block 2** (all 30
  block-2 golden checks still pass unchanged = the superset invariant).
- **orbitcab_tube_audition** — offline render tool (there is no tube-param UI yet): a DI wav → Tube mode → cab, a
  curated 12-preset matrix → labelled −3 dBFS wavs to judge by ear. `<DI.wav> [cabIR.wav] [outDir]`.

### Grounding (real felitronics API)
`eq::Svf` gives runtime-gain min-phase High/LowShelf (TPT, 0 latency, JUCE-free) → no hand-rolled shelf; one
instance covers both channels. Shelf Q is Butterworth-fixed → the Depth "resonance bump" dropped for v1 (plain
shelf). Latency unchanged (**31**).

### Validation
Core **491/491**. Maniacal golden **42/42** (30 block-2 + B1–B6): SagEnvelope dual-TC unit (attack½=128,
release½=12115 samples), sag compression (−4.7 dB), presence/depth passband FR (+6/+5 dB), feel-ON robustness
(RT no-alloc, latency 31, block-size determinism, NaN/huge finite), NFB-opens (1.21→1.74), overflow recovery.

### Consilium + after-review (codex gpt-5.5/xhigh + a Claude adversarial agent — both converged)
- **Fixed — sag overflow poison:** a finite input whose |x|·g exceeds FLT_MAX made demand +Inf → the sag env
  latched to NaN (flushDenormals didn't clear it) → rail stuck at the 0.2 floor forever. Now the demand is
  capped + `SagEnvelope::flushDenormals` zaps NaN/Inf. **B4's 1e30 never overflowed** (false confidence) → added
  **B6** which feeds 1e38 (genuinely overflows) and asserts the post-burst tail matches a clean run.
- **Fixed — audition tool:** pre-roll → ~600 ms (the processor may not override reset(), and 128 ms was shorter
  than EL84's 240 ms sag recovery → presets bled); header comment reconciled with the −3 dBFS normalisation.
- **Deferred (documented):** the per-block **bias-shift** was cut — a per-block operating-point shift breaks
  block-size determinism; a per-sample-bias `TubeStage` overload is needed (`TubeVoicing::sagBiasDepth` reserves
  it). Sag research (codex GitHub pass): the plate/screen-split reservoir model (Amp Books) is the high-value
  future upgrade; `s·shaper(u/s)` is more amp-like than the common OSS "duck gain from rectified audio."
- **Known limit (shared with block 2, not a regression):** the per-block smoother `a=1−exp(−n/τ)` is block-size-
  exact at a block's END, but the intra-block ramp trajectory differs by buffer size DURING a live knob move — a
  sub-25 ms transient, inaudible; the golden's determinism check covers the steady-state (constant-param)
  guarantee that matters for offline-bounce == realtime.

---

## Post-block-3 hardening — UI, level-match, latency alignment, golden reconcile

Between block 3 and block 4 (no new DSP block), the stage was made shippable + the test debt paid:
- **SIMULATOR UI tab** (radio with CAPTURES via ampOn+ampMode), **12-o'clock calibrated defaults**,
  **deterministic per-voicing/drive level-match** (no follower → no enable/disable "kick"), an **always-on
  per-voicing MID bell** (the amp fingerprint), and a **per-checkout UI build-number stamp**.
- **Latency alignment (the enable/disable gap+jump):** the tube carries ~31-sample oversampling latency
  vs 0 for off/capture; the SIMULATOR power toggle flipped PDC → host re-sync gap + misaligned crossfade.
  Fixed by reporting a CONSTANT poweramp latency per MODE (not the power toggle) and delaying the dry/off
  path to match via a shared `cab::DryAligner`. Extended to the capture + preamp NAM stages (rate-match
  latency, 0 at 48k) — including keeping their models ARMED while powered off so the latency persists.
  Guards: `orbitcab_latency_pdc_test` (processor-level, tube PDC invariant) + CabEngine bypass-alignment
  checks in PowerAmpRouterAlignTests (bit-exact dry delay = the model's rate-match latency @96k).
- **Golden reconcile (S4,S5,S6,S7,S8,B3):** the always-on MID bell + voicing recalibration made the tube
  voiced / non-unity / min-phase, so the block-2 "pure waveshaper" checks (unity, identity, linear-phase,
  THD, presence threshold) were STALE. Reconciled (consilium codex+deepseek + Claude adversarial after-
  review) to assert the NEW correct contract — NOT relaxed to hide a bug:
  - S4 → THD monotonic on the bell-free KERNEL over 12 dB Drive steps (a KT88 class-AB crossover dip near
    6→12 dB is real push-pull physics, documented, not fudged away).
  - S5 → the bell-robust SE≫PP differential is the asymmetry proof; the absolute H2 floor relaxed −35→−45.
  - S6 → drive-comp gives DRIVE-INVARIANT small-signal gain (<0.05 dB drift), not unity (the bell sets it).
  - S7 → the MID bell delivers the SPEC'd per-voicing tone (gain@midHz ≈ midDb) + linear at Drive-min.
  - S8 → OS round-trip is LINEAR-PHASE via residual group delay ≈0 (latency-aligned) at HF (bell ≈0 phase).
  - B3 → presence delivers the voicing's spec'd boost (≈dbToGain(presenceMaxDb)).
  Golden **43/43** again.

## Block 4 — virtual load + output transformer + dynamic bias (all knob-scaled, default OFF)

Three "amp realism" stages, each a knob (default 0 ⇒ block-3-identical, golden-clean), consilium-designed
(codex + deepseek). The user approved stage 1 by ear ("ОХРЕНЕТЬ как хорошо") + asked for UI knobs on all.
- **Stage 1 — VIRTUAL LOAD** (`Load` knob): reactive-speaker impedance pre-EQ BEFORE the nonlinearity —
  per-voicing LF cone-resonance Bell + HF inductive-rise HighShelf (felitronics Svf). Golden B7 (spec-exact).
- **Stage 2 — OUTPUT TRANSFORMER** (`Iron` knob): OS-domain LF core saturation (low-note grind/compression,
  split-band tanh) + HF leakage rolloff. Golden B8.
- **Stage 3 — DYNAMIC BIAS** (`Bloom` knob): per-sample PP-bias drift toward class-B under sag (crossover
  bloom) via a `TubeStage::at(u, vbDelta)` overload — per-sample (ZOH from the host-rate droop) so it stays
  block-size-safe, not a per-block reconfigure. Needs Sag > 0. Golden B9.
- UI: SIMULATOR row is now Drive · Sag · Presence · Depth · **Load · Iron · Bloom** · Output (8 knobs).
- Golden **50/50**. Values are consilium/by-ear starting points; tune per voicing.

## Clean capture<->tube switch — root cause + fix (2026-07-02)

User symptom: a live Capture<->Simulator switch had a GAP (accepted: PDC re-report) + a level
jump and "~0.5 s volume drift". A previous pass tried fast-track (pumped, removed), a makeup
slew (kept), keep-warm (no effect, removed), constant-PDC (rejected: permanent latency).

### Diagnosis (this session — measured, not inherited)
Built `orbitcab_switch_probe` (dev tool): gain maps vs level, null-vs-continuous-reference
envelope traces, stale-wake A/B nulls, shared-stimulus reference gains.
1. **The slew was broken**: clamping the target ±slew around the SMOOTHER'S CURRENT value while
   `setTargetValue` re-arms the 30 ms ramp every block compounds into a block-size-dependent
   crawl — measured **0.4 dB/s @ 64-sample blocks / 3.2 @ 512** instead of the designed 9. The
   leveler was effectively FROZEN (settled −0.41 dB where −12.7 dB was correct).
2. Because of (1), a mode switch played the FULL uncompensated step: post-amp A/B (capture
   −17.5 vs tube −19.9 dBFS @ −18 in) + the cab's spectral weighting difference (this IR:
   capture-spectrum +12.7 dB vs tube +8.6) ≈ **6.7–8.8 dB jump**. The ORIGINAL "0.5 s swell"
   complaint was the healthy pre-slew leveler honestly chasing that 4.1 dB cab component
   through its 150 ms followers.
3. **Frozen-stage state is NOT audible**: switched-away-and-back vs never-switched nulls show
   ±0.05 dB (sag/NAM state freeze is fine; keep-warm unnecessary — confirmed).
4. The A/B step is LEVEL-dependent (capture = hard limiter: output pinned ≈ −17.3 dBFS for
   −30…−6 in; tube passes more dynamics): 8.5 dB @ −30 → ~1.8 @ −12. No constant trim can
   equalize everywhere; at playing levels it's stable ~1.8–2.5 dB.

### Fix (consilium: codex gpt-5.5/xhigh + deepseek-v4-pro/max; both answers folded in)
- **AutoLeveler reworked**: SmoothedValue dropped. Block-rate target dB-slew-limited against
  the PREVIOUS TARGET (true 9 dB/s, block-size-invariant) + per-sample rate-limited applied
  gain (dB-linear glide as a geometric progression, one multiply/sample; the LINEAR gain is
  the single source of truth — a separate dB accumulator drifts and pops on landing, caught by
  the sample-slope test). Deterministic retargets (route snap, on/off flip) jump the target and
  glide at a bounded FAST 40 dB/s (still a hard rate limit — synced with the seam fade, not a
  pump). Followers re-seeded at the CURRENT dry level on snap so the ratio sticks.
- **Per-route makeup memory (CabEngine)**: each route's converged makeup is cached while it
  dwells (≥0.4 s, non-silent, not fading) and SNAPPED to when the route returns. Keyed to a
  level-context generation (EQ/filters/dry-wet/tube knobs/input trim POD compare + an atomic
  bumped by IR/model mutators) — a stale cache never snaps (consilium: stale snap = pump).
  Trigger = router-ACCEPTED fades (`fadeJustStarted()`), not raw params (automation faster
  than the 30 ms fade would desync), + the preamp power flip.
- **Deterministic capture level-match for the tube**: at model load AmpStage measures the
  capture's reference gain on the shared `cab::levelprobe` stimulus (LP-shaped −18 dBFS noise;
  message thread, pre-swap instances, re-Reset after → live path stays bit-identical). The
  router lifts tubeMakeup by (capRefGainDb − kTubeRefGainDb = −1.96, probe-measured) — post-amp
  A/B step at playing levels: **2.5 → 0.3 dB** (gain map: −30…−6 now 6.4/2.1/0.3/−0.3/−0.4/−0.5).
- Result (probe, real IR + real capture): repeated A/B lands ±0.5 dB in ≤150 ms; output A/B
  step (leveler on, noise) 6.7 → **0.18 dB**; leveler-off switch artifacts ±0.2 dB; stale-wake
  nulls ±0.05 dB unchanged. FIRST visit to a route after an IR/EQ/knob change converges once,
  honestly, bounded by 9 dB/s (~0.5 s for a 4 dB cab-weighting difference) — deterministic
  pre-knowledge would need a second convolver; a spectral-estimate upgrade (IR FR × probed
  model spectrum at load time) is the designed future step if it ever matters by ear.
- Tests: AutoLeveler battery (anti-freeze regression, block-size/sample-rate invariance,
  per-sample slope caps, snap semantics, silence edges, enable flips) + the switch-contract
  battery (reference honesty, first-visit bounds, repeated-A/B snap, 10 Hz abuse, stale-context
  no-snap). Full suite 581k checks green; goldens 50/50; PDC gate green.

### After-review hardening (8-angle adversarial pass on the diff; all fixes re-validated green)
- **capDb stepped instantly** when a capture model finished loading/clearing while the TUBE was
  live (an async ±12 dB jump, no fade covers it — the route never changes) → the capture-match
  trim now GLIDES at 40 dB/s (`kCapMatchSlewDbPerSec`), seeding directly on the first block.
- **Cache poisoning windows**: route-makeup writes now also require `AutoLeveler::settled()`
  (never remember a mid-glide gain) and the dwell restarts on an autoLevel flip — an off→on
  toggle followed by a fast A/B could otherwise snap to a half-glided ~unity value.
- **Mirror-ordering race**: refGainDb/refGainValid are published AFTER the live-model swap
  (they feed the AUDIO thread; storing first paired the new model's gain with the old model).
- **Preamp probe waste**: the reference-gain probe is skipped for the preamp stage
  (`measureRefGain=false`) — nothing consumes its value; saves ~0.3 s-of-inference per load.
- **Identical-bytes reload** (the ampOn power toggle re-arms the same .nam) no longer bumps the
  level context — an FNV-1a content hash guards it, so power-cycling doesn't nuke route memory.
- **Context compare quantized** (0.05 dB / 0.005 amount / 1 Hz grids) so CC jitter or a smoothed
  automation ramp can't re-stale the caches every block and silently disable the snap.
- **Preamp-flip snap** is gated on `!isFading()` (never retarget ahead of a deferred seam fade).
- **kTubeRefGainDb anchor guard**: a unit test re-measures the tube's reference gain and fails
  if the baked constant, the levelprobe stimulus, or the voicing calibration drift apart.
  (Known limit: the anchor is 48 kHz; other host rates may add sub-dB drift — documented.)
- Cleanups: one shared LCG (`levelprobe::white`) across probe/tool/tests; dead legacy
  `rampSeconds` param dropped from `AutoLeveler::prepare`; dead includes removed.

### KNOWN LIMITATION (found by B9, pre-existing block 3): feel-layer block-size determinism
At feel=0 the stage is BIT-EXACT across block schedules (S3 = 0.0). But with the FEEL layer engaged there is
a small (~1e-2, steady-state) block-size discrepancy — it is present in block 3's sag/presence/depth (which
shipped and sound right) and is NOT a block-4 regression (the full block-4 path measures 8.3e-3 vs block-3
feel's 1.64e-2). Suspected: the per-block coefficient smoother interacting with long-memory feel state
(sag envelope τ up to 500 ms + the Svf shelves). Inaudible; B9 guards against block 4 making it worse.
TODO: root-cause (candidates: Svf setParams-per-block, or seed/settle the feel smoothers) and restore
bit-exact feel-on determinism. Revisit the 8× / ADAA anti-alias decision when the tube gains HF-hard-clip use.
