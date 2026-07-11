// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <felitronics/analysis/offline/SpectrumCurve.h>   // logMagnitudeCurve + interferenceDb (offline curves)
#include <felitronics/measurement/XcorrAlign.h>           // xcorrAlign — polarity of B vs the A reference
#include <felitronics/eq/Svf.h>                           // the same TPT SVF the live per-slot filters run

#include "Params.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

//==============================================================================
// cab::blend — OFFLINE, MESSAGE-THREAD-ONLY analysis of the A/B blend, run on the PREPARED
// taps (post HEAD/TRIM, resample, reference-unity normalization — exactly what the convolvers
// play; see Convolver::stagedTaps). Analysing the raw source files instead would lie: trim,
// resampling and the per-slot filters all move the interference picture. Two consumers:
//
//   suggestPolarityInvert — auto-polarity on IR load: is slot B phase-flipped against A?
//                           (felitronics::measurement::xcorrAlign; the slots have no
//                           time-shift, so only `invert` is consumed, confidence-gated.)
//   interferenceCurve     — the MIX strip's red/green tint: where the audible A/B blend
//                           cancels (comb) or reinforces, vs a phase-blind power sum.
//
// The audible model mirrors CabEngine::process exactly: per-slot Svf HPF→LPF (Butterworth Q)
// → polarity sign → dry/wet weight, then the LINEAR A/B crossfade with the mute-solo
// override. Filters are applied to the IR itself (the cab section is LTI: filtering the
// signal ≡ filtering the IR). 🔴 Never call from the audio thread — everything allocates.
//==============================================================================
namespace cab::blend
{

// The tint's log-f grid — 20 Hz–20 kHz over kPoints, log-uniform (the offline curves' default
// span), so a UI can map point index ↔ x position linearly.
inline constexpr double kFLo    = 20.0;
inline constexpr double kFHi    = 20000.0;
inline constexpr int    kPoints = 256;

// Auto-polarity acceptance: below this normalized-xcorr confidence the IRs are treated as
// unrelated and NOTHING is touched — a guessed flip on uncorrelated cabs creates the very
// comb it is meant to fix. xcorrAlign itself returns corr 0 when there is no valid suggestion
// (onsets further apart than the search range, degenerate windows).
inline constexpr double kMinPolarityCorr = 0.35;
// Polarity search range: HEAD trim already snaps each IR to its onset, so ±5 ms covers any
// residual pre-delay; onsets further apart than this are refused by xcorrAlign (corr = 0).
inline constexpr double kMaxLagSeconds = 0.005;

// The same Butterworth Q the live per-slot filters run (IRSlot's kFilterQ twin — keep in sync).
inline constexpr double kFilterQ = 0.707;

namespace detail
{
// Mean-fold planar taps to one mono lane (the display/analysis convention, like the reverb send).
template <class T>
inline std::vector<T> foldMono (const std::vector<std::vector<float>>& taps)
{
    std::size_t len = 0;
    for (const auto& ch : taps) len = std::max (len, ch.size());
    if (taps.empty() || len == 0) return {};
    std::vector<T> out (len, T (0));
    for (const auto& ch : taps)
        for (std::size_t i = 0; i < ch.size(); ++i) out[i] += (T) ch[i];
    const T g = T (1) / (T) taps.size();
    for (auto& v : out) v *= g;
    return out;
}
} // namespace detail

// The A/B crossfade the engine actually applies after the mute-solo override
// (CabEngine::process: muting a slot solos the other; B absent → full A).
inline float effectiveMixAB (float mixAB01, bool aMute, bool bMute, bool bLoaded) noexcept
{
    const bool aOn = ! aMute;
    const bool bOn = bLoaded && ! bMute;
    float ab = bLoaded ? mixAB01 : 0.0f;
    if      (! aOn) ab = 1.0f;
    else if (! bOn) ab = 0.0f;
    return ab;
}

// One slot's audible wet branch: mono fold → offline Svf HPF→LPF (the live filters' twin:
// same type/Q/order, so magnitude AND phase match) → polarity sign → weight.
inline std::vector<float> preparedBranch (const std::vector<std::vector<float>>& taps,
                                          const SlotParams& s, float weight, double sampleRate)
{
    auto ir = detail::foldMono<float> (taps);
    if (ir.empty())
        return {};

    if (s.hpfOn || s.lpfOn)
    {
        felitronics::eq::Svf f;
        f.prepare (sampleRate, 1);
        if (s.hpfOn)
        {
            f.setParams (felitronics::eq::FilterType::HighPass, s.hpfHz, kFilterQ, 0.0);
            for (auto& v : ir) v = f.processSample (0, v);
            f.reset();
        }
        if (s.lpfOn)
        {
            f.setParams (felitronics::eq::FilterType::LowPass, s.lpfHz, kFilterQ, 0.0);
            for (auto& v : ir) v = f.processSample (0, v);
        }
    }

    const float g = (s.phase ? -1.0f : 1.0f) * weight;
    for (auto& v : ir) v *= g;
    return ir;
}

// Does slot B sound phase-flipped against A? Normalized cross-correlation around the onsets
// (felitronics::measurement::xcorrAlign) on the prepared, host-rate taps of BOTH slots (one
// common rate — lags are in samples). nullopt = no confident suggestion: unrelated IRs, empty
// slots, onsets out of range — the caller must touch nothing.
inline std::optional<bool> suggestPolarityInvert (const std::vector<std::vector<float>>& tapsA,
                                                  const std::vector<std::vector<float>>& tapsB,
                                                  double sampleRate)
{
    if (! (sampleRate > 0.0))
        return std::nullopt;
    const auto a = detail::foldMono<double> (tapsA);
    const auto b = detail::foldMono<double> (tapsB);
    if (a.size() < 64 || b.size() < 64)               // xcorrAlign's own window floor
        return std::nullopt;

    const int maxLag = std::max (1, (int) std::lround (kMaxLagSeconds * sampleRate));
    const auto r = felitronics::measurement::xcorrAlign (a, b, maxLag);
    if (r.corr < kMinPolarityCorr)
        return std::nullopt;
    return r.invert;
}

// The MIX tint: interference (dB) of the audible A/B blend per log-f grid point — negative =
// phase cancellation eats the frequency, positive = coherent reinforcement (max +3 dB for a
// 50/50 pair), 0 = neutral. Empty when there is no two-sided blend to analyse (a slot empty,
// weighted out by MIX/mute, or degenerate input) — "no tint" rather than a flat curve.
// mixAB01 is the RAW param value; the mute-solo override is applied here (mirrors the engine).
inline std::vector<double> interferenceCurve (const std::vector<std::vector<float>>& tapsA,
                                              const std::vector<std::vector<float>>& tapsB,
                                              const SlotParams& a, const SlotParams& b,
                                              float mixAB01, bool bLoaded, double sampleRate)
{
    if (! (sampleRate > 0.0))
        return {};

    const float ab = effectiveMixAB (mixAB01, a.mute, b.mute, bLoaded);
    const float wA = (1.0f - ab) * a.dryWet01;        // the engine's wet weights: crossfade × dry/wet
    const float wB = ab * b.dryWet01;
    constexpr float kSilent = 1.0e-4f;                // below ≈ −80 dB a branch is out of the blend
    if (wA < kSilent || wB < kSilent)
        return {};

    const auto irA = preparedBranch (tapsA, a, wA, sampleRate);
    const auto irB = preparedBranch (tapsB, b, wB, sampleRate);
    if (irA.empty() || irB.empty())
        return {};

    std::vector<float> coherent (std::max (irA.size(), irB.size()), 0.0f);
    for (std::size_t i = 0; i < irA.size(); ++i) coherent[i] += irA[i];
    for (std::size_t i = 0; i < irB.size(); ++i) coherent[i] += irB[i];

    // normalize=false is the interferenceDb PRECONDITION: all three curves share one absolute
    // dB reference (a normalized curve would make the coherent-minus-incoherent subtraction
    // meaningless). Same grid for all three.
    felitronics::analysis::offline::LogCurveSpec spec;
    spec.fLo = kFLo; spec.fHi = kFHi; spec.points = kPoints; spec.normalize = false;

    const auto cSum = felitronics::analysis::offline::logMagnitudeCurve (std::span<const float> (coherent), sampleRate, spec);
    const auto cA   = felitronics::analysis::offline::logMagnitudeCurve (std::span<const float> (irA), sampleRate, spec);
    const auto cB   = felitronics::analysis::offline::logMagnitudeCurve (std::span<const float> (irB), sampleRate, spec);
    if (cSum.empty() || cA.empty() || cB.empty())
        return {};

    const felitronics::analysis::offline::MicCurveView mics[2] = { { cA, true }, { cB, true } };
    return felitronics::analysis::offline::interferenceDb (cSum, mics);
}

} // namespace cab::blend
