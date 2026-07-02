// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <felitronics/saturation/WaveShaper.h>

//==============================================================================
// cab::poweramp — the PURE tube transfer math, factored out of TubePowerAmp so the static
// curve can be unit-tested DIRECTLY (exact PP odd-symmetry, exact drive-comp unity, slope,
// boundedness) with no oversampling, no DFT, no JUCE. Header-only, std + felitronics only
// (embedded-safe). The shipping stage (TubePowerAmp) drives this same code, so the tests
// exercise the real kernel, not a parallel reimplementation.
//
//   SE (single-ended class A): y = g(u)            → asymmetric, even-harmonic rich
//   PP (push-pull class AB):   y = g(u+Vb) − g(−u+Vb)  → ODD by construction (even cancel)
// g = felitronics::saturation::WaveShaper(Asym) = (tanh(k(t+b)) − tanh(kb)) peak-normalised.
// `topoMix` blends PP(0) ↔ SE(1). The per-half bias mismatch `evenLeak` (0 in block 2) is the
// only thing that breaks PP's exact even-cancellation — by construction, so tests can assert it.
//==============================================================================
namespace cab::poweramp
{

struct TubeVoicing
{
    float driveScale, k, bSE, vbPP, evenLeak;                        // block 2: waveshaper voicing
    // block 3 "feel" — per-tube-fixed sag + NFB-voicing character:
    float sagFastMs, sagRecoveryMs, sagMaxDroop, sagBiasDepth;       // sag: attack, recovery, max rail collapse; sagBiasDepth reserved (future per-sample bias)
    float presenceHz, presenceMaxDb, depthHz, depthMaxDb, nfbOpen;   // shelves + how much they "open" under sag/drive
    float midHz, midDb, midQ;                                        // static MID bell — the amp fingerprint (scoop vs push)
    float levelTrimDb;                                               // static per-voicing output trim (dB) that centres this voicing on
                                                                     // ~the dry level at the calibration Drive (18 dB) and equalises the four
                                                                     // voicings to each other. Base values measured on a real guitar DI; then
                                                                     // corrected BY EAR (Oleh, 2026-07-02): PP 6L6 +0 / EL34 −5 / EL84 −7 /
                                                                     // KT88 +3 folded in (see PowerAmpRouter).
    float levelTrimSEDb;                                             // + this extra trim in single-ended (x1) mode — SE's asymmetry shifts
                                                                     // the level per voicing differently than push-pull (measured; by-ear SE
                                                                     // corrections 6L6 −1 / EL34 −6 / EL84 −3 / KT88 −2 folded in as SE−PP).
    // block 4 — VIRTUAL LOAD (reactive-speaker impedance pre-EQ, BEFORE the nonlinearity + the sag detector):
    float loadResHz, loadResQ, loadResDb;                            // LF cone-resonance impedance peak (Bell) — peaks the DRIVE ~80-110 Hz.
    float loadRiseHz, loadRiseDb;                                    // HF inductive impedance rise (HighShelf, Q 0.707) — lifts the drive
                                                                     // above ~1.3-1.8 kHz. Together = "amp into a real speaker": frequency-
                                                                     // dependent break-up (0 dB gains ⇒ stage is byte-identical to block 3).
    // block 4 — OUTPUT TRANSFORMER (OS domain, AFTER the nonlinearity): LF core saturation + HF leakage rolloff.
    float otLfHz, otSatK, otHfHz;                                    // LF split-band corner (Hz) + core-saturation drive (tanh k) + HF
                                                                     // leakage-rolloff corner (Hz). The Iron knob scales the AMOUNT (0 = bypass).
};

// Brand-neutral voicing presets {0=6L6, 1=EL34, 2=EL84, 3=KT88}. Archetype-voiced (crew-designed, tune by
// ear): the MID bell is the fingerprint — 6L6 American scoop, EL34 British mid-push, EL84 Vox upper-mid
// chime, KT88 near-flat hi-fi. 6L6/KT88 stiff sag (SS-rectified), EL84/EL34 spongier. evenLeak = 0 for now
// (exact PP cancellation held; per-tube even-harmonic leak is a later liveliness pass — see DESIGN-block3.md).
inline constexpr TubeVoicing kTubeVoicings[4] = {
    /* 6L6  American: scooped mids, deep tight lows, smooth (not sharp) top */
    { 0.82f, 2.3f, 0.25f, 0.30f, 0.0f,   8.0f, 130.0f, 0.20f, 0.40f,   5000.0f, 3.0f,  95.0f, 7.5f, 0.45f,    500.0f, -5.0f, 0.70f,   3.9f, -0.3f,    90.0f, 1.6f, 4.0f,  1600.0f, 3.0f,   150.0f, 1.8f, 8000.0f },
    /* EL34 British: forward mids, aggressive upper-mid crunch */
    { 1.12f, 3.2f, 0.15f, 0.20f, 0.0f,   6.0f,  90.0f, 0.22f, 0.50f,   3400.0f, 5.0f, 118.0f, 4.0f, 0.60f,    680.0f,  4.5f, 0.70f,    0.7f,  0.1f,   100.0f, 1.8f, 3.5f,  1500.0f, 4.0f,   170.0f, 2.2f, 7000.0f },
    /* EL84 Vox chime: bright upper-mid, early soft breakup, heavy spongy sag */
    { 1.45f, 1.7f, 0.45f, 0.18f, 0.0f,  14.0f, 240.0f, 0.42f, 0.70f,   5500.0f, 4.0f, 130.0f, 3.0f, 0.75f,   1600.0f,  4.0f, 0.70f,    0.2f,  0.4f,   110.0f, 2.0f, 4.0f,  1800.0f, 4.5f,   210.0f, 2.6f, 8500.0f },
    /* KT88 hi-fi: huge tight lows + a low resonance "thump", near-flat mids, clean late breakup, stiff supply.
       The "mid" bell is repurposed LOW (100 Hz, resonant Q) — KT88's mids are near-flat, so it buys the
       Hiwatt/Ampeg low-end resonance the ear wants; the depth low-shelf below it adds broad extension. */
    { 0.60f, 3.8f, 0.05f, 0.34f, 0.0f,   4.0f,  80.0f, 0.12f, 0.30f,   4800.0f, 4.0f,  68.0f, 10.0f, 0.30f,    100.0f,  5.0f, 1.40f,  -1.5f,  4.0f,    80.0f, 1.4f, 4.0f,  1300.0f, 2.5f,   120.0f, 1.4f, 10000.0f },
};

// Stateless composite tube transfer. configure() once per block from (smoothed) coeffs; at()
// per (oversampled) sample. slopeAtZero() is the composite small-signal slope incl. the pre-gain.
class TubeStage
{
public:
    void configure (float k, float biasSE, float vbPP, float evenLeak, float topoMix) noexcept
    {
        using Shape = felitronics::saturation::WaveShaper::Shape;
        vb_ = vbPP; topo_ = topoMix;
        se_.setShape  (Shape::Asym); se_.setDrive  (k); se_.setBias  (biasSE);
        pos_.setShape (Shape::Asym); pos_.setDrive (k); pos_.setBias ( evenLeak);
        neg_.setShape (Shape::Asym); neg_.setDrive (k); neg_.setBias (-evenLeak);
    }

