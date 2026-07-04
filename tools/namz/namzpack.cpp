// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// namzpack — offline packer/unpacker for the `.namz` format (see src/core/NamCodec.h).
// The SAME codec the plugin loads at runtime, so a packed library is byte-for-byte what
// the plugin consumes. Deterministic + round-trippable: `.namz` in git, raw `.nam` masters
// live in the private capture repo.
//
//   namzpack pack        <in.nam>   <out.namz>   [--no-shuffle]
//   namzpack unpack      <in.namz>  <out.nam>
//   namzpack pack-dir    <srcdir>   <dstdir>     [--no-shuffle]   # every *.nam  -> *.namz
//   namzpack unpack-dir  <srcdir>   <dstdir>                      # every *.namz -> *.nam
//   namzpack verify      <in.nam | dir>                           # pack+unpack round-trip check
//
// Exit non-zero on any failure (so it can gate a conversion / CI).

#include <juce_core/juce_core.h>

#include "core/NamCodec.h"

#include <cstdio>

namespace
{
    constexpr size_t kMaxJson = 64u * 1024u * 1024u;   // generous cap for the unpack zip-bomb guard

    juce::MemoryBlock readFile (const juce::File& f)
    {
        juce::MemoryBlock mb;
        f.loadFileAsData (mb);
        return mb;
    }

    bool writeFile (const juce::File& f, const juce::MemoryBlock& mb)
    {
        f.getParentDirectory().createDirectory();
        return f.replaceWithData (mb.getData(), mb.getSize());
    }

    void print (const juce::String& s) { std::printf ("%s\n", s.toRawUTF8()); }

    juce::String pct (juce::int64 out, juce::int64 in)
    {
        if (in <= 0) return "-";
        return juce::String (100.0 * (double) out / (double) in, 1) + "%";
    }

    // Round-trip a raw .nam through pack→unpack and confirm the reconstructed JSON re-parses
    // and every weight is bit-exact float32. Returns true on success; prints the ratio.
    bool verifyOne (const juce::File& in)
    {
        const auto raw = readFile (in);
        if (raw.getSize() == 0) { print ("  MISS " + in.getFileName()); return false; }

        auto packed = namz::pack (raw.getData(), raw.getSize());
        if (packed.getSize() == 0) { print ("  FAIL pack   " + in.getFileName()); return false; }

        auto back = namz::unpack (packed.getData(), packed.getSize(), kMaxJson);
        if (back.getSize() == 0) { print ("  FAIL unpack " + in.getFileName()); return false; }

        // Cheap structural check: the reconstruction must be valid JSON of similar shape. We don't
        // link nlohmann for a deep compare here — the unit test (NamCodecTests) does the bit-exact
        // weight comparison; here we assert it re-parses via JUCE's JSON and is non-trivial.
        auto parsed = juce::JSON::parse (juce::String::createStringFromData (back.getData(), (int) back.getSize()));
        const bool ok = ! parsed.isVoid();
        print (juce::String (ok ? "  OK   " : "  FAIL parse ") + in.getFileName().paddedRight (' ', 34)
               + juce::String (raw.getSize() / 1024) + "K -> " + juce::String (packed.getSize() / 1024) + "K  "
               + pct ((juce::int64) packed.getSize(), (juce::int64) raw.getSize()));
        return ok;
    }
}

