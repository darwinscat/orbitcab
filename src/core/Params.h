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

// Amp tone EQ — a fixed-frequency tone stack (Bass/Mid/Treble) + a Presence shelf + an
// optional HPF/LPF "tightening" pair. Runs BETWEEN the preamp and poweramp NAM stages, so
// its cuts shape what hits the poweramp's nonlinearity — distinct from SlotParams' per-slot
// HPF/LPF, which shape the cab/IR band AFTER the whole amp. Tone gains are dB (0 = flat).
struct EqParams
{
    bool  on         = false;   // master gate for the whole stage (off = bit-exact passthrough)
    float bassDb     = 0.0f;
    float midDb      = 0.0f;
    float trebleDb   = 0.0f;
    float presenceDb = 0.0f;
    bool  hpfOn      = false;
    float hpfHz      = 80.0f;
    bool  lpfOn      = false;
    float lpfHz      = 10000.0f;
};

// Which power-amp runs at the poweramp seam when `ampOn` is true: the NAM capture
// (cab::AmpStage, default) or the white-box analytic tube stage (cab::poweramp::TubePowerAmp).
// `ampOn` stays the master gate (off => neither runs) — this only picks between the two.
enum class PowerAmpMode { capture, tube };

// White-box tube power-amp controls (cab::poweramp::TubePowerAmp). Only consumed in Tube mode.
// A plain POD like EqParams — JUCE-free, embedded-safe. tubeType indexes a voicing preset
// {0=6L6, 1=EL34, 2=EL84, 3=KT88}; singleEnded picks SE class-A vs (default) push-pull AB.
struct TubeParams
{
    float driveDb     = 0.0f;    // input pre-gain into the tube nonlinearity
    float outputDb    = 0.0f;    // post-stage make-up / trim
    int   tubeType    = 0;       // 0=6L6, 1=EL34, 2=EL84, 3=KT88 (voicing coefficient preset)
    bool  singleEnded = false;   // false = push-pull class AB, true = single-ended class A
    float autoComp    = 1.0f;    // drive-compensation amount (1 = small-signal unity → clean stays clean)
};

struct Params
{
    float inputGainDb  = 0.0f;
    float outputGainDb = 0.0f;
    float mixAB01      = 0.0f;   // 0 = slot A, 1 = slot B (the "mixAB" param / 100)
    bool  bypass       = false;
    bool  preampOn     = false;  // run the NAM preamp stage first (input → PREAMP → POWERAMP → cab)
    bool  ampOn        = false;  // run the NAM poweramp stage in front of the cab
    PowerAmpMode powerAmpMode = PowerAmpMode::capture;  // when ampOn: capture (NAM) vs tube (white-box)
    bool  autoLevel    = true;
    bool  aLoaded      = true;   // slot A has an IR; false = empty (no cab → dry passthrough on A)
    bool  bLoaded      = false;  // slot B currently has an IR (gates B + MIX)

    EqParams   eq;               // amp tone EQ, between the preamp and poweramp NAM stages
    TubeParams tube;             // white-box tube poweramp controls (Tube mode only)
    SlotParams slot[2];          // [0] = A, [1] = B
};

} // namespace cab
