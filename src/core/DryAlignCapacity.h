// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "AmpStage.h"

#include <concepts>

namespace cab
{

//==============================================================================
// How long a ring the dry path of a bypassed NAM stage needs — ASKED of the core, never derived here.
//
// A bypassed stage's dry is held at the stage's reported rate-match latency (cab::DryAligner), and the ring
// clamps a longer tap to capacity-1 SILENTLY: the host still compensates the full latency, the dry arrives
// early, and an on/off crossfade combs. The literal this replaces — 256, justified as
// "ceil(3·hostSR/modelSR)+3, ≤ ~27 even at 384 kHz" — was sized on the formula of an older resampler kernel;
// under the 64-tap kernel the core reports 267 at 352.8 kHz and 288 at 384 kHz, and the ring delivered 255.
//
// felitronics-core ≥ v0.30.0 publishes the answer: NamStage::maxLatencySamples (hostSR), an upper bound over
// every model the stage will accept at that host rate — "the number a consumer sizing a fixed delay line
// actually needs" — plus one, because the ring's usable range is capacity-1. Asked in prepare() and nowhere
// else, so the audio thread neither grows the ring nor clamps into it.
//==============================================================================
template <typename Stage>
concept PublishesLatencyBound = requires (double hostSR)
{
    { Stage::maxLatencySamples (hostSR) } -> std::convertible_to<int>;
};

template <typename Stage = AmpStage>
int namDryAlignCapacity (double hostSR)
{
    if constexpr (PublishesLatencyBound<Stage>)
        return Stage::maxLatencySamples (hostSR) + 1;
    else
        // felitronics-core v0.13.1 publishes no bound. The latency it REPORTS — the number the tap asks this ring
        // for — is ceil(3·hostSR/modelRunSR) + 3 (modules/nam/src/NamStage.cpp:149 at that tag; longer than its
        // path's true delay, which is why the core replaced it): 27 at 384 kHz for a 48 kHz model, and within this
        // ring's 255 for any model rate above ~4.6 kHz at that host. Here only so the tree still builds against
        // the old pin; it goes when that pin is no longer built.
        return 256;
}

} // namespace cab