    float at (float u) const noexcept
    {
        const float se = se_.processSample (u);
        const float pp = pos_.processSample (u + vb_) - neg_.processSample (-u + vb_);
        return (1.0f - topo_) * pp + topo_ * se;
    }

    // Per-sample PP-bias overload (block 4 dynamic bias-shift): `vbDelta` drifts the push-pull operating
    // point sample-by-sample (toward class-B under sag → crossover bloom), with NO per-block reconfigure
    // (which would break block-size determinism). SE (class A) has no crossover, so it stays unmodulated.
    // vbDelta == 0 is bit-identical to at(u). The shaper is memoryless, so re-evaluating it is free of state.
    float at (float u, float vbDelta) const noexcept
    {
        const float se = se_.processSample (u);
        const float vb = vb_ + vbDelta;
        const float pp = pos_.processSample (u + vb) - neg_.processSample (-u + vb);
        return (1.0f - topo_) * pp + topo_ * se;
    }

    // d/dx at(G·x) at x=0 — numeric central difference (topology-agnostic: covers PP's 2-eval
    // differential and the pre-gain G in one go, which felitronics' per-curve slopeAtZero cannot).
    float slopeAtZero (float G) const noexcept
    {
        const float d = 1.0e-4f;
        return (at (G * d) - at (-G * d)) / (2.0f * d);
    }

private:
    felitronics::saturation::WaveShaper se_, pos_, neg_;
    float vb_ = 0.30f, topo_ = 0.0f;
};

} // namespace cab::poweramp
