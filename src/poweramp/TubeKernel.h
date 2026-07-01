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
};

// Brand-neutral voicing presets {0=6L6, 1=EL34, 2=EL84, 3=KT88}. evenLeak = 0 (exact PP cancellation).
// Sag/NFB: 6L6/KT88 stiff (usually SS-rectified); EL84/EL34 spongy (chime/vintage). Tune by ear.
inline constexpr TubeVoicing kTubeVoicings[4] = {
    /* 6L6  */ { 0.85f, 2.0f, 0.18f, 0.30f, 0.0f,   8.0f, 120.0f, 0.22f, 0.04f,   3000.0f, 6.0f, 100.0f, 6.0f, 0.5f },
    /* EL34 */ { 1.10f, 2.6f, 0.28f, 0.22f, 0.0f,   8.0f, 170.0f, 0.30f, 0.05f,   2800.0f, 6.0f, 110.0f, 6.0f, 0.6f },
    /* EL84 */ { 1.40f, 3.2f, 0.33f, 0.18f, 0.0f,  10.0f, 240.0f, 0.42f, 0.06f,   3200.0f, 6.5f, 120.0f, 6.5f, 0.7f },
    /* KT88 */ { 0.70f, 1.7f, 0.12f, 0.34f, 0.0f,   6.0f, 100.0f, 0.18f, 0.03f,   2600.0f, 5.5f,  90.0f, 5.5f, 0.4f },
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
