// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for orbitcab::readRigPack — the .orbitrig reader behind "Import rig…" /
// pack drag-drop: both shipping shapes (exchange zip + unpacked folder), zip-slip basename
// flattening, the per-model size cap, and manifest pickup. Pure (juce_core).
#include <juce_core/juce_core.h>

#include "RigPack.h"

using namespace orbitcab;

struct RigPackTest : juce::UnitTest
{
    RigPackTest() : juce::UnitTest ("RigPack") {}

    static juce::File tempDir (const char* tag)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile (juce::String (tag) + juce::String (juce::Time::currentTimeMillis()));
        dir.createDirectory();
        return dir;
    }

    void runTest() override
    {
        beginTest ("folder pack: loose .namz + rig.json, non-namz ignored, oversize counted");
        {
            auto dir = tempDir ("orbitcab_rigpack_dir_");
            dir.getChildFile ("A.namz").replaceWithText ("aaaa");
            dir.getChildFile ("B.namz").replaceWithText ("bbbbbbbbbbbbbbbb");   // 16 B — over the cap below
            dir.getChildFile ("rig.json").replaceWithText ("{\"format\":\"orbitrig\"}");
            dir.getChildFile ("readme.txt").replaceWithText ("ignore");

            const auto pack = readRigPack (dir, 8);
            expect (pack.models.size() == 1 && pack.failed == 1);
            if (pack.models.size() == 1)
                expect (pack.models.front().first == "A.namz" && pack.models.front().second.getSize() == 4);
            expect (pack.manifestText.contains ("orbitrig"));
            dir.deleteRecursively();
        }

        beginTest ("zip pack: nested entry paths flatten to basenames (zip-slip proof), manifest read");
        {
            auto dir = tempDir ("orbitcab_rigpack_zip_");
            const auto zipFile = dir.getChildFile ("Revolt.orbitrig.zip");
            {
                juce::ZipFile::Builder b;
                auto add = [&b] (const juce::String& path, const juce::String& text)
                {
                    b.addEntry (std::make_unique<juce::MemoryInputStream> (text.toRawUTF8(),
                                                                           (size_t) text.getNumBytesAsUTF8(), true),
                                5, path, juce::Time::getCurrentTime());
                };
                add ("ReVolt Guitar.orbitrig/ReVolt-green-07h.namz", "gggg");
                add ("../evil.namz", "evil");                        // path escape attempt → basename only
                add ("ReVolt Guitar.orbitrig/rig.json", "{\"format\":\"orbitrig\",\"name\":\"X\"}");
                add ("notes/readme.txt", "ignore");
                juce::FileOutputStream os (zipFile);
                expect (os.openedOk() && b.writeToStream (os, nullptr));
            }

            const auto pack = readRigPack (zipFile, 1 << 20);
            expect (pack.models.size() == 2 && pack.failed == 0);
            for (const auto& [name, bytes] : pack.models)
                expect (! name.containsChar ('/') && ! name.startsWith (".."));   // basenames only
            bool sawFlattened = false;
            for (const auto& [name, bytes] : pack.models)
                if (name == "ReVolt-green-07h.namz") sawFlattened = true;
            expect (sawFlattened);
            expect (pack.manifestText.contains ("\"name\":\"X\""));
            dir.deleteRecursively();
        }

        beginTest ("missing / empty pack → no models, no manifest");
        {
            const auto pack = readRigPack (juce::File(), 1 << 20);
            expect (pack.models.empty() && pack.manifestText.isEmpty());

            auto dir = tempDir ("orbitcab_rigpack_empty_");
            const auto empty = readRigPack (dir, 1 << 20);
            expect (empty.models.empty() && empty.failed == 0);
            dir.deleteRecursively();
        }
    }
};

static RigPackTest rigPackTest;
