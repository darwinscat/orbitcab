// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "PowerAmpRouter.h"
#include "../core/AmpStage.h"           // full def — render() calls nam.process()

namespace cab::poweramp
{

namespace
{
    constexpr double kRampSeconds = 0.03;   // matches CabEngine's other live glides
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
}

void PowerAmpRouter::reset()
{
    tube.reset();
    scratch.clear();
    xfade.setCurrentAndTargetValue (1.0f);
    current = fadeFrom = Active::off;
    fading  = false;
    seeded  = false;
}

void PowerAmpRouter::render (Active a, float* const* io, int numChannels, int numSamples,
                             AmpStage& nam) noexcept
{
    switch (a)
    {
        case Active::capture: nam.process  (io, numChannels, numSamples, /*normalize*/ true); break;
        case Active::tube:    tube.process (io, numChannels, numSamples); break;
        case Active::off:     break;   // no-op: the dry signal passes through unchanged
    }
}

void PowerAmpRouter::process (float* const* io, int numChannels, int numSamples,
                              bool ampOn, PowerAmpMode mode, AmpStage& nam) noexcept
{
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
