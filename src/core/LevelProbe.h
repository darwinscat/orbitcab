// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <cmath>
#include <cstdint>

//==============================================================================
// cab::levelprobe — the ONE deterministic reference stimulus used to measure a nonlinear
// stage's gain at a fixed operating point ("reference gain"). Used by:
//   • AmpStage — measures a just-loaded capture model's reference gain OFF-THREAD (before the
//     atomic swap), so the tube stage can be level-matched to the capture deterministically;
//   • the tube side — the baked kTubeRefGainDb constant in PowerAmpRouter.cpp was measured by
//     feeding THIS stimulus through the tube route (orbitcab_switch_probe `refprobe` section);
//   • tests/tools — cross-checks that both sides really used the same stimulus.
//
// The stimulus: LCG white noise through a one-pole low-pass at kShapeHz, scaled to kRefRmsDb.
// LP-shaped (not white) because guitar into a poweramp carries little energy above a few kHz —
// a white probe would overweight HF the stages never see and skew the measured gain. −18 dBFS
// RMS matches the level the NAM stages normalize toward, i.e. the seam's typical playing level.
// Integer-deterministic (LCG on the sample index): identical on every platform / run / rate.
//
// Pure std, header-only, no JUCE — safe for the core and for offline tools alike.
//==============================================================================
namespace cab::levelprobe
{

constexpr double kRefRmsDb  = -18.0;   // stimulus RMS (dBFS)
constexpr double kShapeHz   = 2000.0;  // one-pole LP corner — "guitar-band" shaping
constexpr double kSettleSec = 0.10;    // discard: stage + shaping-filter settle
constexpr double kMeasureSec = 0.20;   // measured window after the settle

// n-indexed white noise in [-1, 1] — same LCG family as the test benches.
inline float white (std::int64_t n) noexcept
{
    std::uint32_t s = (std::uint32_t) (n * 2654435761u + 1013904223u);
    s ^= s >> 15; s *= 2246822519u; s ^= s >> 13;
    return ((float) ((double) s / 4294967296.0) - 0.5f) * 2.0f;
}

// Fill `dst` with the shaped, level-calibrated stimulus. The LP state starts at 0 and the gain
// normalizes the SHAPED noise back to kRefRmsDb: for a one-pole LP with coefficient a on white
// noise of RMS r, the output RMS is r·sqrt(a / (2 − a)) — undo that analytically so the level
// is exact without a measurement pass.
inline void fill (float* dst, int numSamples, double sampleRate) noexcept
{
    const double a = 1.0 - std::exp (-6.283185307179586 * kShapeHz / (sampleRate > 0.0 ? sampleRate : 48000.0));
    const double whiteRms = 1.0 / std::sqrt (3.0);                        // uniform [-1,1]
    const double shapedRms = whiteRms * std::sqrt (a / (2.0 - a));        // one-pole LP on white
    const float g = (float) (std::pow (10.0, kRefRmsDb / 20.0) / shapedRms);
    float lp = 0.0f;
    for (int n = 0; n < numSamples; ++n)
    {
        lp += (float) a * (white (n) - lp);
        dst[n] = lp * g;
    }
}

} // namespace cab::levelprobe
