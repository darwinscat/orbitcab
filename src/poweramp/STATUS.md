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

### Next: Block 2 — nonlinear core (PP/SE waveshaper @4× OS + tube voicings) — **HEAVY block**
Triggers the full protocol incl. the blind-pass Claude agent.
