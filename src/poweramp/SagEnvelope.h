// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <algorithm>
#include <cmath>

//==============================================================================
// cab::poweramp::SagEnvelope — the pure, unit-testable power-supply SAG model (block 3). A
// FEEDFORWARD dual-time-constant follower on rectified DEMAND (|G·x|, mono-linked shared supply)
// → a droop in [0, dmax·amount]. Feedforward (not a post-output loop) is deliberate: it cannot
// self-oscillate (stability for free) and it gives "touch" — the pick transient punches through
// at full headroom for the first few ms before the supply droops, then squishes.
//
// FAST attack (rectifier + first-cap depletion, how fast B+ collapses) + SLOW recovery (reservoir
// recharge — the "bloom"; slow recovery is the whole game — fast recovery = a pumping compressor).
// The droop is soft-saturating in the demand envelope (soft knee, no threshold). The stage applies
// it as a shrinking "B+ rail" s = 1 − droop: y = s·shaper(u/s) — earlier breakup + lower ceiling =
// tube squish, not a clean VCA duck. droop=0 ⇒ the block-2 result is untouched.
//
// Pure: std only, no JUCE, no felitronics — header-only, embedded-safe, tested in isolation.
// Cross-referenced against OSS sag (SwankyAmp, Airwindows PowerSag) + textbook supply models; the
// physical plate/screen-split reservoir model (Amp Books) is a later refinement — see DESIGN-block3.md.
//==============================================================================
namespace cab::poweramp
{

class SagEnvelope
{
public:
    void prepare (double sampleRate) noexcept { fs_ = sampleRate > 0.0 ? sampleRate : 48000.0; reset(); }
    void reset() noexcept { env_ = 0.0f; }

    // Per-tube-fixed character. fastMs = attack (rectifier), recoveryMs = release (B+ bloom),
    // maxDroop = the tube's max rail collapse (stiff tube = small). amount01 = the user Sag knob.
    void setParams (float amount01, float fastMs, float recoveryMs, float maxDroop) noexcept
    {
        amount_   = std::clamp (amount01, 0.0f, 1.0f);
        maxDroop_ = std::clamp (maxDroop, 0.0f, 0.9f);
        aAtt_     = coeff (fastMs);
        aRel_     = coeff (recoveryMs);
    }

    // One baseband sample of rectified demand → droop in [0, maxDroop·amount]. Feedforward: derive
    // the droop from the envelope state, THEN advance the envelope with this sample (no algebraic
    // loop → zero added latency, unconditionally stable).
    inline float process (float demand) noexcept
    {
        const float droop = maxDroop_ * amount_ * (1.0f - std::exp (-2.0f * env_));   // soft-saturating
        const float d = std::fabs (demand);
        env_ += (d > env_ ? aAtt_ : aRel_) * (d - env_);                              // fast up / slow down
        return droop;
    }

    float droop() const noexcept { return maxDroop_ * amount_ * (1.0f - std::exp (-2.0f * env_)); }
    float envelope() const noexcept { return env_; }
    void  flushDenormals() noexcept { if (! std::isfinite (env_) || std::fabs (env_) < 1e-30f) env_ = 0.0f; }   // also zap NaN/Inf (poison guard)

private:
    float coeff (float ms) const noexcept
    {
        const double tau = std::max (1.0e-4, (double) ms * 1.0e-3);
        return (float) (1.0 - std::exp (-1.0 / (fs_ * tau)));
    }

    double fs_ = 48000.0;
    float  env_ = 0.0f;
    float  aAtt_ = 1.0f, aRel_ = 1.0e-3f;
    float  amount_ = 0.0f, maxDroop_ = 0.0f;
};

} // namespace cab::poweramp
