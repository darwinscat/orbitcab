// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cstddef>
#include <string>

#include "Params.h"
#include "IRSlot.h"
#include "AmpStage.h"
#include "DryAligner.h"                          // cab::DryAligner — dry latency alignment for the preamp bypass
#include "AmpEq.h"
#include "AutoLeveler.h"
#include "../poweramp/PowerAmpRouter.h"          // the poweramp seam: NAM capture <-> white-box tube
#include <felitronics/analysis/SpectrumTap.h>   // shared DSP: the SPSC capture tap (was cab::SpectrumTap)
#include <felitronics/core/Smoother.h>          // JUCE-free LinearSmoother — bit-exact juce::SmoothedValue<float,Linear> drop-in

//==============================================================================
// cab::CabEngine — the headless DSP core. Owns the whole real-time signal path:
//
//   global (front): in → [PREAMP (NAM)] → [AMP EQ] → [POWERAMP (NAM)]  — neural + tone, in order
//   per slot (A/B):  → HPF → LPF → Convolution → Phase → Dry/Wet(blend the amp output)
//   then:            MIX crossfades the two slot outputs → Auto-level → Output Gain
//
// The front stages run before the dry tap, so the dry/wet blend + the auto-level reference are
// their output (the thing being cab'd), not the clean DI. The preamp feeds the EQ feeds the
// poweramp. preamp/amp are the same cab::AmpStage class; the tone stack is cab::AmpEq (a teq::
// EqEngine) sitting between them, so its cuts shape what the poweramp distorts — distinct from
// the per-slot HPF/LPF, which shape the cab/IR band after the whole amp.
//
// Two cab::IRSlot instances are the per-slot channels (filters + convolver + the IR
// buffer + trim math); cab::AutoLeveler is the wet->dry match. No JUCE GUI, no APVTS,
// no files, no host: the adapter packs a cab::Params each block and calls process().
// This is the unit-testable core and the WASM/embedded seam.
//
// 🔴 Real-time rule: process() and its callees never allocate, lock, do IO, or throw.
// prepare() does all allocation; IR loads run off-thread via Convolver + atomic swap.
//==============================================================================
namespace cab
{

class CabEngine
{
public:
    // FFT contract for the GUI analyser (mirrors the audio-side tap window).
    static constexpr int fftOrder = felitronics::analysis::kSpectrumFftOrder;
    static constexpr int fftSize  = felitronics::analysis::kSpectrumFftSize;

    // Allocate + configure for this stream and seed smoothers from the initial parameter
    // values (so a restored session doesn't ramp from zero). Not the audio thread.
    void prepare (double sampleRate, int maxBlock, int numChannels, const Params& initial);

    // Process numChannels planar channels in place. RT-safe. `nonRealtime` true (offline
    // bounce) skips the spectrum capture. The adapter clears extra output channels and
    // supplies numChannels = the active channel count.
    void process (float* const* io, int numChannels, int numSamples,
                  const Params& p, bool nonRealtime);

    void reset();

    // Seed the auto-leveler's makeup from the loaded IR(s)' energy so the first audio after
    // prepare starts at ~the converged level (no startup kick, #48). Message thread — call
    // after the IRs are (re)applied in prepareToPlay.
    void seedAutoLevel();

    //--- IR lifecycle (message thread) — forwarded to the per-slot IRSlot --------
    void   setSlotOriginalIR    (int slot, const float* const* samples, int numChannels,
                                 int numSamples, double irSampleRate);
    double slotApplyTrim        (int slot, bool trimOn, float trimFraction01, bool headOn);
    void   slotLoadBytesFallback (int slot, const void* data, size_t size);
    void   clearSlotOriginal    (int slot);
    bool   slotHasOriginal      (int slot) const;
    double slotTrimmedSeconds   (int slot) const;
    const  juce::AudioBuffer<float>& slotOriginal (int slot) const;
    double slotOriginalSampleRate (int slot) const;
    void   pumpConvolverReloads() { slot[0].pumpReload(); slot[1].pumpReload(); }   // retry coalesced IR swaps (poll)

