# Architecture

> The realized architecture after the headless-core refactor is below. The original pre-code
> design intent follows under "The big picture" (kept for history). Guiding rules unchanged:
> build the signal chain **modular** and version the saved state **from day one**.

## Realized architecture (post-refactor — 2026-06-19)

Three layers; dependencies point **one way only: GUI → Adapter → Core.**

```
            host / DAW                                   user (mouse/keys)
                │                                              │
                ▼                                              ▼
   ┌─────────────────────────────┐            ┌──────────────────────────────┐
   │ ADAPTER  (src/, OrbitCab*)   │  APVTS +   │ GUI  (src/ui/, OrbitCab*)    │
   │ juce::AudioProcessor         │◀──facade──▶│ editor + draw-only leaves     │
   │ • APVTS (Parameters.cpp)     │            │ • WaveformDisplay, LevelMeter │
   │ • state save/load + IRPool   │            │ • LookAndFeel, HeaderBrand    │
   │ • IR decode (juce formats)   │            │ • display-FFT (graphics)      │
   │ • snapshots / undo           │            └──────────────────────────────┘
   │ • IRLibrary (bundled enum)   │
   └──────────────┬──────────────┘
                  │ cab::Params (plain POD)  +  float* const* buffers
                  ▼
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ CORE  (src/core/, namespace cab::)  — headless, juce_dsp + juce_audio_basics only │
   │   prepare(sr,maxBlock,ch,Params) ;  process(float** io, n, Params, bool) │
   │                                                                           │
   │   CabEngine ── IRSlot[2] (HPF → LPF → Convolver) ── A/B crossfade         │
   │            └─ AutoLeveler (wet→dry RMS match)  └─ master gain ── meters    │
   │            └─ SpectrumTap[2] (lock-free SPSC → GUI)                        │
   │   Convolver  = juce::dsp::Convolution behind JUCE-FREE float* signatures   │
   └─────────────────────────────────────────────────────────────────────────┘
```

**The boundary rule.** *Audio* = plain numbers + raw buffers → Core, never includes
`juce_gui_basics`, never reads APVTS, never touches files. *Graphics* = anything that draws
or handles input → GUI; the display-FFT is graphics (the audio thread only fills a lock-free
tap). *Adapter* = the translator between host/JUCE and Core (the only layer that depends on
both).

**Data flow per block.** `processBlock` clears extra output channels → `packParams()` reads
the cached APVTS atomics into a `cab::Params` POD → `engine.process(buffer.getArrayOfWritePointers(),
numCh, numSamples, params, isNonRealtime())`. The Core wraps the raw `float**` in a
non-owning `juce::AudioBuffer` and runs the whole chain in place. The Core never sees APVTS.

**Threading.** Audio thread: `process()` only (no alloc/lock/IO/throw — 🔴). Message thread:
IR decode + `IRSlot::setOriginalIR`/`applyTrim`, which build the truncated IR and hand it to
`Convolver::loadIR`, where `juce::dsp::Convolution` does the FFT prep + **atomic swap on its
own loader thread**. GUI thread: a 30 Hz timer reads meters + pulls spectrum frames (atomics).

**The convolver seam.** Everything the Core needs is small portable math *except* the
convolution, which is isolated behind `cab::Convolver` (raw-`float*` signatures, `juce::dsp::
Convolution` hidden in the impl). This keeps the convolution backend swappable without leaking
JUCE into the rest of the Core. Concrete class now; promoted to an interface only when a second
backend exists (YAGNI).

**Testability.** `OrbitCab_Tests` links `src/core/` ONLY (no GUI, no host, no MessageManager)
and unit-tests the Core (CabEngine identity/gains, IRSlot trim/head math, AutoLeveler
convergence/clamp/gate). `tools/dsp-test` drives the full processor for integration + state
round-trip. Both are CI gates.

**File map.** `src/core/` = `Params.h · CabEngine.{h,cpp} · IRSlot.{h,cpp} · Convolver.h ·
AutoLeveler.h · SpectrumTap.h`. `src/` (adapter) = `PluginProcessor.{h,cpp} · Parameters.{h,cpp}
· IRLibrary.h`. `src/ui/` = the draw-only leaves. `tests/` = the headless core tests.

---

## The big picture

A JUCE audio plugin is two halves:

- **Processor** (`juce::AudioProcessor`) — the audio thread. Real-time, no
  allocations/locks/file-IO in `processBlock`. This is where the DSP lives.
- **Editor** (`juce::AudioProcessorEditor`) — the GUI. Optional; the plugin works
  headless. Talks to the processor only through thread-safe parameters.

They communicate through an **`AudioProcessorValueTreeState` (APVTS)** — JUCE's
standard parameter store. APVTS gives us: thread-safe parameter access, host
automation, and **state save/load (`getStateInformation`/`setStateInformation`)
for free** — that's how a DAW remembers your settings in a session.

## Signal chain (built so it can grow)

