// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <teq/EqEngine.h>

#include "Params.h"

#include <memory>

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
// 🔴 Real-time rule: process() never allocates, locks, does IO, or throws. prepare() builds the
// engine on the HEAP — teq::EqEngine is ~200 KB (a 24-band bank), so a by-value member would bloat
// every host that embeds CabEngine and overflow MSVC's 1 MB main-thread stack when the processor is
// stack-allocated (CI integration test). The heap pointer keeps the embedder tiny. Map params from
// the audio thread (same place the adapter reads its atomics) — teq takes no internal lock.
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
    static constexpr int    kNumBands   = 6;         // HPF, Bass, Mid, Treble, Presence, LPF

    // Fill out[kNumBands] from EqParams using the SAME mapping process() applies — so a GUI can draw
    // the exact response via teq::EqEngine::magnitudeDbFor(). Tone bands self-disable at 0 dB; HPF/LPF
    // follow their toggles. Ignores the master eq.on (the caller gates that). Pure / no state.
    static void describe (const EqParams& eq, teq::BandParams* out) noexcept
    {
        using FT = teq::FilterType;
        auto mk = [] (bool on, FT type, double freq, double gainDb, double Q) noexcept
        {
            teq::BandParams b; b.on = on; b.type = type; b.freq = freq; b.gainDb = gainDb; b.Q = Q;
            return b;
        };
        out[0] = mk (eq.hpfOn,               FT::HighPass,  eq.hpfHz,    0.0,            kCutQ);
        out[1] = mk (eq.bassDb     != 0.0f,  FT::LowShelf,  kBassHz,     eq.bassDb,      kCutQ);
        out[2] = mk (eq.midDb      != 0.0f,  FT::Bell,      kMidHz,      eq.midDb,       kMidQ);
        out[3] = mk (eq.trebleDb   != 0.0f,  FT::HighShelf, kTrebleHz,   eq.trebleDb,    kCutQ);
        out[4] = mk (eq.presenceDb != 0.0f,  FT::HighShelf, kPresenceHz, eq.presenceDb,  kCutQ);
        out[5] = mk (eq.lpfOn,               FT::LowPass,   eq.lpfHz,    0.0,            kCutQ);
    }

    void prepare (double sampleRate, int /*maxBlock*/, int numChannels)
    {
        if (! eng) eng = std::make_unique<teq::EqEngine>();   // message thread — allocation allowed here
        eng->prepare (sampleRate, 0, numChannels);
    }

    void reset() noexcept
    {
        if (eng) eng->reset();
        wasOn = false;
    }

    // 🔴 RT-safe, in place. eq.on == false → bit-exact passthrough (no processing). Tone bands
    // self-disable at 0 dB (a flat shelf costs nothing); HPF/LPF follow their own toggles.
    void process (float* const* io, int numChannels, int numSamples, const EqParams& eq) noexcept
    {
        if (! eq.on || eng == nullptr)
        {
            if (wasOn && eng) { eng->reset(); wasOn = false; }   // clear tails once so re-enabling can't pop
            return;
        }
        wasOn = true;

        teq::BandParams b[kNumBands];          // defaults give matched 12 dB/oct cuts for HPF/LPF
        describe (eq, b);                      // same mapping the GUI curve draws
        for (int i = 0; i < kNumBands; ++i) eng->setBand (i, b[i]);

        eng->process (io, numChannels, numSamples);
    }

private:
    std::unique_ptr<teq::EqEngine> eng;   // heap-held: ~200 KB, kept off the embedder's stack (see note above)
    bool wasOn = false;                   // last process saw eq.on — so we reset filter tails exactly once on disable
};

} // namespace cab
