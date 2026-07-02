# Block 2 — Nonlinear tube core — DESIGN (locked)

Synthesised from the consilium (codex `gpt-5.5`/xhigh + deepseek-v4-pro/max) + an independent
blind-pass Claude agent. Strong 3-way consensus. Implementation spec for block 2.

> **AS-BUILT deltas live in [STATUS.md](STATUS.md).** Notably: the PP↔SE blend shipped **linear**
> (not the "equal-power" sketched below — measured click-free); the aliasing claim is an honest
> operating-range MAP with a ~-76 dBc 4x-vs-32x method floor; and the maniacal test pass surfaced +
> fixed real robustness bugs (NaN-poison, overflow, process-before-prepare div-by-zero, n>maxBlock).

## Scope
Turn `cab::poweramp::TubePowerAmp` from a no-op into an **oversampled PP/SE tube waveshaper** with
tube voicings + Drive/Output/Tube/Topology. NOT in this block: sag, NFB loop, virtual speaker load,
output transformer (later blocks).

## Architecture (inside `TubePowerAmp::Impl`)
`PolyphaseOversampler(L=4, tpp=32)` + `WaveShaper(Asym)` hand-wired — **not** `Saturator` wholesale
(PP needs a 2-eval differential; Saturator is single-shaper). Replicate Saturator's *recipe*:
```
Drive pre-gain (smoothed) → upsample ×4 → PP/SE kernel → DC-block (OS domain) → downsample
  → drive-comp (baseband) → Output post-gain (smoothed)
```
All state heap-allocated in `prepare()` (prepare internal buffers for max 2 ch, like AmpStage). No
alloc/lock in `process()`. **Hand-rolled std one-pole smoothers — NOT `juce::SmoothedValue`** → keeps
`TubePowerAmp.cpp` JUCE-free (golden links without JUCE) AND embedded-buildable (see constraint below).

## Kernel
`g(u) = Asym = tanh(k·(u+b)) − tanh(k·b)` (peak-normalised; `slopeAtZero()` available).
- **SE (class A):** `y = g(G·x)` → asymmetric → even-harmonic-rich (2nd). DC-block required.
- **PP (class AB):** `y = g(G·x + Vb) − g(−G·x + Vb)` → odd-symmetric by construction → **even harmonics
  cancel exactly** (Taylor: even powers vanish). `Vb` = class-A↔AB / crossover bias. **Block 2 = warm
  AB only** (C¹-smooth, alias-safe); hard class-B + 8× OS deferred. Tiny per-half `evenLeak` mismatch
  → controlled small 2nd-harmonic "warmth" (voicing, not a bug).
- Live PP↔SE switch: short internal **equal-power crossfade (~25 ms)**, both kernels run (cheap).

## Tube voicings (coefficient presets — brand-neutral, tune by FFT/ear)
`{ driveScale, k, b(SE), Vb(PP), evenLeak }`. Starting points:
| Tube | driveScale | k | b | Vb | evenLeak | character |
|---|---|---|---|---|---|---|
| 6L6  | 0.85 | 2.0 | 0.18 | 0.30 | 0.03 | high headroom, tight, late breakup |
| EL34 | 1.10 | 2.6 | 0.28 | 0.22 | 0.06 | mid-forward, earlier crunch |
| EL84 | 1.40 | 3.2 | 0.33 | 0.18 | 0.08 | low headroom, chimey, crossover edge |
| KT88 | 0.70 | 1.7 | 0.12 | 0.34 | 0.02 | most headroom, smoothest |
`k` stays per-tube (curve identity), NOT driven by the knob. `driveScale` folds into `G` so each
tube's breakup point differs while Drive still means "how hard I push".

## Controls (4; inert unless `ampOn && mode==tube`)
- **Drive** → input pre-gain `G = dbToGain(driveDb·driveScale)`, per-sample, smoothed. ~0..+36 dB.
- **Output** → post-gain, smoothed. ~−24..+12 dB.
- **Tube{6L6,EL34,EL84,KT88}**, **Topology{Push-Pull, Single-Ended}**.
- **Drive-comp** `comp = (G·slopeNumeric)^(−autoComp)`, autoComp≈0.8, **numeric central-difference
  slope** of the actual kernel (topology-agnostic) → honest A/B, near-identity at Drive=min.

## Latency
`latencySamples() = ovs.latencySamples() = tpp−1 = 31`, **constant** across factor + every sound
param (mode-only). Already plumbed (block 1): router → CabEngine → updateLatency + ampMode listener.
Block 2 just returns a non-zero number → **zero new latency wiring**.
**Finding C** (crossfade not latency-aligned): now 31 samples ≈ 0.65 ms @48k, transient over a 30 ms
manual fade → **ACCEPT**, keep the doc note; delay-align endpoints in the integration block.

## Params plumbing (minimal diff)
- `core/Params.h`: add POD `struct TubeParams { float driveDb, outputDb, autoComp; int tubeType; bool singleEnded; }` + field `TubeParams tube;` in `cab::Params` (mirrors `EqParams eq;`).
- `TubePowerAmp.h`: add `void setParams(const cab::TubeParams&) noexcept;` (the ONE header touch — reuses the POD, no DSP type leaks; `process` signature unchanged).
- `PowerAmpRouter::process(...)` gains `const cab::TubeParams&`, calls `tube.setParams(...)` before the tube render; `CabEngine::process` passes `p.tube`.
- `Parameters.cpp`: 4 params at `kParamVersion` — `tubeDrive`(float), `tubeOutput`(float), `tubeType`(Choice), `tubeTopo`(Choice). Cache ptrs in PluginProcessor, read in `packParams()`. **No latency depends on them → no new listeners.**

## Golden test (`tests/PowerAmpCoreGolden.cpp`, JUCE-free, links saturation+oversampling)
Hand-rolled DFT (non-RT test). Bit-exact identity NO LONGER holds (FIR round-trip). Assert:
1. near-identity at Drive=min, latency-aligned by 31, max-abs < 1e-3 (ripple tol);
2. THD(min) < THD(mid) < THD(max), THD(min) < ~0.1%;
3. PP 2nd/4th ≥ ~50 dB below fundamental; SE 2nd within ~20–30 dB → PP_even ≪ SE_even;
4. alias floor < −80 dB @4× (13–15 kHz tone into hard drive, non-harmonic in-band bins);
5. `latencySamples()==31`, invariant across Drive/Tube/Topology;
6. SE DC mean < 1e-4 after DC-block; PP ~0;
7. RMS level-match across Drive within ~0.5 dB (drive-comp honest);
8. no-NaN + hostile variable-block sweep `{1,17,64,128,333,512}`.

## Embedded / extraction constraint (per Oleh's question)
Keep `TubePowerAmp` **std-only + header-only felitronics deps + heap-only-in-prepare + hand-rolled
smoother**. This single discipline serves BOTH the felitronics-core extraction AND an M7/Daisy-class
embedded build. Don't pull JUCE into the stage (the router keeps JUCE, at the integration layer).
Bare-metal-no-heap (static allocation via template max-ch/block) is a later option, not precluded.

## Top risks
1. **Aliasing** at high drive / cold crossover → warm-AB only, C¹-smooth kernel, alias golden gate; 8×/hard-class-B later.
2. **Level/PDC trust** ("louder wins") → numeric-slope drive-comp, latency fixed=31 (param-independent), RMS-match + near-identity goldens.
3. **Zipper / JUCE-leak** → hand-rolled one-pole on every coeff (~25 ms), equal-power topology crossfade, keep `.cpp` JUCE-free.
