// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>   // jlimit, Decibels
#include <algorithm>
#include <cmath>

//==============================================================================
// cab::AutoLeveler — the wet->dry loudness match. Two slow one-pole RMS
// followers + a silence gate produce a makeup gain of sqrt(dryMS / mixMS), clamped
// and rate-limited so the blend stays balanced without pumping or zippering.
//
// Gain dynamics (two stages, both block-size- and sample-rate-invariant):
//   • the block-rate TARGET is dB-slew-limited against the PREVIOUS TARGET at
//     kMakeupSlewDbPerSec — the hard cap on how fast the loudness match may move.
//     Normal playing never hits it (the 150 ms followers already move slowly); it makes
//     an audible loudness PUMP structurally impossible from any fast target jump.
//   • the APPLIED gain glides toward the target at that same rate PER SAMPLE (a
//     dB-linear ramp implemented as a geometric progression — one multiply per sample,
//     no per-sample pow). No juce::SmoothedValue: re-arming a fixed-length ramp against
//     a slew-clamped target every block compounds into a block-size-dependent crawl
//     (measured 0.4 dB/s @ 64-sample blocks vs the intended 9 dB/s — the frozen-leveler
//     bug this design replaces).
//
// DETERMINISTIC retargets — a poweramp route snap (the engine knows the correct makeup
// for the route it is switching to), the enable/disable toggle, seed() at IR load — are
// not follower chases: they may jump the TARGET directly (bypassing the target slew) and
// open a bounded FAST window (kSnapSlewDbPerSec) so the applied gain lands in ~a fade
// length instead of dragging at 9 dB/s. The fast rate is still a hard rate limit — a
// deliberate glide synced with a mode change, not a pump.
//
// Pure-numeric: it takes per-block mean-square energies and returns a per-sample
// gain — no buffers, no JUCE GUI — so it's trivially unit-testable and portable.
//==============================================================================
namespace cab
{

class AutoLeveler
{
public:
    void prepare (double sampleRate)
    {
        currentSampleRate = sampleRate > 1000.0 ? sampleRate : 48000.0;
        const double perSample = kMakeupSlewDbPerSec / currentSampleRate;
        const double perSampleFast = kSnapSlewDbPerSec / currentSampleRate;
        ratioUpNormal = (float) std::pow (10.0, perSample / 20.0);
        ratioUpFast   = (float) std::pow (10.0, perSampleFast / 20.0);
        ratioDnNormal = 1.0f / ratioUpNormal;
        ratioDnFast   = 1.0f / ratioUpFast;
        reset();
    }

    void reset()
    {
        dryMeanSq = 0.0;
        mixMeanSq = 0.0;
        targetDb  = 0.0f;
        targetGain = 1.0f;
        appliedGain = 1.0f;
        fastSamplesLeft = 0;
        prevEnabled = true;
    }

    // Seed the makeup + followers to a known gain (estimated from the loaded IR's energy)
    // so the first audio after prepare starts at ~the converged value — no startup boost
    // while the followers crawl up from zero (#48). The followers are set to a consistent
    // (dry, mix) pair so the slow follower doesn't yank the makeup during convolver warm-up.
    void seed (float makeupGain)
    {
        const float g = juce::jlimit (kMatchMinGain, kMatchMaxGain, makeupGain);
        targetDb  = juce::Decibels::gainToDecibels (g, kSilentDb);
        targetGain = g;
        appliedGain = g;
        fastSamplesLeft = 0;
        // Seed the followers at a REALISTIC program energy (−18 dBFS mean-square), not 1.0
        // (= 0 dBFS): the seed state decays at the follower τ, and a 0 dBFS seed out-masses a
        // real signal (~−15..−20 dBFS) for ~5τ ≈ 0.75 s, pinning the ratio to the seed long
        // after audio arrives. At −18 dBFS the followers hand over to the live signal promptly.
        dryMeanSq = kSeedMeanSq;
        mixMeanSq = kSeedMeanSq / ((double) g * g);     // sqrt(dryMeanSq / mixMeanSq) == g
    }

    // DETERMINISTIC route retarget (the poweramp seam capture<->tube/off switch): jump the
    // target to the KNOWN converged makeup of the route being switched to and glide there at
    // the fast (still hard-limited) rate, in sync with the router's crossfade. The mix follower
    // is re-seeded at the CURRENT dry level so the followers agree with the snapped ratio
    // instead of pulling back toward the old route's spectrum (no absolute-level transient:
    // the dry follower keeps tracking the real signal).
    void snapRatioTo (float makeupGain)
    {
        const float g = juce::jlimit (kMatchMinGain, kMatchMaxGain, makeupGain);
        targetDb = juce::Decibels::gainToDecibels (g, kSilentDb);
        targetGain = g;
        // Re-seed the mix follower so the followers AGREE with the snapped ratio — otherwise the
        // very next block's raw target would pull straight back toward the OLD route's spectrum.
        // In silence the dry state is floored so the pair stays consistent (tiny but ratio-true);
        // when signal returns both followers climb together and the ratio holds.
        mixMeanSq = std::max (dryMeanSq, kMatchFloorMeanSq) / ((double) g * g);
        fastSamplesLeft = (int) std::lround (kSnapWindowSeconds * currentSampleRate);
    }

