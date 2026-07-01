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
// BLOCK 2: an oversampled push-pull / single-ended tube waveshaper with per-tube voicings and
// Drive/Output/Tube/Topology controls (set via setParams). Sag, NFB loop, virtual load and the
// output transformer are LATER blocks. latencySamples() is now the oversampler round-trip (31).
//==============================================================================
namespace cab { struct TubeParams; }   // fwd-decl — the JUCE-free control POD (core/Params.h)

namespace cab::poweramp
{

class TubePowerAmp
{
public:
    TubePowerAmp();
    ~TubePowerAmp();

    // Allocate state for this stream / block size. Message/host thread (prepareToPlay) — never
    // the audio thread. `oversampleFactor` defaults to 4 (the shipping value); a test may pass a
    // higher factor (e.g. 32) to build an alias-free reference for null comparison. Latency is
    // tpp-1 regardless of factor, so 4x and 32x stay sample-aligned.
    void prepare (double sampleRate, int maxBlock, int oversampleFactor = 4);
    void reset();

    // Set the tube controls (Drive/Output/Tube/Topology). RT-safe: stores targets only; the
    // coefficients are smoothed inside process(). Called once per block before process().
    void setParams (const cab::TubeParams& params) noexcept;

    // 🔴 RT-safe, in place on planar channels: oversampled PP/SE tube waveshaper.
    void process (float* const* io, int numChannels, int numSamples) noexcept;

    // Host-rate latency = the oversampler round-trip (tpp-1), constant across drive/tube/topology.
    int  latencySamples() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cab::poweramp
