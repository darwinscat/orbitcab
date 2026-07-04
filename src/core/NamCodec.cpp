// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "NamCodec.h"

#include <json.hpp>   // nlohmann/json — vendored via nam_core's Dependencies/nlohmann (hidden here)

#include <cstring>
#include <string>
#include <vector>

static_assert (sizeof (float) == 4, "namz assumes 32-bit IEEE-754 float");

namespace namz
{
namespace
{
    constexpr char kMagic[4] = { 'N', 'A', 'M', 'Z' };

    using json = nlohmann::json;

    // codec = store: the body is written verbatim (float bytes byte-plane shuffled, but NOT compressed).
    // That keeps `.namz` DETERMINISTIC and byte-identical across platforms — they're committed to git —
    // and zlib-free. The shuffle is free and helps whatever OUTER compressor sees the file (the installer's
    // LZMA, git's packing); an inner deflate there is redundant. Corruption is caught by the length checks
    // in unpack(), and the raw-body size is capped against the reconstructed-JSON cap (zip-bomb guard).

    //== byte-plane shuffle: AoS float bytes -> SoA planes (groups the structured sign/exponent
    //== bytes apart from the noisy mantissa, so the OUTER compressor models them better). Reversible. ==
    void shuffleInto (const float* src, size_t count, juce::uint8* dst)
    {
        const auto* s = reinterpret_cast<const juce::uint8*> (src);
        for (size_t i = 0; i < count; ++i)
        {
            dst[0 * count + i] = s[4 * i + 0];
            dst[1 * count + i] = s[4 * i + 1];
            dst[2 * count + i] = s[4 * i + 2];
            dst[3 * count + i] = s[4 * i + 3];
        }
    }

    void unshuffleInto (const juce::uint8* src, size_t count, float* dst)
    {
        auto* d = reinterpret_cast<juce::uint8*> (dst);
        for (size_t i = 0; i < count; ++i)
        {
            d[4 * i + 0] = src[0 * count + i];
            d[4 * i + 1] = src[1 * count + i];
            d[4 * i + 2] = src[2 * count + i];
            d[4 * i + 3] = src[3 * count + i];
        }
    }

    bool isNumericWeights (const json& v)
    {
        if (! v.is_array())
            return false;
        for (const auto& x : v)
            if (! x.is_number())
                return false;
        return true;
    }

    // Type a --set string value for JSON: "true"/"false" → bool, all-digits → integer, else string.
    // So `--set boost=true` lands as a real bool and `--set gain_hours=16` as a number.
    json typeValue (const juce::String& s)
    {
        if (s == "true")  return true;
        if (s == "false") return false;
        if (s.isNotEmpty() && s.containsOnly ("0123456789")) return (long long) s.getLargeIntValue();
        return s.toStdString();
    }

    // DFS: pull every numeric "weights" array out into `out` (in traversal order) and
    // replace its value in the tree with its ordinal index (a JSON integer). The stripped
    // tree is the skeleton; the index makes rehydration order-independent.
    void extractWeights (json& node, std::vector<std::vector<float>>& out)
    {
        if (node.is_object())
        {
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                if (it.key() == "weights" && isNumericWeights (it.value()))
                {
                    std::vector<float> w;
                    w.reserve (it.value().size());
                    for (const auto& x : it.value())
                        w.push_back ((float) x.get<double>());
                    const auto idx = (long long) out.size();
                    out.push_back (std::move (w));
                    it.value() = idx;              // array -> ordinal index
                }
                else
                {
                    extractWeights (it.value(), out);
                }
            }
        }
        else if (node.is_array())
        {
            for (auto& v : node)
                extractWeights (v, out);
        }
    }

    // DFS inverse: wherever a "weights" key holds an integer index, swap in that float segment. `ok`
    // is cleared if any placeholder cannot be filled (idx out of range) — the signature of a truncated
    // or corrupt stream, so the caller rejects instead of emitting a JSON with a bare ordinal for weights.
    void refillWeights (json& node, const std::vector<std::vector<float>>& segs, bool& ok)
    {
        if (node.is_object())
        {
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                if (it.key() == "weights" && it.value().is_number_integer())
                {
                    const auto idx = it.value().get<long long>();
                    if (idx >= 0 && idx < (long long) segs.size())
                        it.value() = segs[(size_t) idx];   // vector<float> -> JSON number array
                    else
                        ok = false;
                }
                else
                {
                    refillWeights (it.value(), segs, ok);
                }
            }
        }
        else if (node.is_array())
        {
            for (auto& v : node)
                refillWeights (v, segs, ok);
        }
    }
} // namespace

