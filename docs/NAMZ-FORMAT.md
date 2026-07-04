<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# `.namz` — packed NeuralAmpModeler captures

A `.nam` file is JSON whose bulk is one or more flat `"weights"` arrays written as full-precision
**decimal strings** (~20 chars per number). The NAM engine (`NeuralAmpModelerCore`, built here with
`NAM_SAMPLE_FLOAT`) loads those weights into `std::vector<float>` — so the decimals are truncated to
**float32 on load anyway**.

`.namz` stores each weight as a 4-byte **float32** instead of ~20 bytes of text: **≈5.5× smaller** than
the raw JSON, and **bit-exact** to what the engine computes (zero quality loss). Everything except the
weight arrays (architecture / config / metadata) is preserved verbatim. The float bytes are byte-plane
**shuffled** but **not compressed** (`codec = store`) — that keeps the bytes deterministic and identical
across platforms (they're committed to git) and zlib-free, while the shuffle still helps whatever OUTER
compressor sees the file (the installer's LZMA, git's packing).

Used in two places, one currency:
- the bundled **factory library** (shipped as `.namz` under `resources/preamps/` etc.), and
- the **embedded-in-preset/session pool** (a self-contained saved sound — state `v7`).

Codec: `src/core/NamCodec.{h,cpp}` (public surface is juce-only; nlohmann/json is hidden in the `.cpp`).
Tests: `tests/NamCodecTests.cpp` (contract-first, adversarial). CLI: `tools/namz/namzpack.cpp`.

## Wire format

All multi-byte integers are **little-endian** (every shipping target is LE).

```
[0..3]   magic          'N','A','M','Z'
[4]      formatVersion  2   (readers still accept 1 = no meta block)
[5]      codec          0 = store/uncompressed;       1 = deflate (reserved), 2 = zstd (reserved)
[6]      dtype          0 = float32;                  1 = float16 (reserved, lossy)
[7]      flags          bit0 = weight bytes shuffled into 4 byte-planes (lossless; groups the
                               structured bytes so the OUTER compressor squeezes ~6% more)
[8..9]   metaLen        u16 — bytes of the display-metadata JSON that follows (v2 only)
[meta]   metaLen bytes  small JSON of display fields — read via readMeta() WITHOUT touching the weights
[..]     body (codec 0 = stored verbatim):
             u32  skeletonLen
             u8   skeleton[skeletonLen]   minified JSON; each numeric "weights" array is replaced by
                                          its ordinal integer index
             u32  numArrays
             u32  lengths[numArrays]      float count of each weights array (in index order)
             u8   payload[]               sum(lengths) * 4 bytes of float32 (byte-shuffled iff flags bit0)
```

`unpack` un-shuffles the payload, rebuilds the JSON (weights re-inserted as float32 numbers) and hands it
to the existing `nlohmann::json::parse → nam::get_dsp` path, so the loaded model is identical.

## Contracts (enforced by tests)

- **Lossless to float32** — for every weight, `unpack(pack(x)) == (float)x`, bit-exact, at any nesting
  depth (incl. `SlimmableContainer` submodels). Verified across `-0.0`, subnormals, `FLT_MAX`, and
  precision/tie cases.
- **Metadata/config preserved** verbatim — nested objects, `null`, unicode, big doubles, escapes.
- **Deterministic** — `pack(x)` is byte-identical across runs **and across platforms** (no compressor,
  so no zlib/gzip header variation); outputs are committed to git. The codec is idempotent
  (`pack(unpack(pack(x))) == pack(x)`).
- **Robust** — `unpack` never crashes/hangs/OOMs; every malformed input (bad magic, unknown
  version/codec/dtype, truncation at any byte, a lying `metaLen`, a huge `numArrays`, an oversized body
  over the output cap) is rejected cleanly and returns empty.
- **Compatible** — `formatVersion 1` blobs (no meta block) still unpack; `readMeta` returns empty for
  v1 / non-`.namz`, and the typed display fields for v2.

> Out of contract: non-finite weights (`NaN`/`±Inf`). Real NAM captures never contain them; JSON can't
> represent them either. The codec targets the finite float32 range NAM produces.

## Metadata (the header block)

`pack` can stamp/overwrite display fields (via `--set`, typed: `true`/`false`→bool, digits→int, else
string). The same set is mirrored into the small header block so the UI can read it cheaply
(`readMeta`) — no weight inflate. Fields the plugin reads:

| field | example | used for |
|---|---|---|
| `modeled_by` | `Darwin's Cat` | provenance / tooltip |
| `gear_make` / `gear_model` | `Two Notes` / `ReVolt Guitar` | the caption above the preamp combo |
| `gear_type` | `preamp` | — |
| `tone_type` | `clean` / `crunch` / `hi-gain` | the channel button captions |
| `boost` | `true` | boost indicator |
| `device` | `tube:1,pnp:1` | the schematic device glyphs (see below) |

**`device` spec** — a comma-separated list of `type:count` in signal order; supports hybrids:
`tube` (triode), `pnp`/`npn`/`bjt`/`transistor`, `fet`/`jfet`/`mosfet`, `dsp`/`chip`/`ic` (chip), `diode`.
Examples: `tube:4` (a V4), `pnp:1` (the ISA), `tube:1,pnp:1` (the ReVolt — a tube + a transistor).

## The `orbitcab_namz` CLI (`tools/namz`)

The **same codec** the plugin uses, so a packed library is byte-for-byte what the plugin consumes.

| command | does |
|---|---|
| `pack   <in.nam> <out.namz> [--set k=v …] [--no-shuffle]` | pack (+ stamp metadata) |
| `unpack <in.namz> <out.nam>` | unpack back to `.nam` |
| `pack-dir <src> <dst>` / `unpack-dir <src> <dst>` | a whole folder |
| `restamp <in> <out> --set k=v …` | rewrite metadata losslessly (unpack + pack) |
| `meta   <in.namz>` | print the header metadata (cheap, no weight inflate) |
| `verify <in.nam \| dir>` | pack→unpack round-trip check + ratio |

## Provenance

Raw `.nam` **masters** are the source of truth for re-quantization/attribution and live in the private
capture repo (`orbitcab-nam-capture`), not in this public repo — the public repo ships the lossless
`.namz`. Because `.namz → .nam` is fully reversible (`unpack-dir`), the packed library is never a dead end.
