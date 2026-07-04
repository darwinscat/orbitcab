// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_core/juce_core.h>

#include <utility>
#include <vector>

//==============================================================================
// orbitcab::ui — the DATA model for a preamp's active device(s): the type enum, the string
// parsers, and the (possibly HYBRID) "device spec". Pure juce_core — NO graphics — so it is
// unit-testable in isolation; the schematic drawing lives in DeviceGlyph.h which includes this.
//==============================================================================
namespace orbitcab::ui
{

enum class DeviceType { none, tube, pnp, fet, dsp, diode };

inline DeviceType deviceFromString (const juce::String& s)
{
    const auto l = s.trim().toLowerCase();
    if (l == "tube" || l == "valve")          return DeviceType::tube;
    if (l == "pnp"  || l == "npn" || l == "bjt" || l == "transistor") return DeviceType::pnp;
    if (l == "fet"  || l == "jfet" || l == "mosfet") return DeviceType::fet;
    if (l == "dsp"  || l == "chip" || l == "ic" || l == "digital")    return DeviceType::dsp;
    if (l == "diode")                         return DeviceType::diode;
    return DeviceType::none;
}

//==============================================================================
// A device SPEC lists the active devices of a (possibly HYBRID) preamp, in signal order:
//   "tube:1"        → one triode           (Volt-style)
//   "tube:4"        → four triodes          (V4)
//   "pnp:1"         → one transistor        (solid-state ISA)
//   "tube:1,pnp:1"  → a tube AND a transistor (a hybrid — e.g. the ReVolt)
// Stored in the model metadata under "device". A bare "tube" means count 1; unknown types and
// non-positive counts are dropped, so a malformed spec yields fewer entries, never garbage.
using DeviceSpec = std::vector<std::pair<DeviceType, int>>;

inline DeviceSpec parseDeviceSpec (const juce::String& s)
{
    DeviceSpec out;
    for (auto tok : juce::StringArray::fromTokens (s, ",", ""))
    {
        tok = tok.trim();
        if (tok.isEmpty())
            continue;
        const auto type = deviceFromString (tok.upToFirstOccurrenceOf (":", false, false));
        const int  cnt  = tok.contains (":") ? tok.fromFirstOccurrenceOf (":", false, false).trim().getIntValue() : 1;
        if (type != DeviceType::none && cnt > 0)
            out.push_back ({ type, juce::jmin (cnt, 12) });
    }
    return out;
}

// Total glyph count across the spec (clamped — bounds the drawn row + the popup width reservation).
inline int deviceSpecCount (const DeviceSpec& spec)
{
    int n = 0;
    for (const auto& p : spec) n += p.second;
    return juce::jlimit (0, 12, n);
}

} // namespace orbitcab::ui
