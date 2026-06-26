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
#include "AutoLeveler.h"
#include "SpectrumTap.h"

//==============================================================================
// cab::CabEngine — the headless DSP core. Owns the whole real-time signal path:
//
//   global (front): in → [PREAMP (NAM)] → [POWERAMP (NAM)]  — two optional neural stages, in order
//   per slot (A/B):  → HPF → LPF → Convolution → Phase → Dry/Wet(blend the amp output)
//   then:            MIX crossfades the two slot outputs → Auto-level → Output Gain
//
// The two NAM stages run before the dry tap, so the dry/wet blend + the auto-level reference are
// their output (the thing being cab'd), not the clean DI. The preamp feeds the poweramp. Both are
// the same cab::AmpStage class — `preamp` is just a second instance ahead of `amp`.
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
    static constexpr int fftOrder = kSpectrumFftOrder;
    static constexpr int fftSize  = kSpectrumFftSize;

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
    void  setSpectrumActive (bool shouldFeed) { spectrumActive.store (shouldFeed, std::memory_order_relaxed); }
    bool  pullSpectrum (bool pre, float* destFftSize);

    double sampleRate() const { return currentSampleRate; }

private:
    AmpStage    preamp;                    // optional NAM preamp, runs first (feeds the poweramp)
    AmpStage    amp;                       // optional NAM poweramp, front of the cab
    IRSlot      slot[2];
    AutoLeveler autoLeveler;
    juce::AudioBuffer<float> wet[2];       // per-slot convolution scratch
    juce::AudioBuffer<float> dryBuffer;    // copy of the raw input for the dry/wet blend

    // Smoothed so live tweaks / automation don't zipper. Phase is a sign (+1/-1) ramped
    // through 0 — a brief dip rather than a hard polarity click.
    juce::SmoothedValue<float> mixSm[2]   { { 1.0f }, { 1.0f } };
    juce::SmoothedValue<float> phaseSm[2] { { 1.0f }, { 1.0f } };
    juce::SmoothedValue<float> mixABSmoothed { 0.0f };
    juce::SmoothedValue<float> gainSmoothed  { 1.0f };
    juce::SmoothedValue<float> muteGateSmoothed { 1.0f };

    double currentSampleRate = 44100.0;

    float inputGainPrev = 1.0f;            // block-ramp start for zipper-free input trim
    std::atomic<float> inLevel  { 0.0f };
    std::atomic<float> outLevel { 0.0f };

    SpectrumTap preTap, postTap;
    std::atomic<bool> spectrumActive { false };
};

} // namespace cab