//==============================================================================
bool isNamz (const void* data, std::size_t n) noexcept
{
    return data != nullptr && n >= sizeof (kMagic)
        && std::memcmp (data, kMagic, sizeof (kMagic)) == 0;
}

//==============================================================================
juce::MemoryBlock pack (const void* namJson, std::size_t n, PackOptions opts)
{
    std::vector<std::vector<float>> arrays;
    std::string skeleton;
    std::string headerMeta;                         // display metadata JSON for the cheap header block
    try
    {
        auto j = json::parse (static_cast<const char*> (namJson),
                              static_cast<const char*> (namJson) + n);

        // Provenance stamping (optional): set/overwrite fields in `metadata` (typed: true/false → bool,
        // digits → integer, else string). The SAME set is mirrored into the header block so a reader can
        // pull tone_type / boost / gear without inflating the weights (readMeta).
        const auto metaKeys = opts.metadata.getAllKeys();
        if (! metaKeys.isEmpty())
        {
            if (! j.contains ("metadata") || ! j["metadata"].is_object())
                j["metadata"] = json::object();
            json hdr = json::object();
            for (const auto& k : metaKeys)
            {
                const auto val = typeValue (opts.metadata[k]);
                j["metadata"][k.toStdString()] = val;
                hdr[k.toStdString()]           = val;
            }
            headerMeta = hdr.dump();
            if (headerMeta.size() > 0xFFFF)         // u16 length field — display metadata is tiny; never hit
                headerMeta.clear();
        }

        extractWeights (j, arrays);
        skeleton = j.dump();                        // minified, weights replaced by indices
    }
    catch (...) { return {}; }

    // Assemble the stored body (skeleton + lengths + float payload; shuffled but not compressed).
    size_t totalFloats = 0;
    for (const auto& a : arrays)
        totalFloats += a.size();

    juce::MemoryBlock plain;
    {
        juce::MemoryOutputStream mos (plain, false);
        mos.writeInt ((int) skeleton.size());                    // u32 LE
        mos.write (skeleton.data(), skeleton.size());
        mos.writeInt ((int) arrays.size());
        for (const auto& a : arrays)
            mos.writeInt ((int) a.size());

        if (opts.shuffle && totalFloats > 0)
        {
            std::vector<float> flat;
            flat.reserve (totalFloats);
            for (const auto& a : arrays)
                flat.insert (flat.end(), a.begin(), a.end());
            std::vector<juce::uint8> shuffled (totalFloats * 4);
            shuffleInto (flat.data(), totalFloats, shuffled.data());
            mos.write (shuffled.data(), shuffled.size());
        }
        else
        {
            for (const auto& a : arrays)
                if (! a.empty())
                    mos.write (a.data(), a.size() * sizeof (float));
        }
    }

    juce::MemoryBlock out;
    {
        juce::MemoryOutputStream hdr (out, false);
        hdr.write (kMagic, sizeof (kMagic));
        hdr.writeByte ((char) kFormatVersion);
        hdr.writeByte ((char) CodecStore);
        hdr.writeByte ((char) DtypeF32);
        hdr.writeByte ((char) (opts.shuffle ? FlagShuffle : 0));
        hdr.writeShort ((short) (juce::uint16) headerMeta.size());   // metaLen (v2), little-endian
        if (! headerMeta.empty())
            hdr.write (headerMeta.data(), headerMeta.size());
        hdr.write (plain.getData(), plain.getSize());                // body stored verbatim (no compression)
    }
    // The MemoryOutputStream MUST be destroyed (flushed) before we return `out`: it over-allocates the
    // block and only trims it to the exact byte count on flush. Returning `out` while it's still open
    // leaks that trailing slack — harmless to the old inflate path (it stopped at the stream end) but the
    // store reader's exact-length check rejects it, so unpack() returned empty. It only surfaced on MSVC,
    // where the return copies `out` before the stream's destructor trims it (clang's NRVO hid the bug).
    return out;
}

