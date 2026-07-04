// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Comprehensive, CONTRACT-first tests for namz:: — the lossless `.nam` (JSON) <-> `.namz` (packed
// float32) codec — plus the device-spec parser. These try to BREAK the codec, not mirror it:
// numeric edge weights (subnormal / -0 / FLT_MAX / precision), malformed structure (no/empty/non-array
// weights, nested, non-object JSON), a corrupted/truncated/oversized container at every boundary,
// oversized metadata, byte-shuffle invertibility, pack DETERMINISM (outputs are committed to git), and
// the v5/v6→v7 pool-upgrade path (gzip(raw .nam) → .namz). Crew-reviewed test plan.
//
// CONTRACTS under test:
//   • Lossless to float32: for every weight, unpack(pack(x)) == (float)x, bit-exact — at any nesting.
//   • Non-weight metadata/config preserved (nested, null, unicode, big doubles, special chars).
//   • Deterministic: pack(x) == pack(x), byte-identical. Idempotent: pack(unpack(pack(x))) == pack(x).
//   • shuffle on vs off round-trip to identical float32 weights.
//   • isNamz true ONLY on the magic; unpack rejects (empty, no crash/hang/OOM) every malformed input.
//   • v1 (no meta block) still unpacks; readMeta empty for v1/non-namz, typed fields for v2.
#include <juce_core/juce_core.h>

#include <json.hpp>
#include "core/NamCodec.h"
#include "ui/DeviceSpec.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace orbitcab::ui;

namespace
{
    constexpr size_t kCap = 64u * 1024u * 1024u;

    // Every numeric "weights" array (DFS order) as float32.
    void collect (const json& node, std::vector<std::vector<float>>& out)
    {
        if (node.is_object())
            for (auto it = node.begin(); it != node.end(); ++it)
            {
                if (it.key() == "weights" && it.value().is_array())
                {
                    std::vector<float> w;
                    for (const auto& x : it.value())
                        if (x.is_number()) w.push_back ((float) x.get<double>());
                    out.push_back (std::move (w));
                }
                else
                    collect (it.value(), out);
            }
        else if (node.is_array())
            for (const auto& v : node)
                collect (v, out);
    }

    std::vector<std::vector<float>> weightsOf (const json& j) { std::vector<std::vector<float>> w; collect (j, w); return w; }

    // Parse reconstructed .namz→json bytes (empty MemoryBlock → null json).
    json parseBlock (const juce::MemoryBlock& mb)
    {
        if (mb.getSize() == 0) return json();
        try { return json::parse (std::string ((const char*) mb.getData(), mb.getSize())); }
        catch (...) { return json(); }
    }

    // A structurally faithful mini-.nam with N top-level weights + a nested submodel weights array +
    // metadata (null, unicode, big double) + non-weight numeric arrays that must NOT be packed.
    json makeNam (const std::vector<double>& topWeights, const std::vector<double>& nestedWeights)
    {
        json inner;
        inner["architecture"] = "WaveNet";
        inner["config"] = { { "kernel_sizes", { 6, 6, 15 } }, { "dilations", { 1, 3, 7 } } };
        inner["weights"] = nestedWeights;

        json j;
        j["version"] = "0.7.0";
        j["architecture"] = "SlimmableContainer";
        j["sample_rate"] = 48000;
        j["metadata"] = { { "loudness", -20.559617728394866 }, { "gain", 0.0026303589869 },
                          { "validation_esr", nullptr }, { "notes", juce::CharPointer_UTF8 ("café — ☃ \"q\" \\ \n\t").getAddress() } };
        j["config"]["submodels"] = json::array();
        j["config"]["submodels"].push_back ({ { "max_value", 0.5 }, { "model", inner } });
        j["weights"] = topWeights;
        return j;
    }

    json defaultNam() { return makeNam ({ -0.5, 0.5, 1.36406, -1.0e-6, 0.0, 0.333333333333 }, { 0.1, -0.25, 12.5, -7.0, 2.0e-7 }); }

    // A .nam with a SINGLE weights array (so weightsOf(...)[0] is that array — nlohmann sorts keys, so a
    // nested "config" would otherwise sort before top-level "weights"). Used for indexed spot-checks.
    json makeFlatNam (const std::vector<double>& w) { json j; j["architecture"] = "WaveNet"; j["sample_rate"] = 48000; j["weights"] = w; return j; }

