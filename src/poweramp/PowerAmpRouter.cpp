// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "PowerAmpRouter.h"
#include "../core/AmpStage.h"           // full def — render() calls nam.process()
#include "TubeKernel.h"                 // kTubeVoicings — per-voicing level-match threshold
#include <cmath>

namespace cab::poweramp
{

namespace
{
    constexpr double kRampSeconds = 0.03;   // matches CabEngine's other live glides
}

void PowerAmpRouter::prepare (double sampleRate, int maxBlock, int numChannels)
{
    tube.prepare (sampleRate, maxBlock);
    alignLatency = juce::jmax (0, tube.latencySamples());   // the amount the dry/off path is delayed by in tube mode
    const int ch = juce::jmax (1, numChannels);
    scratch.setSize    (ch, juce::jmax (1, maxBlock), false, false, true);
    dryScratch.setSize (ch, juce::jmax (1, maxBlock), false, false, true);
    dryRing.setSize    (ch, juce::jmax (1, alignLatency), false, false, true);
    dryRing.clear();
    dryRingPos = 0;
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
    dryScratch.clear();
    dryRing.clear();
    dryRingPos = 0;
    xfade.setCurrentAndTargetValue (1.0f);
    current = fadeFrom = Active::off;
    fading  = false;
    seeded  = false;
}

void PowerAmpRouter::advanceDryAlign (const float* const* io, int numChannels, int numSamples) noexcept
{
    // dryScratch = io delayed by alignLatency; the ring advances once per block (fed EVERY block so
    // it is warm the instant the OFF path needs it). alignLatency == 0 → the tube reports no latency,
    // so alignment is a plain copy (dry passes untouched).
    const int L = alignLatency;
    if (L <= 0)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            dryScratch.copyFrom (ch, 0, io[ch], numSamples);
        return;
    }
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* ring = dryRing.getWritePointer (ch);
        float* dst  = dryScratch.getWritePointer (ch);
        int pos = dryRingPos;
        for (int i = 0; i < numSamples; ++i)
        {
            dst[i]    = ring[pos];      // sample from exactly L samples ago
            ring[pos] = io[ch][i];      // overwrite with the freshest input
            if (++pos >= L) pos = 0;
        }
    }
    dryRingPos = (dryRingPos + numSamples) % L;   // all channels advanced identically → one commit
}

void PowerAmpRouter::render (Active a, float* const* dst, int numChannels, int numSamples,
                             AmpStage& nam, bool tubeMode) noexcept
{
    switch (a)
    {
        case Active::capture: nam.process  (dst, numChannels, numSamples, /*normalize*/ true); break;
        case Active::tube:
            tube.process (dst, numChannels, numSamples);
            if (tubeMakeup != 1.0f)   // static per-voicing level trim (params-derived; no follower → no kick)
                for (int i = 0; i < numSamples; ++i)
                    for (int ch = 0; ch < numChannels; ++ch) dst[ch][i] *= tubeMakeup;
            break;
        case Active::off:
            // In tube mode the dry must match the tube's PDC → emit the latency-aligned dry; in
            // capture mode (0-latency) the raw dry already sitting in `dst` passes through untouched.
            if (tubeMode)
                for (int ch = 0; ch < numChannels; ++ch)
                    juce::FloatVectorOperations::copy (dst[ch], dryScratch.getReadPointer (ch), numSamples);
            break;
    }
}

void PowerAmpRouter::process (float* const* io, int numChannels, int numSamples,
                              bool ampOn, PowerAmpMode mode, const TubeParams& tubeParams,
                              AmpStage& nam) noexcept
{
    tube.setParams (tubeParams);   // cheap: stores targets; the tube smooths its coeffs internally
    {   // Deterministic level-match (no follower → no chase/swell/kick), calibrated on a real guitar DI:
        //   • per-voicing trim centres each voicing on ~dry at the default Drive (18 dB) + equalises the four;
        //   • a gentle drive slope (~0.52 dB/dB about 18 dB) flattens the level across Drive, so the stage
        //     stays ~dry-matched at any Drive (→ no enable/disable kick anywhere). Its by-product — quiet
        //     parts rising as Drive climbs while peaks saturate — is exactly power-amp compression/bloom.
        const TubeVoicing& v = kTubeVoicings[(std::size_t) juce::jlimit (0, 3, tubeParams.tubeType)];
        const float d = std::isfinite (tubeParams.driveDb) ? tubeParams.driveDb : 18.0f;
        // The duck is ~flat up to ~12 dB Drive, then falls ~0.86 dB/dB — a knee at 12, not a line from 18.
        // levelTrimDb is the measured make-up at the 18 dB default; the knee term re-references it to 18.
        const float slopeDb = 0.86f * (std::max (0.0f, d - 12.0f) - 6.0f);
        const float seDb    = tubeParams.singleEnded ? v.levelTrimSEDb : 0.0f;   // SE asymmetry shifts level per voicing
        const float trimDb  = juce::jlimit (-24.0f, 24.0f, v.levelTrimDb + slopeDb + seDb);
        tubeMakeup = std::pow (10.0f, 0.05f * trimDb);
    }
    const Active target   = resolve (ampOn, mode);
    const bool   tubeMode = (mode == PowerAmpMode::tube);

    // Keep the dry-alignment delay warm every block and stage the latency-aligned dry (used by the
    // OFF path in tube mode). Runs BEFORE any render — `io` still holds the raw block input here.
    advanceDryAlign (io, numChannels, numSamples);

    if (! seeded)
    {
        // First block after prepare/reset: jump straight to the target with no ramp, so a
        // session booting with ampOn matches the legacy seam (NAM from block 1, no fade-in).
        current = target;
        seeded  = true;
    }
    else if (! fading && target != current)
    {
        // ANY live route change (off↔capture↔tube) → a 30 ms constant-sum crossfade, including the
        // master on/off gate. "off" renders as the dry signal, so off↔tube is a dry↔tube blend. The
        // gradual (not hard-step) change lets the cab auto-leveler TRACK the new level instead of
        // overshooting on a step — that overshoot was the enable/disable volume "kick".
        fadeFrom = current;
        current  = target;
        xfade.setCurrentAndTargetValue (0.0f);
        xfade.setTargetValue (1.0f);
        fading = true;
    }

    if (! fading)
    {
        render (current, io, numChannels, numSamples, nam, tubeMode);
        return;
    }

    // Crossfade: render BOTH endpoints from the SAME block input. Preserve the original input
    // in `scratch`, render the fade-TO mode in place on `io`, render the fade-FROM mode in place
    // on the preserved copy, then blend io = from*(1-t) + to*t with the smoothed ramp. In tube
    // mode both endpoints resolve to the tube's PDC (tube = inherent, off = dryScratch), so the
    // blend is time-aligned — no comb / level jump.
    for (int ch = 0; ch < numChannels; ++ch)
        scratch.copyFrom (ch, 0, io[ch], numSamples);          // scratch = original input

    render (current,  io,                                numChannels, numSamples, nam, tubeMode); // io = fade-TO
    render (fadeFrom, scratch.getArrayOfWritePointers(), numChannels, numSamples, nam, tubeMode); // scratch = fade-FROM

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
