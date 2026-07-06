// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "ReverbBinaryData.h"
#include <juce_core/juce_core.h>

#include <algorithm>
#include <vector>

//==============================================================================
// orbitcab::ReverbLibrary — the one place that enumerates the bundled CC0 spring-reverb IRs.
// They live in their OWN binary-data target/namespace (ReverbBinaryData), NOT the cab's BinaryData,
// precisely so orbitcab::bundledIRs() (which walks every .wav in BinaryData) never lists them as cabs.
//
// Order is the contract: natural-sorted by filename (01-… < 02-… < …), so the "reverbType" choice
// param maps index 1..N onto reverbIRs()[0..N-1] (index 0 = Off, no IR). Keep the StringArray in
// Parameters.cpp ("Off", then the springs) in the SAME order as these files. Sources + licenses:
// resources/reverb-ir/README.md and docs/ASSET-LICENSES.md (all CC0).
//==============================================================================
namespace orbitcab
{

struct ReverbIR
{
    juce::String name;      // display name parsed from the filename ("01-tube.wav" → "Tube")
    juce::String file;      // original filename
    const char*  data = nullptr;
    int          size = 0;
};

// Strip the "NN-" ordering prefix + extension, capitalize → a display name ("01-tube.wav" → "Tube").
inline juce::String reverbDisplayName (const juce::String& filename)
{
    juce::String stem = filename.upToLastOccurrenceOf (".", false, false);   // drop extension
    const int dash = stem.indexOfChar ('-');
    if (dash > 0 && stem.substring (0, dash).containsOnly ("0123456789"))
        stem = stem.substring (dash + 1);
    return stem.isEmpty() ? filename
                          : stem.substring (0, 1).toUpperCase() + stem.substring (1);
}

// All bundled spring IRs, natural-sorted by filename (so reverbType index i maps to [i-1]).
inline std::vector<ReverbIR> reverbIRs()
{
    std::vector<ReverbIR> out;
    for (int i = 0; i < ReverbBinaryData::namedResourceListSize; ++i)
    {
        const juce::String filename (ReverbBinaryData::originalFilenames[i]);
        if (! filename.endsWithIgnoreCase (".wav"))
            continue;
        int size = 0;
        const char* data = ReverbBinaryData::getNamedResource (ReverbBinaryData::namedResourceList[i], size);
        if (data == nullptr || size <= 0)
            continue;
        out.push_back ({ reverbDisplayName (filename), filename, data, size });
    }
    std::sort (out.begin(), out.end(),
               [] (const ReverbIR& a, const ReverbIR& b) { return a.file.compareNatural (b.file) < 0; });
    return out;
}

} // namespace orbitcab
