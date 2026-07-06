// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "BinaryData.h"
#include <juce_core/juce_core.h>

#include <algorithm>
#include <vector>

//==============================================================================
// orbitcab::FactoryPresets — the one place that enumerates the bundled, read-only
// factory presets (.orbitcab blobs embedded via juce_add_binary_data). Mirrors
// IRLibrary: both the processor (first-start default) and the editor (the combo's
// Factory section) read from here, so "which presets ship" lives in one source.
// Dropping a .orbitcab into resources/presets/ ships it (no code change).
//==============================================================================
namespace orbitcab
{

// The preset loaded on first start / "reset to default". Must match a bundled
// preset's name (filename without extension); falls back to the bootstrap default.
inline constexpr const char* kDefaultPresetName = "Roche Limit";

struct FactoryPreset
{
    juce::String name;          // filename without ".orbitcab" (== the preset's display name)
    const char*  data = nullptr;   // points into static BinaryData (always valid)
    int          size = 0;
};

// All bundled factory presets, natural-sorted by name.
inline std::vector<FactoryPreset> factoryPresets()
{
    std::vector<FactoryPreset> out;
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        const juce::String filename (BinaryData::originalFilenames[i]);
        if (! filename.endsWithIgnoreCase (".orbitcab"))
            continue;
        int size = 0;
        const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
        if (data == nullptr || size <= 0)
            continue;
        out.push_back ({ filename.dropLastCharacters ((int) std::strlen (".orbitcab")), data, size });
    }
    std::sort (out.begin(), out.end(),
               [] (const FactoryPreset& a, const FactoryPreset& b) { return a.name.compareNatural (b.name) < 0; });
    return out;
}

// Find a bundled preset by name. The returned data pointer is into static BinaryData,
// so returning by value is safe (data == nullptr if not found).
inline FactoryPreset findFactoryPreset (const juce::String& name)
{
    for (auto& p : factoryPresets())
        if (p.name == name)
            return p;
    return {};
}

} // namespace orbitcab