    //--- AMP (NAM) lifecycle — forwarded to the front-of-chain AmpStages ----------
    // Load/clear run on the message thread (off-thread build + atomic swap); collectAmpGarbage
    // is pumped by the processor's 30 Hz timer to reclaim swapped-out models safely. `amp` is the
    // poweramp; `preamp` is the second instance that runs ahead of it (same class, same threading).
    // A reload of the IDENTICAL bytes (the ampOn power toggle re-arms the same model) must NOT
    // invalidate the route-makeup memory — the deterministic ref-gain probe reproduces the same
    // value, so nothing the caches depend on changed. Detected via a cheap content hash.
    bool   loadAmpModelBytes  (const void* data, std::size_t size, float trimDb = 0.0f)
    {
        const bool ok = amp.loadModelFromMemory (data, size, trimDb);
        const juce::uint64 h = ok ? contentHash (data, size) : 0;
        if (ok && h != lastAmpBytesHash) { lastAmpBytesHash = h; bumpLevelContext(); }
        return ok;
    }
    void   clearAmpModel()                              { amp.clearModel(); lastAmpBytesHash = 0; bumpLevelContext(); }
    void   collectAmpGarbage()                          { amp.collectGarbage(); preamp.collectGarbage(); }
    bool   ampHasModel()         const                  { return amp.hasModel(); }
    double ampModelSampleRate()  const                  { return amp.modelSampleRate(); }
    double ampModelLoudness()    const                  { return amp.modelLoudness(); }
    bool   ampModelHasLoudness() const                  { return amp.modelHasLoudness(); }
    int    ampLatencySamples()   const                  { return amp.latencySamples(); }
    int    tubePowerAmpLatencySamples() const           { return powerAmpRouter.tubeLatencySamples(); }

    // Same identical-bytes guard as the poweramp.
    bool   loadPreampModelBytes (const void* data, std::size_t size, float trimDb = 0.0f)
    {
        const bool ok = preamp.loadModelFromMemory (data, size, trimDb);
        const juce::uint64 h = ok ? contentHash (data, size) : 0;
        if (ok && h != lastPreampBytesHash) { lastPreampBytesHash = h; bumpLevelContext(); }
        return ok;
    }
    void   clearPreampModel()                           { preamp.clearModel(); lastPreampBytesHash = 0; bumpLevelContext(); }
    bool   preampHasModel()        const                { return preamp.hasModel(); }
    int    preampLatencySamples()  const                { return preamp.latencySamples(); }

    //--- cross-thread reads for the GUI ------------------------------------------
    float inputLevel()  const { return inLevel.load  (std::memory_order_relaxed); }
    float outputLevel() const { return outLevel.load (std::memory_order_relaxed); }
    float autoLevelGain() const { return autoLeveler.currentGain(); }   // current wet->dry makeup (tests/diagnostics)

    // DSP load meter — each stage's wall-clock as a smoothed % of the block's real-time budget.
    // Written on the audio thread (a few monotonic-clock reads per block), read by the GUI.
    float cpuTotal()    const { return cpuPct[0].load (std::memory_order_relaxed); }
    float cpuPreamp()   const { return cpuPct[1].load (std::memory_order_relaxed); }
    float cpuEq()       const { return cpuPct[2].load (std::memory_order_relaxed); }
    float cpuPoweramp() const { return cpuPct[3].load (std::memory_order_relaxed); }
    float cpuCab()      const { return cpuPct[4].load (std::memory_order_relaxed); }

    void  setSpectrumActive (bool shouldFeed) { spectrumActive.store (shouldFeed, std::memory_order_relaxed); }
    bool  pullSpectrum (bool pre, float* destFftSize);

    double sampleRate() const { return currentSampleRate; }

private:
    // Message-thread notification that the level context changed OUTSIDE Params (an IR or model
    // load/clear/trim) — invalidates the per-route makeup memory on the audio side.
    void bumpLevelContext() { modelGeneration.fetch_add (1, std::memory_order_relaxed); }

