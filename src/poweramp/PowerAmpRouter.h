// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_dsp/juce_dsp.h>          // AudioBuffer scratch + SmoothedValue fade
#include "TubePowerAmp.h"
#include "../core/Params.h"             // cab::PowerAmpMode

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
// Latency alignment (tube mode): the tube stage has real host-rate latency (its oversampling —
// ~31 samples), whereas OFF (dry) and the NAM capture are ~zero-latency. If the reported PDC
// changed when the SIMULATOR power toggled (0 ↔ tubeLatency), the host would re-sync — an audible
// GAP — and the off↔tube crossfade would blend a 0-latency dry against a 31-sample-late tube
// (comb / level jump). So in TUBE MODE this router reports a CONSTANT latency (= the tube's) for
// BOTH power states and delays the OFF/dry path by that same amount (an always-warm delay line):
// toggling the SIMULATOR never changes PDC (no gap) and the crossfade blends time-aligned signals
// (no jump). Capture mode stays ~zero-latency on both sides, so it needs no alignment; a capture↔
// tube MODE switch still changes PDC (deliberate, rare — a plugin-like re-sync there is expected).
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

    // Render one stage into `dst` (off in tube mode → the latency-aligned dry from `dryScratch`;
    // off in capture mode → no-op, raw dry passes through).
    void render (Active a, float* const* dst, int numChannels, int numSamples,
                 AmpStage& nam, bool tubeMode) noexcept;

    // Advance the dry-alignment delay once per block (kept warm EVERY block, any mode) and leave
    // `dryScratch` = the raw input delayed by the tube's latency — the dry the host expects at PDC
    // = tubeLatency, used by the OFF path in tube mode. No-op copy when the tube reports 0 latency.
    void advanceDryAlign (const float* const* io, int numChannels, int numSamples) noexcept;

    TubePowerAmp tube;
    juce::AudioBuffer<float>   scratch;             // preallocated: holds the fade-FROM render
    juce::AudioBuffer<float>   dryScratch;          // preallocated: the raw input delayed by tubeLatency
    juce::AudioBuffer<float>   dryRing;             // persistent alignLatency-sample delay history (per channel)
    int  dryRingPos   = 0;                          // shared write/read cursor into dryRing
    int  alignLatency = 0;                          // = tube.latencySamples(), captured in prepare()
    juce::SmoothedValue<float> xfade { 1.0f };      // 0→1 ramp during a capture<->tube switch
    Active current  = Active::off;
    Active fadeFrom = Active::off;
    bool   fading   = false;
    bool   seeded   = false;                        // first block after prepare jumps to target (no ramp)

    // Deterministic per-voicing/drive LEVEL-MATCH make-up: the drive-comp (autoComp) ducks the driven
    // level below dry by a measured amount = 0.95·(Drive − thr[voicing]); this make-up gain undoes it so
    // the tube sits at ~the dry/capture level → no level jump on enable/disable (the "kick"). Purely a
    // function of the params (no follower) → deterministic, no chase/swell. Recomputed per block.
    float  tubeMakeup = 1.0f;
};

} // namespace cab::poweramp
