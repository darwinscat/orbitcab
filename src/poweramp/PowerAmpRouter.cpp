// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "PowerAmpRouter.h"
#include "../core/AmpStage.h"           // full def — render() calls nam.process()

namespace cab::poweramp
{

namespace
{
    constexpr double kRampSeconds = 0.03;   // matches CabEngine's other live glides

    inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }

    // block mean-square over all channels — the in/out energy probe for the tube loudness-normalizer.
    inline double meanSquare (float* const* io, int nCh, int n) noexcept
    {
        if (nCh <= 0 || n <= 0) return 0.0;
        double s = 0.0;
        for (int ch = 0; ch < nCh; ++ch)
        {
            const float* p = io[ch];
            for (int i = 0; i < n; ++i) s += (double) p[i] * (double) p[i];
        }
        return s / ((double) nCh * (double) n);
    }
}

void PowerAmpRouter::prepare (double sampleRate, int maxBlock, int numChannels)
{
    tube.prepare (sampleRate, maxBlock);
    scratch.setSize (juce::jmax (1, numChannels), juce::jmax (1, maxBlock), false, false, true);
    xfade.reset (sampleRate, kRampSeconds);
    xfade.setCurrentAndTargetValue (1.0f);
    current = fadeFrom = Active::off;
    fading  = false;
    seeded  = false;
    tubeLevel.prepare (sampleRate, kRampSeconds);
    tubeOutGain = 1.0f;
    tubeSeed = true;
    tubeRenderedLast = false;
}

void PowerAmpRouter::reset()
{
    tube.reset();
    scratch.clear();
    xfade.setCurrentAndTargetValue (1.0f);
    current = fadeFrom = Active::off;
    fading  = false;
    seeded  = false;
    tubeLevel.reset();
    tubeSeed = true;
    tubeRenderedLast = false;
}

void PowerAmpRouter::render (Active a, float* const* io, int numChannels, int numSamples,
                             AmpStage& nam) noexcept
{
    switch (a)
    {
        case Active::capture: nam.process  (io, numChannels, numSamples, /*normalize*/ true); break;
        case Active::tube:
        {
            // Loudness-normalize the tube to its INPUT level (setting-invariant): output loudness ≈
            // input loudness at ANY drive, so enabling it never jumps the post-poweramp level (→ no
            // AutoLeveler kick; honest A/B vs the loudness-normalized capture). Seed the leveler on
            // (re)activation so it SNAPS to the converged gain instead of chasing over ~150 ms. The
            // Tube Output knob (stripped from the core in process()) re-applies here as tubeOutGain.
            const double inMS = meanSquare (io, numChannels, numSamples);
            tube.process (io, numChannels, numSamples);
            const double outMS = meanSquare (io, numChannels, numSamples);
            if (tubeSeed) { tubeLevel.seed ((float) std::sqrt ((inMS + 1.0e-12) / (outMS + 1.0e-12))); tubeSeed = false; }
            else            tubeLevel.processBlock (inMS, outMS, /*enabled*/ true, numSamples);
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = tubeLevel.getNextGain() * tubeOutGain;
                for (int ch = 0; ch < numChannels; ++ch) io[ch][i] *= g;
            }
            break;
        }
        case Active::off:     break;   // no-op: the dry signal passes through unchanged
    }
}

void PowerAmpRouter::process (float* const* io, int numChannels, int numSamples,
                              bool ampOn, PowerAmpMode mode, const TubeParams& tubeParams,
                              AmpStage& nam) noexcept
{
    // Strip the Tube Output knob out of the core and re-apply it AFTER the loudness match in render()
    // (else the normalizer would cancel it). The core then renders drive/character at its natural level.
    TubeParams tp = tubeParams;
    tubeOutGain = dbToGain (std::isfinite (tp.outputDb) ? tp.outputDb : 0.0f);
    tp.outputDb = 0.0f;
    tube.setParams (tp);   // cheap: stores targets; the tube smooths its coeffs internally
    const Active target = resolve (ampOn, mode);

    if (! seeded)
    {
        // First block after prepare/reset: jump straight to the target with no ramp, so a
        // session booting with ampOn matches the legacy seam (NAM from block 1, no fade-in).
        current = target;
        seeded  = true;
    }
    else if (target == Active::off)
    {
        // The master ampOn gate going OFF is ALWAYS an instantaneous hard switch — even mid-fade —
        // exactly like the legacy `if (p.ampOn)` seam. Cancel any in-flight crossfade.
        current = target;
        fading  = false;
    }
    else if (! fading && target != current)
    {
        if (current == Active::off)
        {
            // Gate ON (off → capture/tube) is a hard switch, like the legacy seam (no fade-in).
            current = target;
        }
        else
        {
            // capture <-> tube, both stages live → click-free 30 ms constant-sum crossfade.
            fadeFrom = current;
            current  = target;
            xfade.setCurrentAndTargetValue (0.0f);
            xfade.setTargetValue (1.0f);
            fading = true;
        }
    }

    // (Re)activation of the tube → seed the loudness-normalizer on the next tube render (snap, no chase).
    const bool tubeNow = (current == Active::tube) || (fading && fadeFrom == Active::tube);
    if (tubeNow && ! tubeRenderedLast) tubeSeed = true;
    tubeRenderedLast = tubeNow;

    if (! fading)
    {
        render (current, io, numChannels, numSamples, nam);
        return;
    }

    // Crossfade: render BOTH endpoints from the SAME block input. Preserve the original input
    // in `scratch`, render the fade-TO mode in place on `io`, render the fade-FROM mode in place
    // on the preserved copy, then blend io = from*(1-t) + to*t with the smoothed ramp.
    for (int ch = 0; ch < numChannels; ++ch)
        scratch.copyFrom (ch, 0, io[ch], numSamples);          // scratch = original input

    render (current,  io,                              numChannels, numSamples, nam);  // io = fade-TO
    render (fadeFrom, scratch.getArrayOfWritePointers(), numChannels, numSamples, nam); // scratch = fade-FROM

    for (int n = 0; n < numSamples; ++n)
    {
        const float t = xfade.getNextValue();
        for (int ch = 0; ch < numChannels; ++ch)
            io[ch][n] = scratch.getSample (ch, n) * (1.0f - t) + io[ch][n] * t;
    }

    if (! xfade.isSmoothing())
        fading = false;
}

} // namespace cab::poweramp
