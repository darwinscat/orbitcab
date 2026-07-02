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
    // Dry-alignment ring capacity: covers ANY stage latency (tube oversampling ~31; NAM rate-match
    // ≈ ceil(3·hostSR/modelSR)+3, ≤ ~27 even at 384 kHz) with wide margin. Fixed so the delay tap
    // can vary per block (tube ↔ capture) without ever reallocating on the audio thread.
    constexpr int kAlignRingSamples = 256;

}

void PowerAmpRouter::prepare (double sampleRate, int maxBlock, int numChannels)
{
    // One tube per OS-quality option, ALL prepared here → the live OS switch is just picking `osSel` (a
    // per-block branch), never a reallocation on the audio thread. Latency is tpp-based (31), invariant
    // across OS factor, so switching costs 0 PDC. Higher OS = softer top (less folded aliasing).
    for (int i = 0; i < kNumOs; ++i)
        tube[i].prepare (sampleRate, maxBlock, kOsFactor[i]);
    const int ch = juce::jmax (1, numChannels);
    scratch.setSize (ch, juce::jmax (1, maxBlock), false, false, true);
    dryAligner.prepare (ch, maxBlock, kAlignRingSamples);
    xfade.reset (sampleRate, kRampSeconds);
    xfade.setCurrentAndTargetValue (1.0f);
    current = fadeFrom = Active::off;
    fading  = false;
    seeded  = false;
}

void PowerAmpRouter::reset()
{
    for (int i = 0; i < kNumOs; ++i) tube[i].reset();
    scratch.clear();
    dryAligner.reset();
    xfade.setCurrentAndTargetValue (1.0f);
    current = fadeFrom = Active::off;
    fading  = false;
    seeded  = false;
}

void PowerAmpRouter::render (Active a, float* const* dst, int numChannels, int numSamples,
                             AmpStage& nam) noexcept
{
    switch (a)
    {
        case Active::capture: nam.process  (dst, numChannels, numSamples, /*normalize*/ true); break;
        case Active::tube:
            tube[osSel].process (dst, numChannels, numSamples);
            if (tubeMakeup != 1.0f)   // static per-voicing level trim (params-derived; no follower → no kick)
                for (int i = 0; i < numSamples; ++i)
                    for (int ch = 0; ch < numChannels; ++ch) dst[ch][i] *= tubeMakeup;
            break;
        case Active::off:
            // Emit the latency-aligned dry so an off↔active crossfade stays time-aligned to the
            // active stage's PDC. When that stage reports 0 latency, the aligned dry == the raw dry,
            // so this is a straight pass-through (identical to the legacy dry path).
            for (int ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::copy (dst[ch], dryAligner.delayed (ch), numSamples);
            break;
    }
}

void PowerAmpRouter::process (float* const* io, int numChannels, int numSamples,
                              bool ampOn, PowerAmpMode mode, const TubeParams& tubeParams,
                              AmpStage& nam) noexcept
{
    osSel = juce::jlimit (0, kNumOs - 1, tubeParams.osIndex);   // live OS-quality pick (all tubes pre-prepared)
    for (int i = 0; i < kNumOs; ++i) tube[i].setParams (tubeParams);   // cheap (stores targets); keeps the idle
                                                                       // tube's params current so a live switch has no param jump, only a brief state settle
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
        // No automatic capture level-match: the tube's loudness is Oleh's MANUAL per-voicing
        // calibration only (an earlier auto-match to the loaded capture's probed reference gain
        // made the tube's level depend on whether a capture happened to be ARMED — it jumped
        // +5.6 dB mid-session the first time the user visited the NAM tab; removed by decision).
        const float trimDb  = juce::jlimit (-24.0f, 24.0f, v.levelTrimDb + slopeDb + seDb);
        tubeMakeup = std::pow (10.0f, 0.05f * trimDb);
    }
    const Active target   = resolve (ampOn, mode);
    const bool   tubeMode = (mode == PowerAmpMode::tube);
    fadeStartedThisBlock  = false;

    // Keep the dry-alignment delay warm every block and stage the latency-aligned dry (used by the
    // OFF path). Runs BEFORE any render — `io` still holds the raw block input here. The delay tracks
    // the ACTIVE mode's PDC: the tube's fixed oversampling latency, or the NAM capture's rate-match
    // (0 at 48 kHz). This must match what PluginProcessor::updateLatency reports for the same mode.
    const int alignLatency = tubeMode ? tube[0].latencySamples() : nam.latencySamples();   // tube latency invariant across OS
    dryAligner.advance (io, numChannels, numSamples, alignLatency);

    if (! seeded)
    {
        // First block after prepare/reset: jump straight to the target with no ramp, so a
        // session booting with ampOn matches the legacy seam (NAM from block 1, no fade-in).
        current = target;
        seeded  = true;
    }
    else if (! fading && target != current)
    {
        if (target != Active::off && current != Active::off)
        {
            // capture<->tube MODE JUMP: deliberately HARD — no crossfade. The honest PDC
            // re-report (0 <-> 31) makes the host re-sync/mute around this switch anyway, so a
            // 30 ms blend of two time-misaligned stages bought nothing and cost a smeared,
            // audible "transition". The switch is a clean cut; the leveler retargets instantly
            // off the same event (fadeStartedThisBlock).
            current = target;
            fadeStartedThisBlock = true;
        }
        else
        {
            // The master on/off gate keeps its 30 ms constant-sum crossfade ("off" renders as the
            // latency-aligned dry, so off↔stage is a time-aligned blend — the beloved click-free
            // power toggle). The gradual change also lets the auto-leveler TRACK the new level.
            fadeFrom = current;
            current  = target;
            xfade.setCurrentAndTargetValue (0.0f);
            xfade.setTargetValue (1.0f);
            fading = true;
            fadeStartedThisBlock = true;   // the engine keys its leveler route-snap off THIS (accepted
                                           // transitions only), never off the raw params (see header)
        }
    }

    if (! fading)
    {
        render (current, io, numChannels, numSamples, nam);
        return;
    }

    // Crossfade: render BOTH endpoints from the SAME block input. Preserve the original input
    // in `scratch`, render the fade-TO mode in place on `io`, render the fade-FROM mode in place
    // on the preserved copy, then blend io = from*(1-t) + to*t with the smoothed ramp. In tube
    // mode both endpoints resolve to the tube's PDC (tube = inherent, off = dryScratch), so the
    // blend is time-aligned — no comb / level jump.
    for (int ch = 0; ch < numChannels; ++ch)
        scratch.copyFrom (ch, 0, io[ch], numSamples);          // scratch = original input

    render (current,  io,                                numChannels, numSamples, nam); // io = fade-TO
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