    bool weightsBitExact (const json& a, const json& b)
    {
        const auto wa = weightsOf (a), wb = weightsOf (b);
        if (wa.size() != wb.size()) return false;
        for (size_t i = 0; i < wa.size(); ++i)
        {
            if (wa[i].size() != wb[i].size()) return false;
            for (size_t k = 0; k < wa[i].size(); ++k)
                if (! juce::exactlyEqual (wa[i][k], wb[i][k])) return false;
        }
        return true;
    }

    juce::MemoryBlock gzip (const juce::String& s)   // simulate a v5/v6 pool entry (deflate raw .nam)
    {
        juce::MemoryBlock out;
        { juce::MemoryOutputStream mos (out, false); juce::GZIPCompressorOutputStream gz (mos, 9); gz.write (s.toRawUTF8(), s.getNumBytesAsUTF8()); }
        return out;
    }
    juce::MemoryBlock gunzip (const juce::MemoryBlock& mb)
    {
        juce::MemoryInputStream in (mb.getData(), mb.getSize(), false);
        juce::GZIPDecompressorInputStream gz (in);
        juce::MemoryBlock out; char buf[16384];
        for (;;) { const int n = gz.read (buf, sizeof (buf)); if (n <= 0) break; out.append (buf, (size_t) n); }
        return out;
    }
}

struct NamCodecTest : juce::UnitTest
{
    NamCodecTest() : juce::UnitTest ("NamCodec") {}

    // pack → unpack → parse; assert weights bit-exact + metadata preserved. Returns the reconstructed json.
    json roundTrip (const json& nam, bool shuffle)
    {
        const auto s = nam.dump();
        auto packed = namz::pack (s.data(), s.size(), { shuffle });
        expect (packed.getSize() > 0, "pack ok");
        expect (namz::isNamz (packed.getData(), packed.getSize()), "packed has magic");
        auto back = parseBlock (namz::unpack (packed.getData(), packed.getSize(), kCap));
        expect (! back.is_null(), "unpack+parse ok");
        expect (weightsBitExact (nam, back), "weights bit-exact float32");
        return back;
    }

