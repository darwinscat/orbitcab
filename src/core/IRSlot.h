// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cstddef>

#include "Convolver.h"

//==============================================================================
// cab::IRSlot — one full IR channel: in -> HPF -> LPF -> Convolution. Owns the
// per-slot filters + convolver (audio thread) AND the decoded IR buffer + the
// TRIM / HEAD-trim math (message thread). Two instances (A/B) back the two slots.
//
// 🔴 RT rule: processWet() never allocates/locks/IO/throws. setOriginalIR()/applyTrim()
// run on the message thread; applyTrim rebuilds off-thread + atomic-swaps via Convolver.
//==============================================================================
namespace cab
{

class IRSlot
{
public:
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset();

    //--- IR lifecycle (message thread) ---------------------------------------
    // Store a freshly-decoded (planar) IR. Detects leading silence and resets the
    // reload-coalescing; does NOT touch the convolver — call applyTrim() to push it.
    void setOriginalIR (const float* const* samples, int numChannels, int numSamples, double irSampleRate);
    void clearOriginal();
    bool hasOriginal() const { return original.getNumSamples() > 0; }

    // Bundled fallback: decode encoded bytes (adapter-level) and load with the energy-norm; no
    // original buffer, no trim. Rare — only if the normal sample decode failed upstream.
    void loadBytesFallback (const void* data, size_t size);

    // Retry a reload that the convolver rejected while mid-crossfade (coalescing — the engine accepts
    // once idle). Call periodically from the message-thread reload poll. No-op when nothing is pending.
    void pumpReload() { conv.flushPending(); }

    // Rebuild the truncated (trimOn + fraction) / head-trimmed (headOn skips the detected
    // leading silence) IR from the original, ~2 ms fade on the cut, atomic-swap into the
    // convolver. Coalesces identical reloads. Returns the resulting IR length in seconds.
    double applyTrim (bool trimOn, float trimFraction01, bool headOn);
    double trimmedLengthSeconds() const;

    //--- RT-safe wet path -----------------------------------------------------
    void processWet (juce::AudioBuffer<float>& wetDst, const juce::AudioBuffer<float>& src,
                     int numChannels, int numSamples, bool hpfOn, float hpfHz, bool lpfOn, float lpfHz);

    //--- message-thread accessors (for the embedded-IR pool) -------------
    const juce::AudioBuffer<float>& originalBuffer() const { return original; }
    double originalSampleRate() const { return irSampleRate; }

private:
    Convolver conv;
    juce::dsp::StateVariableTPTFilter<float> hpf, lpf;
    bool prevHpfOn = false, prevLpfOn = false;   // last processWet on-state — reset a filter on re-enable

    juce::AudioBuffer<float> original;     // full decoded IR (message-thread only)
    double irSampleRate   = 44100.0;
    int    leadSilence    = 0;             // detected leading-silence samples (HEAD trim)
    int    lastTrimSamples = -1;           // coalesce identical reloads
    int    lastHeadStart   = -1;
};

} // namespace cab
