// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_core/juce_core.h>   // jassert

#include <type_traits>
#include <utility>

namespace cab
{

//==============================================================================
// THE CORE'S VERDICT, WHERE THE CORE GIVES ONE. felitronics-core v0.30.0 (its law 11) made every block-level
// process() and several prepare()s return [[nodiscard]] bool — "accepted and honoured in full" — and a refused
// call touches NOTHING: a NAM stage that refuses leaves the dry signal where the amp should be, a gate that
// refuses leaves the block ungated. v0.13.1 returns void from the same calls, and the tree builds against both.
//
// Every call routed through here is one this plugin cannot get refused: widths are at most 2
// (isBusesLayoutSupported accepts mono→mono and stereo→stereo only), a block never exceeds the prepared
// maxBlock (CabEngine::process clamps to it), and host rates sit far inside every module's domain. So a
// refusal is a BROKEN INVARIANT, not a runtime condition — loud in a debug build, and never silently swallowed
// in the source the way an ignored [[nodiscard]] is.
//==============================================================================
template <typename Call>
[[nodiscard]] inline bool verdictOf (Call&& call)
{
    if constexpr (std::is_void_v<decltype (call())>)
    {
        call();
        return true;       // this core gives no verdict (felitronics-core v0.13.1)
    }
    else
    {
        return call();
    }
}

template <typename Call>
inline void expectAccepted (Call&& call)
{
    [[maybe_unused]] const bool accepted = verdictOf (std::forward<Call> (call));
    jassert (accepted);    // unreachable in this plugin by construction — see above
}

} // namespace cab