//==============================================================================
juce::MemoryBlock unpack (const void* namz, std::size_t n, std::size_t maxJsonBytes)
{
    if (! isNamz (namz, n) || n < 8)
        return {};

    const auto* bytes = static_cast<const juce::uint8*> (namz);
    const juce::uint8 fmt   = bytes[4];
    const juce::uint8 codec = bytes[5];
    const juce::uint8 dtype = bytes[6];
    const juce::uint8 flags = bytes[7];
    if (fmt > kFormatVersion || codec != CodecStore || dtype != DtypeF32)
        return {};                                  // unknown/future variant — refuse cleanly

    // v2 carries a [u16 metaLen][meta] block before the stored body; v1 starts at byte 8.
    std::size_t off = 8;
    if (fmt >= 2)
    {
        if (n < 10) return {};
        off = 10 + (std::size_t) (juce::uint16) juce::ByteOrder::littleEndianShort (bytes + 8);
        if (off > n) return {};
    }

    // Body is stored verbatim (codec = store). Cap the raw size against the reconstructed-JSON cap so a
    // crafted oversized blob can't force a huge allocation before the per-field checks below catch it.
    if (n - off > maxJsonBytes + 4096)
        return {};
    juce::MemoryBlock plain (bytes + off, n - off);
    if (plain.getSize() == 0)
        return {};

    try
    {
        juce::MemoryInputStream in (plain.getData(), plain.getSize(), false);
        auto remaining = [&in] { return (size_t) juce::jmax ((juce::int64) 0, in.getNumBytesRemaining()); };

        if (remaining() < 4) return {};
        const auto skeletonLen = (size_t) (juce::uint32) in.readInt();
        if (skeletonLen > remaining()) return {};
        std::string skeleton (skeletonLen, '\0');
        if (skeletonLen > 0 && in.read (skeleton.data(), (int) skeletonLen) != (int) skeletonLen) return {};

        if (remaining() < 4) return {};
        const auto numArrays = (size_t) (juce::uint32) in.readInt();
        if (numArrays > remaining() / 4) return {};            // can't hold that many u32 lengths → truncated / OOM guard
        std::vector<size_t> lengths (numArrays);
        size_t totalFloats = 0;
        for (size_t i = 0; i < numArrays; ++i)
        {
            lengths[i] = (size_t) (juce::uint32) in.readInt();
            totalFloats += lengths[i];
        }
        // The float32 payload must be EXACTLY the rest of the buffer (division, not multiply → no overflow).
        // Rejects truncation and lying/short lengths, which would otherwise slice garbage into the weights.
        if (remaining() % sizeof (float) != 0 || totalFloats != remaining() / sizeof (float)) return {};

        std::vector<std::vector<float>> segs (numArrays);
        if (totalFloats > 0)
        {
            std::vector<float> flat (totalFloats);
            if ((flags & FlagShuffle) != 0)
            {
                std::vector<juce::uint8> shuffled (totalFloats * 4);
                if (in.read (shuffled.data(), (int) shuffled.size()) != (int) shuffled.size()) return {};
                unshuffleInto (shuffled.data(), totalFloats, flat.data());
            }
            else if (in.read (flat.data(), (int) (totalFloats * sizeof (float))) != (int) (totalFloats * sizeof (float)))
            {
                return {};
            }
            size_t o = 0;
            for (size_t i = 0; i < numArrays; ++i)
            {
                segs[i].assign (flat.begin() + (long) o, flat.begin() + (long) (o + lengths[i]));
                o += lengths[i];
            }
        }

        auto j = json::parse (skeleton);
        bool refillOk = true;
        refillWeights (j, segs, refillOk);
        if (! refillOk) return {};   // an unfillable weight placeholder → truncated/corrupt input
        const auto rebuilt = j.dump();
        if (rebuilt.size() > maxJsonBytes)
            return {};

        juce::MemoryBlock out;
        out.append (rebuilt.data(), rebuilt.size());
        return out;
    }
    catch (...) { return {}; }
}

//==============================================================================
juce::StringPairArray readMeta (const void* namz, std::size_t n)
{
    juce::StringPairArray out;
    if (! isNamz (namz, n) || n < 10)
        return out;
    const auto* bytes = static_cast<const juce::uint8*> (namz);
    if (bytes[4] < 2)                                           // v1 has no meta block
        return out;
    const std::size_t metaLen = (std::size_t) (juce::uint16) juce::ByteOrder::littleEndianShort (bytes + 8);
    if (metaLen == 0 || 10 + metaLen > n)
        return out;
    try
    {
        auto j = json::parse (bytes + 10, bytes + 10 + metaLen);
        if (j.is_object())
            for (auto it = j.begin(); it != j.end(); ++it)
                out.set (juce::String (it.key()),
                         it.value().is_string() ? juce::String (it.value().get<std::string>())
                                                : juce::String (it.value().dump()));   // bool/number → text
    }
    catch (...) { return {}; }
    return out;
}

} // namespace namz