    void runTest() override
    {
        //====================================================================== round-trip / numeric
        beginTest ("round-trip: metadata (null, unicode, big double, escapes) preserved both shuffle modes");
        for (bool sh : { true, false })
        {
            const auto nam = defaultNam();
            auto back = roundTrip (nam, sh);
            expect (back["metadata"]["validation_esr"].is_null(), "null preserved");
            expectEquals (back["metadata"]["notes"].get<std::string>(), nam["metadata"]["notes"].get<std::string>());
            expect (juce::exactlyEqual (back["metadata"]["loudness"].get<double>(), nam["metadata"]["loudness"].get<double>()), "big double preserved");
            expect (back["config"]["submodels"][0]["model"]["config"]["dilations"]
                        == nam["config"]["submodels"][0]["model"]["config"]["dilations"], "non-weight int array untouched");
        }

        beginTest ("numeric edges round-trip bit-exact: -0, subnormal, FLT_MAX/MIN, precision, range");
        {
            const float fm = std::numeric_limits<float>::max();          // 3.4028235e38
            const float fmin = std::numeric_limits<float>::min();        // smallest normal
            const float sub = std::numeric_limits<float>::denorm_min();  // smallest subnormal
            std::vector<double> W = { 0.0, -0.0, (double) sub, (double) fmin, (double) fm, -(double) fm,
                                      0.1, 1.0 / 3.0, -1.36406, 1.0e-7, 123456.789, (double) std::nextafter (0.5f, 1.0f) };
            auto nam = makeFlatNam (W);                 // single weights array → weightsOf(...)[0] is W
            auto back = roundTrip (nam, true);
            // spot-check the signed zero + subnormal + max explicitly (the classic round-trip losers)
            const auto w = weightsOf (back)[0];
            expect (std::signbit (w[1]), "-0.0 keeps its sign bit");
            expect (juce::exactlyEqual (w[2], sub), "smallest subnormal survives");
            expect (juce::exactlyEqual (w[4], fm),  "FLT_MAX survives");
        }

        beginTest ("determinism: pack(x)==pack(x) byte-identical; idempotent pack(unpack(pack(x)))==pack(x)");
        {
            const auto s = defaultNam().dump();
            auto a = namz::pack (s.data(), s.size(), {});
            auto b = namz::pack (s.data(), s.size(), {});
            expect (a.matches (b.getData(), b.getSize()), "two packs identical");
            auto rebuilt = namz::unpack (a.getData(), a.getSize(), kCap);
            auto c = namz::pack (rebuilt.getData(), rebuilt.getSize(), {});
            expect (a.matches (c.getData(), c.getSize()), "pack∘unpack∘pack is a fixed point");
        }

        beginTest ("shuffle on vs off: identical float32 weights, both smaller than raw");
        {
            const auto s = defaultNam().dump();
            auto on  = namz::pack (s.data(), s.size(), { true });
            auto off = namz::pack (s.data(), s.size(), { false });
            expect (weightsBitExact (parseBlock (namz::unpack (on.getData(),  on.getSize(),  kCap)),
                                     parseBlock (namz::unpack (off.getData(), off.getSize(), kCap))), "same weights");
            expect (on.getSize() < s.size() && off.getSize() < s.size(), "both smaller than raw JSON");
        }

        //====================================================================== structural edges
        beginTest ("structural: empty weights [], no weights at all, non-numeric \"weights\" left alone");
        {
            expect (! roundTrip (makeNam ({}, {}), true).is_null(), "empty weight arrays round-trip");

            json noW; noW["architecture"] = "LSTM"; noW["metadata"] = { { "loudness", -18.0 } }; noW["sample_rate"] = 44100;
            auto s = noW.dump(); auto p = namz::pack (s.data(), s.size(), {});
            expect (p.getSize() > 0 && parseBlock (namz::unpack (p.getData(), p.getSize(), kCap)) == noW, "a .nam with NO weights round-trips verbatim");

            json odd; odd["weights"] = "not-an-array"; odd["also"] = json::object({ { "weights", json::object() } });
            s = odd.dump(); p = namz::pack (s.data(), s.size(), {});
            expect (p.getSize() > 0 && parseBlock (namz::unpack (p.getData(), p.getSize(), kCap)) == odd, "non-numeric \"weights\" values are not extracted, survive verbatim");
        }

        beginTest ("structural: deep nesting, many arrays, large array, empty object, non-object top-level");
        {
            // many small nested weights arrays
            json deep; deep["a"]["b"]["c"]["weights"] = std::vector<double> { 1, 2, 3 };
            deep["a"]["b"]["weights"] = std::vector<double> { 4, 5 };
            deep["list"] = json::array(); for (int i = 0; i < 20; ++i) deep["list"].push_back ({ { "weights", std::vector<double> { (double) i, -(double) i } } });
            expect (! roundTrip (deep, true).is_null(), "deep + many weights arrays round-trip");

            // large single array
            std::vector<double> big (60000); for (size_t i = 0; i < big.size(); ++i) big[i] = std::sin ((double) i) * 1.3;
            expect (! roundTrip (makeNam (big, {}), true).is_null(), "60k-weight array round-trips");

            // degenerate top-level JSON — must not crash, must round-trip
            for (const char* lit : { "{}", "[]", "[1,2,3]", "\"hi\"", "42", "null", "true" })
            {
                auto p = namz::pack (lit, std::strlen (lit), {});
                expect (p.getSize() > 0, juce::String ("packs ") + lit);
                expect (parseBlock (namz::unpack (p.getData(), p.getSize(), kCap)) == json::parse (lit), juce::String ("round-trips ") + lit);
            }
        }

        //====================================================================== container robustness
        beginTest ("isNamz: true only on magic; false on JSON/gzip/random/short/null");
        {
            const char* jsonB = "{\"weights\":[]}";
            expect (! namz::isNamz (jsonB, std::strlen (jsonB)), "raw JSON not namz");
            const unsigned char gz[] = { 0x1f, 0x8b, 0x08, 0x00 };  expect (! namz::isNamz (gz, 4), "gzip not namz");
            const unsigned char rnd[] = { 0x00, 0xff, 0x4e, 0x41 }; expect (! namz::isNamz (rnd, 4), "random not namz");
            expect (! namz::isNamz ("NAM", 3), "3 bytes not namz");
            expect (! namz::isNamz (nullptr, 0), "null not namz");
            const char* nmz = "NAMZ...."; expect (namz::isNamz (nmz, 8), "magic is namz");
        }

        beginTest ("corruption: flipped header bytes + unknown version/codec/dtype rejected (empty, no crash)");
        {
            const auto s = defaultNam().dump();
            auto good = namz::pack (s.data(), s.size(), {});
            auto bad = [&] (int idx, juce::uint8 v) { juce::MemoryBlock m (good); ((juce::uint8*) m.getData())[idx] = v; return namz::unpack (m.getData(), m.getSize(), kCap).getSize(); };
            expect (bad (0, 'X') == 0, "flipped magic rejected");
            expect (bad (4, 3) == 0,   "formatVersion 3 (>2) rejected");
            expect (bad (4, 255) == 0, "formatVersion 255 rejected");
            expect (bad (5, 1) == 0,   "codec 1 rejected");
            expect (bad (6, 1) == 0,   "dtype 1 (f16) rejected");
            expect (namz::unpack (good.getData(), good.getSize(), kCap).getSize() > 0, "the untouched control still unpacks");
        }

        beginTest ("corruption: truncation at every byte never crashes and returns empty");
        {
            const auto tiny = makeFlatNam ({ 0.5, -0.5, 0.25 }).dump();   // small blob → cheap to fuzz every prefix
            auto good = namz::pack (tiny.data(), tiny.size(), { true });
            // Contract: NEVER crash/garbage. A prefix is either rejected (empty) OR — if it lost only the
            // trailing gzip checksum — decodes to the EXACT original. It must never yield partial/corrupt data.
            bool safe = true;
            for (size_t len = 0; len < good.getSize(); ++len)
            {
                const auto r = namz::unpack (good.getData(), len, kCap);
                safe &= (r.getSize() == 0 || weightsBitExact (parseBlock (r), makeFlatNam ({ 0.5, -0.5, 0.25 })));
            }
            expect (safe, "every truncated prefix → empty or the exact original (never partial/garbage/crash)");
            // trailing garbage after a valid blob: decodes the valid stream, ignores the junk — never corrupt.
            juce::MemoryBlock plus (good); plus.append ("junkjunk", 8);
            const auto r = namz::unpack (plus.getData(), plus.getSize(), kCap);
            expect (r.getSize() == 0 || weightsBitExact (parseBlock (r), makeFlatNam ({ 0.5, -0.5, 0.25 })), "trailing junk never corrupts");
        }

        beginTest ("corruption: metaLen that lies (larger than buffer) is rejected");
        {
            const auto s = defaultNam().dump();
            auto good = namz::pack (s.data(), s.size(), { true });   // v2 → has metaLen at bytes 8..9
            juce::MemoryBlock m (good);
            ((juce::uint8*) m.getData())[8] = 0xff; ((juce::uint8*) m.getData())[9] = 0xff;   // metaLen = 65535
            expect (namz::unpack (m.getData(), m.getSize(), kCap).getSize() == 0, "metaLen > buffer rejected");
        }

        beginTest ("zip-bomb guard: reconstruction over maxJsonBytes is rejected");
        {
            const auto s = defaultNam().dump();
            auto good = namz::pack (s.data(), s.size(), {});
            expect (namz::unpack (good.getData(), good.getSize(), 8).getSize() == 0, "tiny cap rejects");
            expect (namz::unpack (good.getData(), good.getSize(), kCap).getSize() > 0, "generous cap accepts");
        }

        //====================================================================== metadata / readMeta
        beginTest ("v2 metadata: typed --set, header readable without inflate, skeleton mirrors it");
        {
            const auto s = defaultNam().dump();
            namz::PackOptions o;
            o.metadata.set ("tone_type", "hi-gain"); o.metadata.set ("boost", "true");
            o.metadata.set ("stages", "16"); o.metadata.set ("modeled_by", "Darwin's Cat");
            o.metadata.set ("device", "tube:1,pnp:1");
            auto p = namz::pack (s.data(), s.size(), o);
            auto m = namz::readMeta (p.getData(), p.getSize());
            expectEquals (m["tone_type"], juce::String ("hi-gain"));
            expectEquals (m["boost"], juce::String ("true"));    // bool → text
            expectEquals (m["stages"], juce::String ("16"));     // number → text
            expectEquals (m["device"], juce::String ("tube:1,pnp:1"));
            auto j = parseBlock (namz::unpack (p.getData(), p.getSize(), kCap));
            expect (j["metadata"]["boost"].is_boolean() && j["metadata"]["boost"].get<bool>(), "boost typed bool in model metadata");
            expectEquals (j["metadata"]["stages"].get<int>(), 16);
            expect (namz::readMeta (s.data(), s.size()).size() == 0, "readMeta on raw JSON = empty");
        }

        beginTest ("metadata: special chars / a field literally named \"weights\" survive; empty --set = no header");
        {
            json nam = defaultNam();
            nam["metadata"]["weird=key,with:chars"] = "v=a,b:c\n\"q\"";
            nam["metadata"]["weights"] = "v1.2.3";   // a metadata STRING called weights must NOT be packed as floats
            auto back = roundTrip (nam, true);
            expectEquals (back["metadata"]["weights"].get<std::string>(), std::string ("v1.2.3"));
            expectEquals (back["metadata"]["weird=key,with:chars"].get<std::string>(), std::string ("v=a,b:c\n\"q\""));

            auto s = defaultNam().dump();
            auto plain = namz::pack (s.data(), s.size(), {});
            expect (namz::readMeta (plain.getData(), plain.getSize()).size() == 0, "no --set → empty header meta");
        }

        beginTest ("metadata: >64KB header is dropped (not overflowed); the model still round-trips");
        {
            const auto s = defaultNam().dump();
            namz::PackOptions o;
            o.metadata.set ("blob", juce::String::repeatedString ("x", 70000));   // > u16
            auto p = namz::pack (s.data(), s.size(), o);
            expect (p.getSize() > 0, "packs despite huge metadata");
            expect (namz::readMeta (p.getData(), p.getSize()).size() == 0, "oversized header meta dropped, not corrupt");
            auto j = parseBlock (namz::unpack (p.getData(), p.getSize(), kCap));
            expectEquals (j["metadata"]["blob"].get<std::string>().size(), (size_t) 70000);   // still in the skeleton
        }

        //====================================================================== v5/v6 → v7 pool path
        beginTest ("pool upgrade: gzip(raw .nam) is NOT namz; inflate→pack→unpack preserves weights");
        {
            const auto raw = defaultNam().dump();
            auto gz = gzip (raw);
            expect (! namz::isNamz (gz.getData(), gz.getSize()), "a v5/v6 gzip blob is not mistaken for .namz");
            auto inflated = gunzip (gz);                                   // what the restore path does for old states
            auto packed = namz::pack (inflated.getData(), inflated.getSize(), {});   // → toNamz upgrade
            expect (namz::isNamz (packed.getData(), packed.getSize()), "upgraded to .namz");
            expect (weightsBitExact (defaultNam(), parseBlock (namz::unpack (packed.getData(), packed.getSize(), kCap))), "weights survive the v6→v7 upgrade");
        }

        //====================================================================== device spec (DeviceSpec.h)
        beginTest ("deviceFromString: aliases + unknown");
        {
            expect (deviceFromString ("tube") == DeviceType::tube && deviceFromString ("valve") == DeviceType::tube, "tube");
            expect (deviceFromString ("pnp") == DeviceType::pnp && deviceFromString ("NPN") == DeviceType::pnp
                    && deviceFromString ("bjt") == DeviceType::pnp && deviceFromString ("transistor") == DeviceType::pnp, "bjt family");
            expect (deviceFromString ("fet") == DeviceType::fet && deviceFromString ("jfet") == DeviceType::fet && deviceFromString ("mosfet") == DeviceType::fet, "fet");
            expect (deviceFromString ("dsp") == DeviceType::dsp && deviceFromString ("chip") == DeviceType::dsp && deviceFromString ("ic") == DeviceType::dsp, "dsp");
            expect (deviceFromString ("diode") == DeviceType::diode, "diode");
            expect (deviceFromString ("") == DeviceType::none && deviceFromString ("bogus") == DeviceType::none, "unknown → none");
        }

        beginTest ("parseDeviceSpec: hybrids, counts, and malformed specs drop gracefully");
        {
            auto eq = [] (const DeviceSpec& s, const DeviceSpec& e) { return s == e; };
            expect (eq (parseDeviceSpec ("tube:1,pnp:1"), { { DeviceType::tube, 1 }, { DeviceType::pnp, 1 } }), "hybrid");
            expect (eq (parseDeviceSpec ("tube:4"), { { DeviceType::tube, 4 } }), "count");
            expect (eq (parseDeviceSpec ("tube"), { { DeviceType::tube, 1 } }), "bare type → count 1");
            expect (eq (parseDeviceSpec (" tube:1 , pnp:2 "), { { DeviceType::tube, 1 }, { DeviceType::pnp, 2 } }), "whitespace tolerated");
            expect (parseDeviceSpec ("").empty(), "empty spec");
            expect (eq (parseDeviceSpec ("tube:1,"), { { DeviceType::tube, 1 } }), "trailing comma");
            expect (parseDeviceSpec (":1").empty() && parseDeviceSpec ("bogus:2").empty(), "no/unknown type dropped");
            expect (parseDeviceSpec ("tube:0").empty() && parseDeviceSpec ("tube:-3").empty() && parseDeviceSpec ("tube:x").empty(), "non-positive / non-numeric count dropped");
            expect (eq (parseDeviceSpec ("tube:99"), { { DeviceType::tube, 12 } }), "count clamped to 12");
            expectEquals (deviceSpecCount (parseDeviceSpec ("tube:4,pnp:1")), 5);
            expectEquals (deviceSpecCount (parseDeviceSpec ("tube:99,pnp:99")), 12);   // clamped
            expectEquals (deviceSpecCount (parseDeviceSpec ("")), 0);
        }
    }
};

static NamCodecTest namCodecTest;
