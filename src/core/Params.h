// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

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

    bool operator== (const SlotParams&) const = default;   // context compare (leveler route memory)
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

    bool operator== (const EqParams&) const = default;   // context compare (leveler route memory)
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
    float autoComp    = 1.0f;    // full drive-compensation: small-signal unity ⇒ the tube tracks the INPUT level
                                 // deterministically (matched to capture, NO enable kick — no follower to chase). The
                                 // adapter's loudness-normalizer then boosts the ducked loud parts back → ~constant loudness.
    // block 3 "feel" layer (all [0,1] amounts; 0 = off ⇒ exact block-2 behaviour):
    float sag         = 0.0f;    // dynamic power-supply sag (bloom / touch / compression under load)
    float presence    = 0.0f;    // NFB-style HF voicing that opens up when pushed
    float depth       = 0.0f;    // NFB-style LF voicing that loosens when pushed
    // block 4:
    float load        = 0.0f;    // reactive-speaker VIRTUAL LOAD amount [0,1]: scales the per-voicing impedance
                                 // pre-EQ (LF cone-resonance peak + HF inductive rise) before the nonlinearity.
                                 // 0 = flat/off ⇒ exact block-3 behaviour; 1 = full per-voicing load.
    float iron        = 0.0f;    // OUTPUT-TRANSFORMER amount [0,1]: scales LF core saturation (low-note grind/
                                 // compression) + HF leakage rolloff, in the OS domain. 0 = bypass.
    float bias        = 0.0f;    // dynamic BIAS-SHIFT / bloom [0,1]: under sag the PP operating point drifts toward
                                 // class-B (crossover bloom / touch). Scales the per-voicing depth; needs Sag > 0.
    int   osIndex     = 1;       // OS-quality selector into the router's factor list {2,4,8,16,32}: 1 = 4× (default).
                                 // The router keeps one tube per factor prepared, so this switches live with no realloc.

    bool operator== (const TubeParams&) const = default;   // context compare (leveler route memory)
};

// In-amp spring reverb — a mono convolution spring tank, inserted AFTER the tone stack (EQ) and
// BEFORE the poweramp, exactly like a real amp's built-in reverb: the wet is summed with the amped
// guitar and then coloured by the poweramp + cab. NOT a clean stereo studio reverb. The engine only
// needs `type>0` (on) + `mix01` (return amount) here; WHICH spring is loaded into the reverb convolver
// is chosen by the adapter (it loads the matching bundled IR). type is a plain index: 0 = Off, 1..N =
// the Nth bundled spring — a change re-selects the IR off-thread (atomic swap), so the enum lives adapter-side.
struct ReverbParams
{
    int   type  = 0;       // 0 = Off (bypassed, zero CPU); 1..N = bundled reverb index
    float mix01 = 0.0f;    // reverb return amount [0,1] (the "Reverb Mix" param / 100) — parallel add, dry kept
    float scale01 = 0.15f; // wet-level calibration multiplier [0,1] — TEMP calibration knob (a spring/plate IR
                           // convolved with a sustained note accumulates a lot of tail energy, so the raw return
                           // is hot). Baked per-reverb once dialled in, then this knob is removed.

    bool operator== (const ReverbParams&) const = default;   // context compare (leveler route memory)
};

// In-amp NOISE GATE — a dual-detection (ISP Decimator "G-String" style) gate. The DETECTOR keys off
// the CLEAN post-trim input (before the preamp distortion, where the note envelope is un-compressed);
// the VCA attenuates AFTER the tone stack and BEFORE the spring-reverb send (so preamp hiss is killed
// but the spring tail rings out). Only the threshold is user-facing — attack/hold/release/hysteresis/
// floor are fixed inside cab::NoiseGate. Deliberately NOT part of the leveler LevelContext: the gate is
// a scale on the signal (dry + wet scale together), so the wet/dry makeup ratio is invariant to it.
struct GateParams
{
    bool  on          = false;
    float thresholdDb = -50.0f;   // open threshold (dBFS), referenced to the post-trim input level
    bool operator== (const GateParams&) const = default;
};

struct Params
{
    float inputGainDb  = 0.0f;
    float outputGainDb = 0.0f;
    float mixAB01      = 0.0f;   // 0 = slot A, 1 = slot B (the "mixAB" param / 100)
    bool  bypass       = false;
    bool  preampOn     = false;  // run the NAM preamp stage first (input → PREAMP → POWERAMP → cab)
    float preampVolumeDb = 0.0f; // post-preamp output volume (dB) — drives the EQ/poweramp/cab; before the dry tap
    bool  ampOn        = false;  // run the NAM poweramp stage in front of the cab
    PowerAmpMode powerAmpMode = PowerAmpMode::capture;  // when ampOn: capture (NAM) vs tube (white-box)
    bool  autoLevel    = true;
    bool  aLoaded      = true;   // slot A has an IR; false = empty (no cab → dry passthrough on A)
    bool  bLoaded      = false;  // slot B currently has an IR (gates B + MIX)
    bool  monoAmp      = false;  // input folded to one channel (Input = Left/Right) → run the amp
                                 // section (preamp/EQ/poweramp) on ONE lane + duplicate before the
                                 // cab (½ the NAM cost). false = true-stereo (v1). Set by the adapter.

    GateParams  gate;            // in-amp noise gate: detector on the clean input, VCA after EQ / before reverb send
    EqParams    eq;              // amp tone EQ, between the preamp and poweramp NAM stages
    ReverbParams reverb;         // in-amp spring reverb, between the EQ and poweramp (mono send/return)
    TubeParams  tube;            // white-box tube poweramp controls (Tube mode only)
    SlotParams  slot[2];         // [0] = A, [1] = B
};

} // namespace cab