```
v1 (MVP):
  input ─▶ [HPF] ─▶ [LPF] ─▶ [Convolution (IR)] ─▶ [phase invert] ─▶ [dry/wet mix] ─▶ [output gain] ─▶ out
                                    ▲
                              loaded IR .wav
v2 (planned):
  input ─▶ [PowerAmp (non-linear)] ─▶ [oversample ↑] ... [↓] ─▶ [HPF/LPF] ─▶ [Convolution] ─▶ [mix] ─▶ [gain]
           (oversample ONLY wraps the non-linear stage; convolution is linear)
v3 (planned):
  IR editor/mixer = offline processing of the IR buffer BEFORE it's handed to
  juce::dsp::Convolution. Doesn't change the realtime chain shape.
```

Implement the chain with **`juce::dsp::ProcessorChain`** so stages are
add/remove/reorder by editing one type alias, not rewiring `processBlock`.

The convolver is **`juce::dsp::Convolution`**:
- zero-latency *partitioned* convolution (`juce::dsp::Convolution::Latency` /
  `NonUniform` options),
- `loadImpulseResponse(...)` auto-resamples the IR to the host sample rate and can
  normalize it,
- this is the single biggest reason the project is "small": we don't write FFT
  convolution by hand.

## Parameters (v1)

| Param ID | Range | Notes |
|----------|-------|-------|
| `hpfHz`  | 20–500 Hz | high-pass before/after cab (decide), skewed log |
| `lpfHz`  | 1.5k–20k Hz | low-pass, log |
| `mix`    | 0–100% | dry/wet |
| `gain`   | -24..+24 dB | output trim |
| `phase`  | bool | invert polarity |
| (IR path)| — | the loaded IR file path / bundled-IR id, stored in state, not a host-automatable param |

> This table is the early shape; the authoritative parameter list lives in
> `src/Parameters.cpp`. As built: params are **per slot (A/B)** — HPF/LPF each split into
> an on-toggle + a freq, plus `phase`, `mix` (Dry/Wet), `trimOn`, `mute` — over the global
> `inputGain`, `gain`, `mixAB` (A↔B), `bypass`, `autoLevel`. **Defaults: helpers on
> (`autoLevel`); everything that colours/cuts is off.** **HEAD trim is NOT a param** — it's
> a global session-state property (see the state model below).

## State model — versioned from day one

The APVTS ValueTree is serialized into the DAW session. We add a top-level
`stateVersion` attribute now (v1 = 1). On load:

```
read stateVersion
  == current  -> load normally
  <  current  -> migrate (add v2 PowerAmp params with safe defaults), bump
  >  current  -> a newer plugin saved this; load what we recognize, warn
```

Without this, adding PowerAmp params in v2 risks corrupting/zeroing v1 sessions.
Cheap insurance, written once.

Two non-param settings ride alongside the APVTS params:

- **`headTrim`** — a property on the APVTS state tree (default on). Audio-affecting (it
  re-trims the IRs), so it travels with the session + A/B/C/D snapshots + undo, but is
  *not* a host-automatable param. Toggled from the gear settings panel.
- **Global view prefs** — `dryWetShown`, `spectrumOn` live in an app-wide `PropertiesFile`
  (owned by `AppPreferences`, one per machine), NOT the session. The update-checker's
  last-seen release tag shares that same single file.

## IR loading

- **User IRs:** drag-drop onto the editor + a file browser. Validate (wav, sane
  length/SR), then `convolution.loadImpulseResponse(file, ...)` (loaded on a
  background thread, swapped in atomically — never block the audio thread).
- **Bundled IRs:** the CC0 Brutal/Emerald packs embedded via JUCE `BinaryData`
  (BinaryBuilder/CMake `juce_add_binary_data`), exposed as an optgroup list mirroring
  the web cabinet-ir-utility.

## Planned source layout

```
orbitcab/
  CMakeLists.txt           # top-level: fetch JUCE, define the plugin target
  cmake/                   # helper modules (JUCE fetch pin, signing, etc.)
  src/
    PluginProcessor.h/.cpp # juce::AudioProcessor — owns the chain + APVTS
    PluginEditor.h/.cpp    # juce::AudioProcessorEditor — the GUI
    dsp/
      CabChain.h           # the ProcessorChain alias + helpers
      Convolver.h          # thin wrapper around juce::dsp::Convolution + IR loading
    state/
      StateVersion.h       # version constant + migration
    ui/
      ...                  # components (knobs, IR picker, drop zone)
  resources/
    ir/                    # bundled CC0 IRs (BinaryData source)
    icons/                 # branding
  packaging/
    macos/                 # entitlements, pkg scripts, notarize helper
    windows/               # installer / signing script
  .github/workflows/
    build.yml              # PR builds (no secrets)
    release.yml            # signed+notarized release (protected, secrets)
  tests/                   # pluginval / unit tests
```

## Quality gates we'll wire up

- **pluginval** (Tracktion's validator) in CI — catches a huge class of host-
  compat bugs.
- **auval** on macOS for AU.
- Load/save round-trip test for the versioned state.
- Test in at least: Reaper (cheap, scriptable), Logic/GarageBand (AU), one more.
