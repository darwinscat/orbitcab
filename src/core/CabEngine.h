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
#include "AmpEq.h"
#include "AutoLeveler.h"
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
    bool   loadAmpModelBytes  (const void* data, std::size_t size, float trimDb = 0.0f) { return amp.loadModelFromMemory (data, size, trimDb); }
    void   clearAmpModel()                              { amp.clearModel(); }
    void   collectAmpGarbage()                          { amp.collectGarbage(); preamp.collectGarbage(); }
    bool   ampHasModel()         const                  { return amp.hasModel(); }
    double ampModelSampleRate()  const                  { return amp.modelSampleRate(); }
    double ampModelLoudness()    const                  { return amp.modelLoudness(); }
    bool   ampModelHasLoudness() const                  { return amp.modelHasLoudness(); }
    int    ampLatencySamples()   const                  { return amp.latencySamples(); }

    bool   loadPreampModelBytes (const void* data, std::size_t size, float trimDb = 0.0f) { return preamp.loadModelFromMemory (data, size, trimDb); }
    void   clearPreampModel()                           { preamp.clearModel(); }
    bool   preampHasModel()        const                { return preamp.hasModel(); }
    int    preampLatencySamples()  const                { return preamp.latencySamples(); }

    //--- cross-thread reads for the GUI ------------------------------------------
    float inputLevel()  const { return inLevel.load  (std::memory_order_relaxed); }
    float outputLevel() const { return outLevel.load (std::memory_order_relaxed); }

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
    AmpStage    preamp;                    // optional NAM preamp, runs first (feeds the EQ → poweramp)
    AmpEq       ampEq;                      // amp tone stack, between the preamp and poweramp NAM stages
    AmpStage    amp;                       // optional NAM poweramp, front of the cab
    IRSlot      slot[2];
    AutoLeveler autoLeveler;
    juce::AudioBuffer<float> wet[2];       // per-slot convolution scratch
    juce::AudioBuffer<float> dryBuffer;    // copy of the raw input for the dry/wet blend

    // Smoothed so live tweaks / automation don't zipper. Phase is a sign (+1/-1) ramped
    // through 0 — a brief dip rather than a hard polarity click.
    felitronics::core::LinearSmoother mixSm[2]   { { 1.0f }, { 1.0f } };
    felitronics::core::LinearSmoother phaseSm[2] { { 1.0f }, { 1.0f } };
    felitronics::core::LinearSmoother mixABSmoothed { 0.0f };
    felitronics::core::LinearSmoother gainSmoothed  { 1.0f };
    felitronics::core::LinearSmoother muteGateSmoothed { 1.0f };

    double currentSampleRate = 44100.0;

    float inputGainPrev = 1.0f;            // block-ramp start for zipper-free input trim
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