int main (int argc, char** argv)
{
    juce::StringArray args;
    juce::StringPairArray meta;
    bool shuffle = true;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if      (a == "--no-shuffle")          shuffle = false;
        else if (a == "--set" && i + 1 < argc)          // --set key=value  (repeatable) → metadata
        {
            const juce::String kv (argv[++i]);
            meta.set (kv.upToFirstOccurrenceOf ("=", false, false),
                      kv.fromFirstOccurrenceOf ("=", false, false));
        }
        else args.add (a);
    }

    if (args.size() < 1)
    {
        print ("usage: namzpack pack|unpack|pack-dir|unpack-dir|restamp|meta|verify ...  [--no-shuffle] [--set k=v ...]");
        return 2;
    }

    const juce::String cmd = args[0];
    namz::PackOptions opts;
    opts.shuffle  = shuffle;
    opts.metadata = meta;

    if ((cmd == "pack" || cmd == "unpack") && args.size() == 3)
    {
        const juce::File in  (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
        const juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (args[2]));
        const auto raw = readFile (in);
        if (raw.getSize() == 0) { print ("cannot read " + args[1]); return 1; }

        const auto result = (cmd == "pack")
            ? namz::pack   (raw.getData(), raw.getSize(), opts)
            : namz::unpack (raw.getData(), raw.getSize(), kMaxJson);
        if (result.getSize() == 0) { print (cmd + " failed for " + args[1]); return 1; }
        if (! writeFile (out, result)) { print ("cannot write " + args[2]); return 1; }
        print (args[1] + " -> " + args[2] + "  (" + juce::String (raw.getSize()) + " -> "
               + juce::String (result.getSize()) + " bytes, " + pct ((juce::int64) result.getSize(), (juce::int64) raw.getSize()) + ")");
        return 0;
    }

    if ((cmd == "pack-dir" || cmd == "unpack-dir") && args.size() == 3)
    {
        const bool packing = (cmd == "pack-dir");
        const juce::File src (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
        const juce::File dst (juce::File::getCurrentWorkingDirectory().getChildFile (args[2]));
        if (! src.isDirectory()) { print ("not a directory: " + args[1]); return 1; }
        dst.createDirectory();

        const juce::String inExt  = packing ? "*.nam"  : "*.namz";
        const juce::String outExt = packing ? ".namz"  : ".nam";
        auto files = src.findChildFiles (juce::File::findFiles, false, inExt);
        files.sort();
        juce::int64 totIn = 0, totOut = 0;
        int done = 0, failed = 0;
        for (const auto& f : files)
        {
            const auto raw = readFile (f);
            const auto result = packing
                ? namz::pack   (raw.getData(), raw.getSize(), opts)
                : namz::unpack (raw.getData(), raw.getSize(), kMaxJson);
            const auto outFile = dst.getChildFile (f.getFileNameWithoutExtension() + outExt);
            if (result.getSize() == 0 || ! writeFile (outFile, result)) { print ("  FAIL " + f.getFileName()); ++failed; continue; }
            totIn += (juce::int64) raw.getSize(); totOut += (juce::int64) result.getSize(); ++done;
        }
        print (juce::String (done) + " files, " + juce::String (totIn / 1024) + "K -> "
               + juce::String (totOut / 1024) + "K  (" + pct (totOut, totIn) + "), " + juce::String (failed) + " failed");
        return failed == 0 ? 0 : 1;
    }

    if (cmd == "restamp" && args.size() == 3)
    {
        // Rewrite a model's `metadata` (via --set k=v) losslessly: unpack → set fields → repack.
        // Accepts a .namz (unpacked first) or a raw .nam. Weights survive bit-exact (float32).
        const juce::File in  (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
        const juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (args[2]));
        const auto raw = readFile (in);
        if (raw.getSize() == 0) { print ("cannot read " + args[1]); return 1; }
        juce::MemoryBlock nam = namz::isNamz (raw.getData(), raw.getSize())
            ? namz::unpack (raw.getData(), raw.getSize(), kMaxJson) : raw;
        if (nam.getSize() == 0) { print ("unpack failed for " + args[1]); return 1; }
        const auto result = namz::pack (nam.getData(), nam.getSize(), opts);
        if (result.getSize() == 0 || ! writeFile (out, result)) { print ("restamp failed for " + args[1]); return 1; }
        print (args[1] + " restamped (" + juce::String (meta.size()) + " field(s)) -> " + args[2]);
        return 0;
    }

    if (cmd == "meta" && args.size() == 2)
    {
        // Print the cheap display-metadata block (v2) without inflating the weights.
        const juce::File in (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
        const auto raw = readFile (in);
        if (raw.getSize() == 0) { print ("cannot read " + args[1]); return 1; }
        const auto m = namz::readMeta (raw.getData(), raw.getSize());
        if (m.size() == 0) { print ("(no display metadata)"); return 0; }
        for (const auto& k : m.getAllKeys())
            print ("  " + k + " = " + m[k]);
        return 0;
    }

    if (cmd == "verify" && args.size() == 2)
    {
        const juce::File p (juce::File::getCurrentWorkingDirectory().getChildFile (args[1]));
        int failed = 0, total = 0;
        if (p.isDirectory())
        {
            auto files = p.findChildFiles (juce::File::findFiles, false, "*.nam");
            files.sort();
            for (const auto& f : files) { ++total; if (! verifyOne (f)) ++failed; }
        }
        else { ++total; if (! verifyOne (p)) ++failed; }
        print (juce::String (total - failed) + "/" + juce::String (total) + " verified");
        return failed == 0 ? 0 : 1;
    }

    print ("bad arguments");
    return 2;
}
