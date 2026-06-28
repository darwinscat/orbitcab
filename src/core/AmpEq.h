// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <teq/EqEngine.h>

#include "Params.h"

//==============================================================================
// cab::AmpEq — the amp tone stack. A fixed-frequency Bass/Mid/Treble + a Presence shelf +
// an optional HPF/LPF "tightening" pair, built on teq::EqEngine (matched biquads, RT-safe,
// JUCE-free, zero-alloc).
//
// Position: runs BETWEEN the preamp and poweramp NAM stages (input → PREAMP → EQ → POWERAMP
// → cab). So its cuts shape what hits the poweramp's nonlinearity — deliberately DISTINCT
// from cab::IRSlot's per-slot HPF/LPF, which shape the cab/IR band AFTER the whole amp.
//
// Tone-stack corners are fixed (a generic, musical voicing). They're public constexpr so a
// later GUI curve can mirror them, and a future step can make them per-model from a measured
// tone-stack sweep without touching the chain.
//
// 🔴 Real-time rule: process() never allocates, locks, does IO, or throws. prepare() does all
// allocation (teq's state is fixed-size). Map params from the audio thread (same place the
// adapter reads its atomics) — teq takes no internal lock.
//==============================================================================
namespace cab
{

class AmpEq
{
public:
    // Fixed tone-stack voicing. Public so the GUI can draw a matching curve; later these can
    // become per-model (fit from a low-level tone-stack frequency sweep of the real unit).
    static constexpr double kBassHz     = 100.0;     // low shelf
    static constexpr double kMidHz      = 600.0;     // bell
    static constexpr double kMidQ       = 0.8;
    static constexpr double kTrebleHz   = 2800.0;    // high shelf
    static constexpr double kPresenceHz = 5000.0;    // high shelf, above treble so they stay distinct
    static constexpr double kCutQ       = 0.707;     // Butterworth (HPF/LPF use the 12 dB/oct cascade)

    void prepare (double sampleRate, int /*maxBlock*/, int numChannels) noexcept
    {
        eng.prepare (sampleRate, 0, numChannels);
    }

    void reset() noexcept
    {
        eng.reset();
        wasOn = false;
    }

    // 🔴 RT-safe, in place. eq.on == false → bit-exact passthrough (no processing). Tone bands
    // self-disable at 0 dB (a flat shelf costs nothing); HPF/LPF follow their own toggles.
    void process (float* const* io, int numChannels, int numSamples, const EqParams& eq) noexcept
    {
        if (! eq.on)
        {
            if (wasOn) { eng.reset(); wasOn = false; }   // clear tails once so re-enabling can't pop
            return;
        }
        wasOn = true;

        using FT = teq::FilterType;
        auto set = [this] (int i, bool on, FT type, double freq, double gainDb, double Q) noexcept
        {
            teq::BandParams b;                 // defaults: off, slope 12, not swept (→ matched 12 dB/oct cascade for cuts)
            b.on = on; b.type = type; b.freq = freq; b.gainDb = gainDb; b.Q = Q;
            eng.setBand (i, b);
        };

        set (0, eq.hpfOn,               FT::HighPass,  eq.hpfHz,    0.0,            kCutQ);
        set (1, eq.bassDb     != 0.0f,  FT::LowShelf,  kBassHz,     eq.bassDb,      kCutQ);
        set (2, eq.midDb      != 0.0f,  FT::Bell,      kMidHz,      eq.midDb,       kMidQ);
        set (3, eq.trebleDb   != 0.0f,  FT::HighShelf, kTrebleHz,   eq.trebleDb,    kCutQ);
        set (4, eq.presenceDb != 0.0f,  FT::HighShelf, kPresenceHz, eq.presenceDb,  kCutQ);
        set (5, eq.lpfOn,               FT::LowPass,   eq.lpfHz,    0.0,            kCutQ);

        eng.process (io, numChannels, numSamples);
    }

private:
    teq::EqEngine eng;
    bool wasOn = false;   // last process saw eq.on — so we reset filter tails exactly once on disable
};

} // namespace cab
