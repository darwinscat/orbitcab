# Coding style — OrbitCab

> Written after the headless-core refactor, **derived from the code that exists**,
> not invented up front. The rules below are what the `core/` / adapter / `ui/` code is
> already written in; follow them so new work stays consistent. Formatting is captured in
> the repo `.clang-format`.

## The one rule that matters most: the layer boundary

Three layers, dependencies point **one way only**: **GUI → Adapter → Core.**

| Layer | Where | May depend on | May NOT touch |
|-------|-------|---------------|---------------|
| **Core** (`src/core/`, `cab::`) | the DSP math | `juce_dsp`, `juce_audio_basics` | `juce_gui_basics`, APVTS, files, the host, the editor |
| **Adapter** (`src/`, `OrbitCab*`) | APVTS, state, IR decode, threading | both sides | drawing / mouse |
| **GUI** (`src/ui/`) | editor + components | the adapter (via APVTS + a thin facade) | the audio thread |

**Litmus test when unsure where code goes:** *could it run unchanged inside a Web Audio
`AudioWorklet`, with no DAW and no window?* If yes → it's Core (and must stay JUCE-GUI-free,
so the math can compile to WASM / embedded). If it draws or handles input → GUI.
If it translates between the host/JUCE world and the Core → Adapter.

Concretely: the Core's whole audio-thread surface is `process(float* const* io, n, const
Params&)` + a `Params` POD. The Core never reads APVTS — the adapter packs the atomics into
`Params`. The display-FFT is **GUI** (the audio thread only fills a lock-free tap).

## 🔴 Real-time safety (non-negotiable)

Inside `process()` and anything it calls: **no allocation, no locks, no IO, no `throw`.**

- Preallocate in `prepare()` (filters, convolvers, scratch buffers).
- Cross threads with `std::atomic` or lock-free FIFOs (see `felitronics::analysis::SpectrumTap`).
- IR loads run off-thread + atomic-swap inside `cab::Convolver` — never in `process()`.
- Packing `Params` from APVTS atomics is plain float loads — RT-safe.

## Naming

- **Adapter / GUI** classes are product-named: `OrbitCabAudioProcessor`,
  `OrbitCabAudioProcessorEditor`, `OrbitCabLookAndFeel`.
- **Core** classes are brand-neutral, functional, in `namespace cab`: `CabEngine`, `IRSlot`,
  `AutoLeveler`, `Convolver`, `Params` — the core is the reusable unit the WASM/embedded
  build shares, so it doesn't carry the product brand.
- Adapter-side helpers shared across files live in `namespace orbitcab` (`createParameterLayout`,
  `bundledIRs`).
- Parameter IDs are **frozen** string literals (`"hpfOnA"`, …) — v1 sessions depend on them
  (see `Parameters.cpp`); never rename them.

## Files

- One responsibility per file. Header-only for tiny pure units (`Params.h`, `AutoLeveler.h`,
  `Convolver.h`, `IRSlot.h`); a `.cpp` only where there's real logic to
  compile once and link into the test target (`CabEngine.cpp`, `IRSlot.cpp`).
- **Abstraction only when there's a second user (YAGNI).** `Convolver` is concrete — its
  signatures are JUCE-free so a web/embedded backend can slot in later, but no `IConvolver`
  interface exists until that second backend is actually written.
- Quoted includes resolve relative to the including file: from `src/` use `"core/X.h"` /
  `"ui/X.h"`; within `src/ui/` a sibling is just `"X.h"`.

## Formatting (`.clang-format`)

JUCE-flavoured: 4-space indent, **Allman braces** (`{` on its own line for funcs/classes/
blocks), **space before parens** (`foo (args)`, `if (x)`), **left pointer** (`float* p`),
**spaces inside braced inits** (`{ 1.0f }`), namespaces not indented. `ColumnLimit: 0` — keep
lines hand-aligned; clang-format normalises spacing/braces but won't rewrap.

> Not auto-enforced in CI yet (a whole-tree reformat is a separate, dedicated commit). Run
> `clang-format -i <file>` on files you touch; keep the diff to your change.

## Comments

Explain **why**, not **what** — the codebase is strong here, keep it. A comment earns its
place by recording a decision, a constraint (🔴 RT, frozen IDs), or a non-obvious reason
(`// new IR => force reload`). Don't narrate the obvious.

## C++ idioms

- `std::unique_ptr` for ownership; **no raw owning pointers**. `T&` for non-null non-owning,
  `T*` only when nullable.
- `const`-correctness; mark methods `const` when they don't mutate.
- `constexpr` / `enum class` over `#define`; named constants, no magic numbers.
- No exceptions / no heap in the RT path (and, for the eventual embedded target, no heap in
  the IR-load path either).

## Tests

- The core is unit-tested headless (`tests/`, `OrbitCab_Tests`, links `core/` only). New
  core units ship their tests **in the same commit** (extract `AutoLeveler` → its test lands
  with it).
- Integration / state behaviour that needs the full processor lives in the offline
  `tools/dsp-test` harness (it returns non-zero on any failure → a real CI gate).
- Every change keeps the gate green: build + `auval -v aufx Orbt Dcat` +
  `pluginval --strictness-level 10` + `OrbitCab_Tests` + `orbitcab_dsp_test`.
