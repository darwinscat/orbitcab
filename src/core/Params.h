// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

//==============================================================================
// cab::Params — one audio block's worth of parameter values, as plain numbers.
//
// This is the seam between the JUCE adapter and the headless DSP core: the
// adapter reads APVTS atomics and fills this POD; CabEngine::process consumes it.
// Deliberately free of JUCE, APVTS, atomics, files — so the same struct compiles
// under Emscripten / embedded. Plain values only.
//==============================================================================
namespace cab
{

struct SlotParams
{
    bool  hpfOn    = false;
    bool  lpfOn    = false;
    bool  phase    = false;   // invert polarity of the wet branch
    bool  mute     = false;
    float hpfHz    = 80.0f;
    float lpfHz    = 7000.0f;
    float dryWet01 = 1.0f;    // 0 = dry, 1 = full wet (the "mix" param / 100)
};

struct Params
{
    float inputGainDb  = 0.0f;
    float outputGainDb = 0.0f;
    float mixAB01      = 0.0f;   // 0 = slot A, 1 = slot B (the "mixAB" param / 100)
    bool  bypass       = false;
    bool  autoLevel    = true;
    bool  bLoaded      = false;  // slot B currently has an IR (gates B + MIX)

    SlotParams slot[2];          // [0] = A, [1] = B
};

} // namespace cab