    // FNV-1a over the model bytes — message-thread only, cheap vs the model build it guards.
    static juce::uint64 contentHash (const void* data, std::size_t size) noexcept
    {
        const auto* p = static_cast<const unsigned char*> (data);
        juce::uint64 h = 1469598103934665603ull;
        for (std::size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h != 0 ? h : 1;   // 0 is the "nothing loaded" sentinel
    }

    AmpStage    preamp;                    // optional NAM preamp, runs first (feeds the EQ → poweramp)
    DryAligner  preampBypassAlign;         // delays the dry to the preamp's PDC while it's OFF → toggling
                                           // the preamp never changes reported latency (no host re-sync gap)
    AmpEq       ampEq;                      // amp tone stack, between the preamp and poweramp NAM stages
    AmpStage    amp;                       // optional NAM poweramp (capture mode), front of the cab
    poweramp::PowerAmpRouter powerAmpRouter; // poweramp seam: ampOn/mode → capture(amp) | tube, click-free
    IRSlot      slot[2];
    AutoLeveler autoLeveler;
    juce::AudioBuffer<float> wet[2];       // per-slot convolution scratch
    juce::AudioBuffer<float> dryBuffer;    // copy of the raw input for the dry/wet blend

    // Smoothed so live tweaks / automation don't zipper. Phase is a sign (+1/-1) ramped
    // through 0 — a brief dip rather than a hard polarity click. (LinearSm alias: the ctor is
    // explicit in felitronics-core v0.1.3, so array members need direct-init, not copy-init —
    // clang enforces this; MSVC let it slide, which is how CI stayed green.)
    using LinearSm = felitronics::core::LinearSmoother;
    felitronics::core::LinearSmoother mixSm[2]   { LinearSm (1.0f), LinearSm (1.0f) };   // explicit ctor: direct-init
    felitronics::core::LinearSmoother phaseSm[2] { LinearSm (1.0f), LinearSm (1.0f) };
    felitronics::core::LinearSmoother mixABSmoothed { 0.0f };
    felitronics::core::LinearSmoother gainSmoothed  { 1.0f };
    felitronics::core::LinearSmoother muteGateSmoothed { 1.0f };

    double currentSampleRate = 44100.0;

    float inputGainPrev = 1.0f;            // block-ramp start for zipper-free input trim

    // Poweramp/preamp signal ROUTE (encoded {preampOn, ampOn, mode}), seeded in prepare(). A change
    // marks a discrete switch used by the tube<->capture loudness match; the makeup slew-limit
    // (AutoLeveler) keeps the leveler's own response gentle so a switch can't pump.
    int   prevRoute = -1;

    // --- per-route makeup memory: the deterministic leveler retarget on a route switch -------
    // Each route's CONVERGED makeup is remembered while that route dwells (settled, non-silent,
    // not fading). On a router-ACCEPTED route change the leveler SNAPS to the new route's
    // remembered makeup (a bounded fast glide in sync with the 30 ms seam crossfade) instead of
    // crawling there through the followers — the audible "~0.5 s volume drift" on capture<->tube
    // A/B. A cache is only trusted while the level CONTEXT (everything that shapes the wet/dry
    // spectral ratio: EQ, filters, dry/wet, IR content, models, input trim, tube knobs) is
    // unchanged — a context edit invalidates all routes (stale snap = a pump risk; consilium).
    struct LevelContext                        // the structural params the ratio depends on
    {
        float inputGainDb = 0.0f; float mixAB01 = 0.0f;
        bool  aLoaded = false, bLoaded = false;
        bool  monoAmp = false;                 // amp-lane fold: changes the levelled signal → route makeup depends on it
        EqParams   eq;
        TubeParams tube;
        SlotParams slotA, slotB;               // incl. filters / dryWet / phase / mute
        bool operator== (const LevelContext&) const = default;
    };
    // QUANTIZED snapshot: exact float compare would re-stale the caches on every block of a
    // smoothed automation ramp or mere MIDI-CC jitter, silently disabling the snap in the very
    // hands-on A/B workflow it serves (review finding). The grids are far below audibility for
    // the makeup ratio (0.05 dB / 0.005 of a 0..1 amount / 1 Hz) but swallow LSB noise.
    static LevelContext makeContext (const Params& p) noexcept
    {
        auto qDb = [] (float v) { return std::round (v * 20.0f) / 20.0f; };   // 0.05 dB grid
        auto q01 = [] (float v) { return std::round (v * 200.0f) / 200.0f; }; // 0.005 grid
        auto qHz = [] (float v) { return std::round (v); };                   // 1 Hz grid
        LevelContext c;
        c.inputGainDb = qDb (p.inputGainDb); c.mixAB01 = q01 (p.mixAB01);
        c.aLoaded = p.aLoaded; c.bLoaded = p.bLoaded; c.monoAmp = p.monoAmp;
        c.eq = p.eq; c.tube = p.tube; c.slotA = p.slot[0]; c.slotB = p.slot[1];
        c.eq.bassDb = qDb (c.eq.bassDb); c.eq.midDb = qDb (c.eq.midDb);
        c.eq.trebleDb = qDb (c.eq.trebleDb); c.eq.presenceDb = qDb (c.eq.presenceDb);
        c.eq.hpfHz = qHz (c.eq.hpfHz); c.eq.lpfHz = qHz (c.eq.lpfHz);
        c.tube.driveDb = qDb (c.tube.driveDb); c.tube.outputDb = qDb (c.tube.outputDb);
        c.tube.autoComp = q01 (c.tube.autoComp);
        c.tube.sag = q01 (c.tube.sag); c.tube.presence = q01 (c.tube.presence);
        c.tube.depth = q01 (c.tube.depth); c.tube.load = q01 (c.tube.load);
        c.tube.iron = q01 (c.tube.iron); c.tube.bias = q01 (c.tube.bias);
        for (SlotParams* s : { &c.slotA, &c.slotB })
        {
            s->hpfHz = qHz (s->hpfHz); s->lpfHz = qHz (s->lpfHz);
            s->dryWet01 = q01 (s->dryWet01);
        }
        return c;
    }
    static int routeIndex (int routeCode) noexcept { return (routeCode >= 100 ? 3 : 0) + routeCode % 100; }

    struct RouteLevel { float makeupDb = 0.0f; juce::uint32 gen = 0; bool valid = false; };
    RouteLevel   routeLevel[6];                // {preamp off/on} × {off, capture, tube}
    // Learned makeup DELTAS between route pairs (dB, [to][from]). Unlike the caches they SURVIVE
    // context changes: the A-vs-B spectral offset is largely IR/EQ-portable, so a first visit
    // after a change snaps to (current + delta) — near the truth — instead of fading from scratch.
    float        pairDeltaDb[6][6] {};
    bool         pairDeltaKnown[6][6] {};
    LevelContext prevContext;
    juce::uint32 contextGen = 0;               // audio-side: bumped on any context change
    std::atomic<juce::uint32> modelGeneration { 0 };   // message-thread bumps (IR / model loads)
    juce::uint32 lastModelGeneration = 0;
    juce::uint64 lastAmpBytesHash = 0, lastPreampBytesHash = 0;   // identical-reload guards (message thread)
    int          routeDwellSamples = 0;        // settled time in the current route
    bool         prevAutoLevel = true;         // autoLevel flip ⇒ the leveler glides ⇒ re-dwell before trusting writes
    static constexpr double kRouteDwellSeconds = 0.4;  // followers ~converged before a cache is trusted

    std::atomic<float> inLevel  { 0.0f };
    std::atomic<float> outLevel { 0.0f };

    felitronics::analysis::SpectrumTap preTap, postTap;
    std::atomic<bool> spectrumActive { false };

    // DSP load meter state. pct[5] = {total, preamp, eq, poweramp, cab}. accumulateLoads() EMA-
    // smooths and publishes to cpuPct (the GUI reads those). cpuSm is the smoother's running state.
    void accumulateLoads (const double pct[5]) noexcept;
    std::atomic<float> cpuPct[5] { { 0.0f }, { 0.0f }, { 0.0f }, { 0.0f }, { 0.0f } };
    double cpuSm[5] { 0.0, 0.0, 0.0, 0.0, 0.0 };
};

} // namespace cab
