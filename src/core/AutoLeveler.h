// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>   // jlimit (SmoothedValue is now felitronics::core::LinearSmoother)
#include <felitronics/core/Smoother.h>
#include <cmath>

//==============================================================================
// cab::AutoLeveler — the wet->dry loudness match. Two slow one-pole RMS
// followers + a silence gate produce a makeup gain of sqrt(dryMS / mixMS), clamped
// and smoothed so the blend stays balanced without pumping or zippering.
//
// Pure-numeric: it takes per-block mean-square energies and returns a per-sample
// gain — no buffers, no JUCE GUI — so it's trivially unit-testable and portable.
//==============================================================================
namespace cab
{

class AutoLeveler
{
public:
    void prepare (double sampleRate, double rampSeconds)
    {
        currentSampleRate = sampleRate;
        dryMeanSq = 0.0;
        mixMeanSq = 0.0;
        matchSmoothed.reset (sampleRate, rampSeconds);
        matchSmoothed.setCurrentAndTargetValue (1.0f);
    }

    void reset()
    {
        dryMeanSq = 0.0;
        mixMeanSq = 0.0;
        matchSmoothed.setCurrentAndTargetValue (1.0f);
    }

    // Seed the makeup + followers to a known gain (estimated from the loaded IR's energy)
    // so the first audio after prepare starts at ~the converged value — no startup boost
    // while the followers crawl up from zero (#48). The followers are set to a consistent
    // (dry, mix) pair so the slow follower doesn't yank the makeup during convolver warm-up.
    void seed (float makeupGain)
    {
        const float g = juce::jlimit (kMatchMinGain, kMatchMaxGain, makeupGain);
        matchSmoothed.setCurrentAndTargetValue (g);
        dryMeanSq = 1.0;
        mixMeanSq = 1.0 / ((double) g * g);     // sqrt(dryMeanSq / mixMeanSq) == g
    }

    // Advance the followers with this block's dry / mixed mean-square energy and set the
    // smoothed makeup target. `enabled` false => aim for unity (the followers keep
    // running, so re-enabling snaps back instantly). Below the silence floor the target
    // is held (won't chase the noise floor).
    void processBlock (double dryBlockMeanSq, double mixBlockMeanSq, bool enabled, int numSamples)
    {
        const double a = 1.0 - std::exp (-(double) numSamples / (kMatchTimeConstant * currentSampleRate));
        dryMeanSq += a * (dryBlockMeanSq - dryMeanSq);
        mixMeanSq += a * (mixBlockMeanSq - mixMeanSq);

        if (! enabled)
            matchSmoothed.setTargetValue (1.0f);
        else if (dryMeanSq > kMatchFloorMeanSq)
            matchSmoothed.setTargetValue (juce::jlimit (kMatchMinGain, kMatchMaxGain,
                                                        (float) std::sqrt (dryMeanSq / (mixMeanSq + 1.0e-12))));
    }

    float getNextGain()      { return matchSmoothed.getNextValue(); }
    float currentGain() const { return matchSmoothed.getCurrentValue(); }

private:
    static constexpr double kMatchTimeConstant = 0.15;    // s — slow enough not to pump
    static constexpr double kMatchFloorMeanSq  = 1.0e-6;  // ~ -60 dBFS RMS: below this, hold
    static constexpr float  kMatchMinGain      = 0.0631f; // -24 dB
    static constexpr float  kMatchMaxGain      = 63.10f;  // +36 dB (headroom for lossy IRs)

    double currentSampleRate = 44100.0;
    double dryMeanSq = 0.0;
    double mixMeanSq = 0.0;
    felitronics::core::LinearSmoother matchSmoothed { 1.0f };
};

} // namespace cab
