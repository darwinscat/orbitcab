// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.
//
// Thin juce adapter over the `namz` library (github.com/darwinscat/namz v1.1.1, MIT). felitronics-core's
// NAM module compiles the single implementation; here we only convert at the juce boundary so OrbitCab
// keeps its juce-only surface and nlohmann stays confined to this translation unit.

#include "NamCodec.h"

#include <cstdint>
#include <vector>

#include <namz.h>          // declarations; implementation symbols are supplied by felitronics::nam

namespace ocnam
{
namespace
{
    juce::MemoryBlock toBlock (const std::vector<std::uint8_t>& v)
    {
        return v.empty() ? juce::MemoryBlock() : juce::MemoryBlock (v.data(), v.size());
    }
}

bool isNamz (const void* data, std::size_t n) noexcept
{
    return namz::isNamz (data, n);
}

juce::MemoryBlock pack (const void* namJson, std::size_t n, PackOptions opts)
{
    namz::PackOptions o;
    o.shuffle = opts.shuffle;
    for (const auto& key : opts.metadata.getAllKeys())
        o.metadata[key.toStdString()] = opts.metadata[key].toStdString();
    return toBlock (namz::pack (namJson, n, o));
}

juce::MemoryBlock unpack (const void* blob, std::size_t n, std::size_t maxJsonBytes)
{
    return toBlock (namz::unpack (blob, n, maxJsonBytes));
}

juce::StringPairArray readMeta (const void* blob, std::size_t n)
{
    juce::StringPairArray out;
    for (const auto& kv : namz::readMeta (blob, n))
        out.set (juce::String (kv.first), juce::String (kv.second));
    return out;
}

} // namespace ocnam
