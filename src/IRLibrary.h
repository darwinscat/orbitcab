// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "BinaryData.h"
#include <juce_core/juce_core.h>

#include <algorithm>
#include <vector>

//==============================================================================
// orbitcab::IRLibrary — the one place that enumerates the bundled CC0 IRs. Used by
// BOTH the processor's by-name load and the editor's browser list, so "what bundled
// IRs exist / which pack / what order" lives in a single source (dedup).
//==============================================================================
namespace orbitcab
{

struct BundledIR
{
    juce::String name;     // original filename, e.g. "01-cookie-monster.wav"
    juce::String pack;     // "Brutal" (01–15) or "Emerald" (16–21)
    const char*  data = nullptr;
    int          size = 0;
};

// Pack split mirrors the bundle: 01–15 = Brutal, 16–21 = Emerald.
inline juce::String packForBundled (const juce::String& filename)
{
    const int n = filename.getIntValue();
    return (n >= 1 && n <= 15) ? "Brutal" : "Emerald";
}

// All bundled *.wav IRs, natural-sorted by filename (so "2-" sorts before "10-").
inline std::vector<BundledIR> bundledIRs()
{
    std::vector<BundledIR> out;
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        const juce::String filename (BinaryData::originalFilenames[i]);
        if (! filename.endsWithIgnoreCase (".wav"))
            continue;
        int size = 0;
        const char* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
        if (data == nullptr || size <= 0)
            continue;
        out.push_back ({ filename, packForBundled (filename), data, size });
    }
    std::sort (out.begin(), out.end(),
               [] (const BundledIR& a, const BundledIR& b) { return a.name.compareNatural (b.name) < 0; });
    return out;
}

} // namespace orbitcab
