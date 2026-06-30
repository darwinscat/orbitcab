// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <cstddef>
#include <memory>

//==============================================================================
// cab::poweramp::TubePowerAmp — a white-box ANALYTIC tube power-amp stage that runs IN
// FRONT of the cab convolution, as the "Tube" alternative to the NAM capture (cab::AmpStage)
// at the same poweramp seam. Same "seam" discipline as cab::AmpStage / cab::Convolver: the
// public API speaks raw float* + sizes and the implementation is hidden behind a pImpl — so
// the (future) DSP (oversampler, PP/SE waveshaper, sag, NFB loop, virtual load) never leaks
// into the rest of the Core, the stage stays JUCE-free, and it can later lift verbatim into a
// felitronics-core module. See src/poweramp/PLAN.md.
//
// 🔴 RT rule: process() never allocates, locks, does IO or throws. All state/buffers are
// heap-allocated in prepare() behind the pImpl, NOT by-value members — so the by-value engine
// member stays tiny and Windows' 1 MB audio-thread stack is never at risk (the MSVC rule).
//
// SCAFFOLD (block 1): process() is a strict no-op passthrough and latencySamples() == 0. The
// DSP blocks land later WITHOUT changing this header — the contract is the stable seam.
//==============================================================================
namespace cab::poweramp
{

class TubePowerAmp
{
public:
    TubePowerAmp();
    ~TubePowerAmp();

    // Allocate state for this stream / block size. Message/host thread (prepareToPlay) —
    // never the audio thread (later blocks build oversamplers / tables here).
    void prepare (double sampleRate, int maxBlock);
    void reset();

    // 🔴 RT-safe, in place on planar channels. Scaffold: clean passthrough (no-op).
    void process (float* const* io, int numChannels, int numSamples) noexcept;

    // Host-rate latency (0 until oversampling lands in a later block).
    int  latencySamples() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cab::poweramp
