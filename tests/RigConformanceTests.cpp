// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// OrbitCab's READER duty of the .orbitrig contract (namz conformance/rig/README.md): load the
// GOLDEN pack — committed in the namz repo, fetched with the pinned dependency — into exactly the
// device expected.json describes, and resolve every row of its selection table. The capture tool
// runs the WRITER duty against the same fixtures in its own CI; any contract drift fails one of us
// at PR time. Reads the models' headers the same way the app does (ocnam::readMeta → PreampRig).
#include <juce_core/juce_core.h>

#include "PreampRig.h"
#include "core/NamCodec.h"

#ifndef ORBITCAB_NAMZ_CONFORMANCE_DIR
#error "ORBITCAB_NAMZ_CONFORMANCE_DIR must point at the fetched namz conformance/ directory"
#endif

struct RigConformanceTest : juce::UnitTest
{
    RigConformanceTest() : juce::UnitTest ("RigConformance") {}

    void runTest() override
    {
        const auto dir = juce::File (ORBITCAB_NAMZ_CONFORMANCE_DIR).getChildFile ("rig");

        beginTest ("golden fixtures present (pinned namz ships conformance/rig)");
        const auto expected = juce::JSON::parse (dir.getChildFile ("expected.json").loadFileAsString());
        expect (expected.isObject());
        if (! expected.isObject())
            return;

        // Build the library the app's way: each golden .namz becomes a user-folder source whose
        // metadata comes from its own header (no manifest — the player's scan path).
        std::vector<orbitcab::PreampSource> sources;
        const auto files = expected["stage"]["files"];
        if (auto* fo = files.getDynamicObject())
            for (const auto& prop : fo->getProperties())
            {
                const auto packName = prop.name.toString();
                orbitcab::PreampSource s;
                s.base = packName.upToLastOccurrenceOf (".", false, false);
                s.id   = "up:" + s.base;
                s.file = dir.getChildFile ("pack").getChildFile (packName);
                juce::MemoryBlock mb;
                expect (s.file.loadFileAsData (mb), "golden model readable");
                s.meta = ocnam::readMeta (mb.getData(), mb.getSize());
                sources.push_back (std::move (s));
            }

        orbitcab::PreampRig rig;
        rig.build (sources);

        beginTest ("the golden pack builds the expected device");
        expect (rig.devices.size() == 1);
        if (rig.devices.size() != 1)
            return;
        const auto& d = rig.devices.front();
        expect (juce::String (d.rigId.c_str())  == expected["rig"]["rig_id"].toString());
        expect (juce::String (d.family.c_str()) == expected["rig"]["name"].toString());
        expect (juce::String (d.slot.c_str())   == expected["stage"]["slot"].toString());
        expect (juce::String (namz::rig::buildControlsSpec (d.controls).c_str())
                    == expected["stage"]["controls"].toString(), "controls spec matches");
        expect ((int) d.files.size() == files.getDynamicObject()->getProperties().size());

        // Display entries: family name + a variant badge per file (metadata-driven, no filename help).
        for (const auto& e : rig.entries)
            expect (e.name == expected["rig"]["name"].toString() && e.variant.isNotEmpty());

        beginTest ("the selection table resolves as pinned");
        auto idFor = [] (const juce::String& packName)   // "Golden-green-07h.namz" → "up:Golden-green-07h"
        {
            return "up:" + packName.upToLastOccurrenceOf (".", false, false);
        };
        for (const auto& row : *expected["selection"].getArray())
        {
            const auto got = rig.resolveControl (idFor (row["from"].toString()),
                                                 row["turn"][0].toString(), row["turn"][1].toString());
            if (row["expect"].isVoid())
                expect (got.isEmpty(), "declared-but-uncaptured value selects nothing");
            else
                expect (got == idFor (row["expect"].toString()), "selection row resolves");
        }
    }
};

static RigConformanceTest rigConformanceTest;
