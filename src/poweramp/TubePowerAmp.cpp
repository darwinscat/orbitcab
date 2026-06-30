// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "TubePowerAmp.h"

namespace cab::poweramp
{

// All real state lives here, on the heap (allocated in prepare) — so process() is alloc-free
// and the by-value TubePowerAmp member never grows the audio-thread stack (MSVC 1 MB rule).
// Block 1 carries only the stream config; the DSP fields (oversampler, waveshaper, sag, NFB,
// virtual load) get added here WITHOUT touching the public header.
struct TubePowerAmp::Impl
{
    double sampleRate = 0.0;
    int    maxBlock   = 0;
};

TubePowerAmp::TubePowerAmp() : impl (std::make_unique<Impl>()) {}
TubePowerAmp::~TubePowerAmp() = default;

void TubePowerAmp::prepare (double sampleRate, int maxBlock)
{
    impl->sampleRate = sampleRate;
    impl->maxBlock   = maxBlock;
}

void TubePowerAmp::reset() {}

void TubePowerAmp::process (float* const* io, int numChannels, int numSamples) noexcept
{
    // Scaffold: strict no-op passthrough. The signal is already in `io`; we touch nothing,
    // so the stage is bit-exact identity (proved by tests/PowerAmpScaffoldGolden.cpp).
    (void) io;
    (void) numChannels;
    (void) numSamples;
}

int TubePowerAmp::latencySamples() const noexcept { return 0; }

} // namespace cab::poweramp
