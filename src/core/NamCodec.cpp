// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.
//
// Thin juce adapter over the `namz` library (github.com/darwinscat/namz v1.0.0, MIT). The codec is the
// library's single header `namz.h`; here we only convert at the juce boundary so OrbitCab keeps its
// juce-only surface and nlohmann stays confined to this translation unit (mirrors AmpStage's pImpl).

#include "NamCodec.h"

#include <cstdint>
#include <vector>

#define NAMZ_IMPLEMENTATION
#include <namz.h>          // the extracted library; brings <nlohmann/json.hpp> (via nam_core's vendored copy)

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
