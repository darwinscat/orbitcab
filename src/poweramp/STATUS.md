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

### Maniacal battery (26 checks, all green) — written to BREAK the idea, not to pass
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
files. Core unit tests **491/491**. Maniacal golden **26/26**. Backward compat preserved.

### Next: Block 3 — the "feel" blocks (sag, NFB loop, virtual load, output transformer) — see PLAN.md.
Revisit the 8× / ADAA anti-alias decision when the tube stage gains real HF-hard-clip use.
