// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_dsp/juce_dsp.h>          // AudioBuffer scratch + SmoothedValue fade
#include "TubePowerAmp.h"
#include "../core/Params.h"             // cab::PowerAmpMode
#include "../core/AutoLeveler.h"        // tube loudness-normalization (setting-invariant, no enable kick)

namespace cab { class AmpStage; }       // forward-decl — the NAM capture, owned by CabEngine

//==============================================================================
// cab::poweramp::PowerAmpRouter — the SINGLE poweramp seam. Picks the active power-amp at the
// CabEngine poweramp position and crossfades click-free between them on a live switch, so
// CabEngine::process needs exactly ONE call here and the existing NAM path (cab::AmpStage `amp`,
// passed by reference and still owned + lifecycle-managed by CabEngine) is untouched:
//
//   ampOn == false              → OFF   (dry passes, exactly as before)
//   ampOn && mode == capture    → NAM capture: amp.process(..., normalize=true)   [default]
//   ampOn && mode == tube       → white-box cab::poweramp::TubePowerAmp
//
// A 30 ms constant-sum (linear) crossfade covers a live capture<->tube switch: both endpoints
// are rendered from the SAME block input (one prepared scratch buffer) and blended. The first
// block after prepare/reset SEEDS the active mode with no ramp, so a session that boots with
// ampOn behaves exactly like the legacy seam (NAM runs from block 1, no fade-in). The OFF<->on
// master gate keeps the engine's existing behaviour; only the capture<->tube move is smoothed.
//
// Note: only ONE of the two stages runs per block (the active one, or both briefly during a
// fade — but never the same stage twice), so the NAM stage's internal state is never advanced
// twice in a block. A stage that goes idle (faded out) is no longer fed; on switching back it
// resumes from stale state — acceptable for a 30 ms fade, revisited if it ever audibly matters.
//
// Latency note: the two stages can report different host-rate latencies (a rate-matched NAM
// capture vs the tube). During the 30 ms blend they are NOT latency-aligned, so the crossfade is
// mildly phase-smeared and the host PDC (reported on the mode switch) leads the audio swap by up
// to the fade length. Negligible while the tube stage is zero-latency; revisit (delay-align the
// endpoints during the fade) when oversampling gives the tube real latency — see PLAN.md.
//
// 🔴 RT: process() never allocates/locks/throws — the scratch is allocated in prepare().
//==============================================================================
namespace cab::poweramp
{

class PowerAmpRouter
{
public:
    void prepare (double sampleRate, int maxBlock, int numChannels);
    void reset();

    // The one poweramp-seam call. In place on planar `io`. `nam` is CabEngine's NAM poweramp
    // (used in capture mode); the tube stage lives inside the router.
    void process (float* const* io, int numChannels, int numSamples,
                  bool ampOn, PowerAmpMode mode, const TubeParams& tubeParams, AmpStage& nam) noexcept;

    int  tubeLatencySamples() const noexcept { return tube.latencySamples(); }

private:
    enum class Active { off, capture, tube };
    static Active resolve (bool ampOn, PowerAmpMode mode) noexcept
    {
        return ! ampOn ? Active::off
                       : (mode == PowerAmpMode::tube ? Active::tube : Active::capture);
    }

    // Render one stage in place on `io` (off = no-op → dry passes through).
    void render (Active a, float* const* io, int numChannels, int numSamples, AmpStage& nam) noexcept;

    TubePowerAmp tube;
    juce::AudioBuffer<float>   scratch;             // preallocated: holds the fade-FROM render
    juce::SmoothedValue<float> xfade { 1.0f };      // 0→1 ramp during a capture<->tube switch
    Active current  = Active::off;
    Active fadeFrom = Active::off;
    bool   fading   = false;
    bool   seeded   = false;                        // first block after prepare jumps to target (no ramp)

    // Tube loudness-normalization: hold the tube's OUTPUT loudness = its INPUT loudness (setting-
    // invariant) so enabling it — at ANY drive — doesn't jump the post-poweramp level → no AutoLeveler
    // kick, honest A/B vs the loudness-normalized capture. Drive then changes CHARACTER, not loudness.
    // The Tube Output knob is stripped from the core and re-applied here (tubeOutGain), on top of the match.
    cab::AutoLeveler tubeLevel;
    float tubeOutGain      = 1.0f;
    bool  tubeSeed         = true;                  // snap the leveler to the converged gain on (re)activation
    bool  tubeRenderedLast = false;                 // detect tube (re)activation → trigger the seed
};

} // namespace cab::poweramp
