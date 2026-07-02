// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_dsp/juce_dsp.h>          // AudioBuffer scratch + SmoothedValue fade
#include "TubePowerAmp.h"
#include "../core/Params.h"             // cab::PowerAmpMode
#include "../core/DryAligner.h"         // cab::DryAligner — latency-aligned dry for the OFF path

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
// Latency alignment (BOTH modes): the active stage has host-rate latency — the tube's oversampling
// (~31 samples), or the NAM capture's rate-match (0 at 48 kHz, a handful of samples when resampling).
// If the reported PDC changed when the power toggled (0 ↔ stageLatency), the host would re-sync — an
// audible GAP — and the off↔active crossfade would blend a 0-latency dry against a late wet (comb /
// level jump). So in a given mode this router reports a CONSTANT latency (= that mode's stage) for
// BOTH power states and delays the OFF/dry path by the SAME amount (an always-warm delay line):
// toggling power never changes PDC (no gap) and the crossfade blends time-aligned signals (no jump).
// The delay amount is per-block dynamic (tube: fixed oversampling; capture: the loaded model's rate-
// match). A capture↔tube MODE switch DOES change PDC (deliberate, rare — a plugin-like re-sync there
// is expected); the frequent power toggle, within either mode, is fully aligned.
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

    int  tubeLatencySamples() const noexcept { return tube[0].latencySamples(); }   // tpp-based, invariant across OS factor

    // Crossfade telemetry for the engine's leveler snap: a route retarget is only real once the
    // router ACCEPTS it (a change arriving mid-fade is deferred to the fade's end), so the leveler
    // must key off these, not off the raw params (codex review point — automation faster than the
    // 30 ms fade would otherwise desync the snap from the audio).
    bool fadeJustStarted() const noexcept { return fadeStartedThisBlock; }   // valid after process()
    bool isFading()        const noexcept { return fading; }

    static constexpr int kNumOs = 5;                // OS-quality options, prepared upfront (RT-safe live switch)
    static constexpr int kOsFactor[kNumOs] = { 2, 4, 8, 16, 32 };   // all report latency 31 (tpp-based, factor-invariant)

private:
    enum class Active { off, capture, tube };
    static Active resolve (bool ampOn, PowerAmpMode mode) noexcept
    {
        return ! ampOn ? Active::off
                       : (mode == PowerAmpMode::tube ? Active::tube : Active::capture);
    }

    // Render one stage into `dst`. OFF emits the latency-aligned dry (the input delayed to the active
    // stage's PDC via `dryAligner`) so an off↔active crossfade stays time-aligned; when the active
    // stage reports 0 latency that dry already equals the raw input, so it's a plain pass-through.
    void render (Active a, float* const* dst, int numChannels, int numSamples, AmpStage& nam) noexcept;

    TubePowerAmp tube[kNumOs];                      // one instance per OS factor, all prepared; osSel picks which runs
    int          osSel = 1;                         // live-switchable (per-block) selector — no realloc on change (1 = 4×)
    juce::AudioBuffer<float>   scratch;             // preallocated: holds the fade-FROM render
    DryAligner                 dryAligner;          // latency-aligned dry for the OFF path (see DryAligner.h)
    juce::SmoothedValue<float> xfade { 1.0f };      // 0→1 ramp during a capture<->tube switch
    Active current  = Active::off;
    Active fadeFrom = Active::off;
    bool   fading   = false;
    bool   fadeStartedThisBlock = false;            // set by process() on the block a fade begins
    bool   seeded   = false;                        // first block after prepare jumps to target (no ramp)

    // Deterministic per-voicing/drive LEVEL-MATCH make-up: the drive-comp (autoComp) ducks the driven
    // level below dry by a measured amount = 0.95·(Drive − thr[voicing]); this make-up gain undoes it so
    // the tube sits at ~the dry/capture level → no level jump on enable/disable (the "kick"). Purely a
    // function of the params (no follower) → deterministic, no chase/swell. Recomputed per block.
    float  tubeMakeup = 1.0f;
};

} // namespace cab::poweramp
