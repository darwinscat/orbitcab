// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_core/juce_core.h>

#include <cstddef>

//==============================================================================
// namz — a tiny lossless codec for NeuralAmpModeler `.nam` files.
//
// A `.nam` is JSON whose bulk is one or more flat `"weights"` arrays written as
// full-precision DECIMAL STRINGS (~20 chars/number). The NAM engine loads those
// weights into `std::vector<float>` (float32) — so the decimals are truncated to
// float32 on load ANYWAY. `.namz` stores each weight as a 4-byte float32 instead
// of ~20 bytes of text, then deflates: ~6x smaller than the raw JSON and BIT-EXACT
// to what the engine computes (zero quality loss). Everything except the weight
// arrays (architecture/config/metadata) is preserved verbatim in a JSON skeleton.
//
// Used in two places, one currency: the bundled factory library (shipped `.namz`)
// and the embedded-in-preset/session pool (a self-contained saved sound). The
// public surface is juce-only — nlohmann/json is hidden in the .cpp so NAM never
// leaks into the rest of the codebase (mirrors AmpStage's pImpl).
//
// Wire format (all multi-byte ints little-endian; targets are all LE):
//   [0..3]  magic  'N','A','M','Z'
//   [4]     formatVersion (2; readers accept 1 = no meta block)
//   [5]     codec   (0 = deflate/zlib via JUCE; 1 = zstd reserved)
//   [6]     dtype   (0 = float32;               1 = float16 reserved, lossy)
//   [7]     flags   (bit0 = weight bytes shuffled into 4 byte-planes)
//   [8..9]  metaLen (u16; v2 only) — bytes of an UNCOMPRESSED display-metadata JSON that follows,
//           readable via readMeta() WITHOUT inflating the weights (tone_type/boost/gear/…). 0 = none.
//   [meta]  metaLen bytes of that JSON (v2 only)
//   [..]    deflate stream; inflates to:
//             u32 skeletonLen
//             u8  skeleton[skeletonLen]   (minified JSON; each numeric "weights"
//                                          array replaced by its ordinal index)
//             u32 numArrays
//             u32 lengths[numArrays]      (float count of each weights array)
//             u8  payload[]               (sum(lengths) * sizeof(dtype) bytes,
//                                          byte-shuffled iff flags bit0)
//==============================================================================
namespace namz
{

inline constexpr juce::uint8 kFormatVersion = 2;

enum Codec : juce::uint8 { CodecDeflate = 0 /*, CodecZstd = 1 (reserved) */ };
enum Dtype : juce::uint8 { DtypeF32     = 0 /*, DtypeF16 = 1 (reserved)   */ };
enum Flags : juce::uint8 { FlagShuffle  = 1u << 0 };

struct PackOptions
{
    bool shuffle = true;   // split float bytes into 4 planes before deflate (+~6%, lossless)

    // Optional string fields to set/overwrite in the top-level `metadata` object before packing
    // (provenance: modeled_by / name / gear_type / tone_type …). Purely descriptive — the DSP
    // ignores them — but they travel with the model. Empty = leave metadata untouched.
    juce::StringPairArray metadata;
};

// True if `data` begins with the `.namz` magic. Cheap; safe on any/short buffer.
bool isNamz (const void* data, std::size_t n) noexcept;

// Read the display-metadata block (tone_type / boost / gear_* / name / modeled_by …) WITHOUT
// inflating the weights — cheap enough to call per model at library-enumeration time. Empty for a
// v1 `.namz`, a non-`.namz` buffer, or one packed without metadata. Values are returned as strings
// (bool → "true"/"false", number → its digits).
juce::StringPairArray readMeta (const void* namz, std::size_t n);

// Parse NAM JSON (`.nam`) bytes → packed `.namz`. Returns an EMPTY block on failure
// (not valid JSON, non-numeric weights, etc.). Lossless w.r.t. the float32 engine.
juce::MemoryBlock pack (const void* namJson, std::size_t n, PackOptions opts = {});

// Inverse: `.namz` bytes → reconstructed `.nam` JSON bytes (weights rehydrated as
// float32 numbers). `maxJsonBytes` caps the inflated output (zip-bomb guard).
// Returns an EMPTY block on failure / over-cap / unknown codec|dtype.
juce::MemoryBlock unpack (const void* namz, std::size_t n, std::size_t maxJsonBytes);

} // namespace namz