    // Advance the followers with this block's dry / mixed mean-square energy and set the
    // rate-limited makeup target. `enabled` false => aim for unity (the followers keep
    // running, so re-enabling snaps back). Below the silence floor the target is held
    // (won't chase the noise floor).
    void processBlock (double dryBlockMeanSq, double mixBlockMeanSq, bool enabled, int numSamples)
    {
        const double a = 1.0 - std::exp (-(double) numSamples / (kMatchTimeConstant * currentSampleRate));
        dryMeanSq += a * (dryBlockMeanSq - dryMeanSq);
        mixMeanSq += a * (mixBlockMeanSq - mixMeanSq);

        float rawTarget;
        if (! enabled)
            rawTarget = 1.0f;
        else if (dryMeanSq > kMatchFloorMeanSq)
            rawTarget = juce::jlimit (kMatchMinGain, kMatchMaxGain,
                                      (float) std::sqrt (dryMeanSq / (mixMeanSq + 1.0e-12)));
        else
            return;   // silence gate: hold the last target — and leave prevEnabled untouched, so a
                      // re-enable that lands inside silence is still treated as a flip (fast glide)
                      // on the first non-silent block instead of being silently consumed here.

        const bool enabledFlipped = (enabled != prevEnabled);
        prevEnabled = enabled;

        const float rawDb = juce::Decibels::gainToDecibels (rawTarget, kSilentDb);
        if (enabledFlipped)
        {
            // The on/off toggle is a deterministic retarget, not a follower chase: jump the
            // target (unity when disabling, the live ratio when re-enabling) and glide fast.
            targetDb = rawDb;
            targetGain = rawTarget;
            fastSamplesLeft = (int) std::lround (kSnapWindowSeconds * currentSampleRate);
            return;
        }

        // dB-domain slew limit against the PREVIOUS TARGET (not the applied gain — clamping
        // around the applied value compounds with the applied glide into a block-size-dependent
        // crawl). Exactly kMakeupSlewDbPerSec regardless of block size.
        const float stepDb = (float) (kMakeupSlewDbPerSec * (double) numSamples / currentSampleRate);
        targetDb = juce::jlimit (targetDb - stepDb, targetDb + stepDb, rawDb);
        targetGain = juce::Decibels::decibelsToGain (targetDb, kSilentDb);   // one pow per block
    }

    // Per-sample: glide the applied gain toward the target at the (normal or fast) hard rate.
    // dB-linear ramp as a geometric progression — one multiply per sample. The LINEAR gain is
    // the single source of truth (a separate dB accumulator would drift against the multiplied
    // gain and pop on landing); min/max clamp the last step exactly onto the target.
    float getNextGain()
    {
        const bool fast = fastSamplesLeft > 0;
        if (fast) --fastSamplesLeft;
        if (appliedGain < targetGain)
            appliedGain = std::min (appliedGain * (fast ? ratioUpFast : ratioUpNormal), targetGain);
        else if (appliedGain > targetGain)
            appliedGain = std::max (appliedGain * (fast ? ratioDnFast : ratioDnNormal), targetGain);
        return appliedGain;
    }

    float currentGain()   const { return appliedGain; }
    float currentGainDb() const { return juce::Decibels::gainToDecibels (appliedGain, kSilentDb); }

    // True when the applied gain has landed on the target (no glide in progress). The engine
    // gates its route-makeup cache writes on this so a mid-glide value (a snap, an enable flip,
    // a slew-limited convergence) can never be remembered as "converged" (review finding).
    bool settled() const
    {
        return std::fabs (juce::Decibels::gainToDecibels (appliedGain, kSilentDb) - targetDb) < 0.05f;
    }

private:
    static constexpr double kMatchTimeConstant  = 0.15;   // s — slow enough not to pump
    static constexpr double kSeedMeanSq         = 0.0158; // −18 dBFS² — realistic program energy for seeds
    static constexpr double kMakeupSlewDbPerSec = 9.0;    // dB/s — hard cap on makeup movement (no pump, ever)
    static constexpr double kSnapSlewDbPerSec   = 40.0;   // dB/s — deterministic-retarget glide (route snap / on-off)
    static constexpr double kSnapWindowSeconds  = 0.35;   // fast-rate budget per snap (covers ±12 dB and lands)
    static constexpr double kMatchFloorMeanSq  = 1.0e-6;  // ~ -60 dBFS RMS: below this, hold
    static constexpr float  kMatchMinGain      = 0.0631f; // -24 dB
    static constexpr float  kMatchMaxGain      = 63.10f;  // +36 dB (headroom for lossy IRs)
    static constexpr float  kSilentDb          = -120.0f;

    double currentSampleRate = 44100.0;
    double dryMeanSq = 0.0;
    double mixMeanSq = 0.0;
    float  targetDb  = 0.0f;               // block-rate target, slew-limited against itself
    float  targetGain = 1.0f;              // linear mirror of targetDb (updated when targetDb moves)
    float  appliedGain = 1.0f;             // per-sample rate-limited glide toward targetGain — THE truth
    float  ratioUpNormal = 1.0f, ratioDnNormal = 1.0f, ratioUpFast = 1.0f, ratioDnFast = 1.0f;
    int    fastSamplesLeft = 0;
    bool   prevEnabled = true;
};

} // namespace cab
