// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for orbitcab::PreampRig — the metadata-first PREAMP device model over
// namz::rig (replacing the old PreampLibrary parse + PreampSelector policy; their scenarios are
// ported here against the SAME library shapes). Covers: the legacy filename-token fallback, the
// `controls`/`settings.*` metadata path (generic switches included), rig_id grouping, the ≥2
// visibility rule, keep-else-default resolution, colour-ranked channel presentation, sparse-matrix
// safety, the variant badges, and the user-folder scan. Pure (juce_core + namz_rig).
#include <juce_core/juce_core.h>

#include "PreampRig.h"

using namespace orbitcab;

namespace
{
    PreampSource src (const char* base)
    {
        PreampSource s;
        s.base = base;
        s.id   = "up:" + juce::String (base);
        return s;
    }

    // A meta-stamped capture the way OrbitNamCapture packs it: the whole device's `controls` spec +
    // THIS file's `settings.<control>` positions (+ identity keys).
    PreampSource metaSrc (const char* base, const char* controlsSpec,
                          std::initializer_list<std::pair<const char*, const char*>> settings,
                          const char* rigId = "dc-test-rig", const char* gearModel = "ReVolt Guitar")
    {
        auto s = src (base);
        s.meta.set ("controls", controlsSpec);
        if (rigId != nullptr)     s.meta.set ("rig_id", rigId);
        if (gearModel != nullptr) s.meta.set ("gear_model", gearModel);
        for (const auto& [k, v] : settings)
            s.meta.set ("settings." + juce::String (k), v);
        return s;
    }

    // The old PreampSelector's reference family, as legacy FILENAMES: "Voltage" across all three
    // dimensions + a "Studio Pre" singleton. Same shapes as PreampSelectorTests used to build.
    PreampRig makeLegacyRig()
    {
        PreampRig rig;
        rig.build ({ src ("Voltage ch1 12h"),
                     src ("Voltage ch2 12h"),
                     src ("Voltage ch2 12h boost"),
                     src ("Voltage ch2 16h"),
                     src ("Voltage ch3 12h"),
                     src ("Studio Pre") });
        return rig;
    }

    const namz::rig::Device* deviceNamed (const PreampRig& rig, const char* family)
    {
        for (const auto& d : rig.devices)
            if (d.family == family) return &d;
        return nullptr;
    }
}

struct PreampRigTest : juce::UnitTest
{
    PreampRigTest() : juce::UnitTest ("PreampRig") {}

    void runTest() override
    {
        beginTest ("legacy filenames: one family device + a singleton, controls from the ≥2 rule");
        {
            auto rig = makeLegacyRig();
            expect (rig.devices.size() == 2 && rig.entries.size() == 6);

            const auto* volt = deviceNamed (rig, "Voltage");
            expect (volt != nullptr && rig.isGroup (*volt));
            if (volt != nullptr)
            {
                // channel (3 values) + boost (both captures exist) + gain (12h/16h) — all ≥2.
                expect (volt->controls.size() == 3);
                juce::StringArray names;
                for (const auto& c : volt->controls) names.add (juce::String (c.name));
                expect (names.contains ("channel") && names.contains ("gain") && names.contains ("boost"));
            }
            const auto* pre = deviceNamed (rig, "Studio Pre");
            expect (pre != nullptr && ! rig.isGroup (*pre) && pre->controls.empty());
        }

        beginTest ("viewFor: a control shows only when it has ≥2 values; current = the file's position");
        {
            auto rig = makeLegacyRig();

            auto v1 = rig.viewFor ("up:Voltage ch1 12h");
            expect (v1.group && v1.controls.size() == 3);
            for (const auto& cv : v1.controls)
            {
                if (cv.name == "channel") expect (cv.visible && cv.current == "ch1" && cv.values.size() == 3);
                if (cv.name == "gain")    expect (cv.visible && cv.current == "12h");
                if (cv.name == "boost")   expect (cv.visible && cv.current == "off");
            }

            auto vs = rig.viewFor ("up:Studio Pre");
            expect (! vs.group && vs.controls.empty());
            auto vNone = rig.viewFor ("nope");
            expect (! vNone.group && vNone.deviceIndex < 0);
        }

        beginTest ("resolveControl: pin the turned control, keep the rest, else fall back (ported)");
        {
            auto rig = makeLegacyRig();
            // ch2/12h/boost → ch1: ch1 keeps 12h (exists) but has no boost → falls back to clean.
            expect (rig.resolveControl ("up:Voltage ch2 12h boost", "channel", "ch1") == "up:Voltage ch1 12h");
            // ch2/12h/boost → gain 16h: ch2/16h has no boost → clean.
            expect (rig.resolveControl ("up:Voltage ch2 12h boost", "gain", "16h") == "up:Voltage ch2 16h");
            // ch2/12h/clean → boost on: the boosted twin exists.
            expect (rig.resolveControl ("up:Voltage ch2 12h", "boost", "on") == "up:Voltage ch2 12h boost");
            // resolving against no current selection is a safe no-op.
            expect (rig.resolveControl ("", "channel", "ch2").isEmpty());
        }

        beginTest ("resolveDevice: keep matching control values, default the rest (noon gain, clean)");
        {
            auto rig = makeLegacyRig();
            const int volt = rig.deviceIndexForId ("up:Voltage ch1 12h");
            // from nothing selected → defaults: first channel, noon gain, clean.
            expect (rig.resolveDevice ("", volt) == "up:Voltage ch1 12h");
            // currently on ch2/16h/clean → keep ch2 + 16h under Voltage (they exist).
            expect (rig.resolveDevice ("up:Voltage ch2 16h", volt) == "up:Voltage ch2 16h");
            // bad index → "".
            expect (rig.resolveDevice ("", 99).isEmpty());
        }

        beginTest ("channel presentation: colour values rank green → orange → blue → red, not scan order");
        {
            PreampRig rig;   // alphabetical scan order feeds blue first — presentation must not.
            rig.build ({ src ("GtrVolt-blue-12h"),
                         src ("GtrVolt-green-12h"),
                         src ("GtrVolt-orange-12h"),
                         src ("GtrVolt-green-16h") });
            auto v = rig.viewFor ("up:GtrVolt-green-12h");
            for (const auto& cv : v.controls)
                if (cv.name == "channel")
                {
                    expect (cv.values.size() == 3);
                    expect (cv.values[0] == "green" && cv.values[1] == "orange" && cv.values[2] == "blue");
                }
            // …and the device-switch default channel follows that rank (green, NOT alphabetical blue).
            const int di = rig.deviceIndexForId ("up:GtrVolt-green-12h");
            expect (rig.resolveDevice ("", di) == "up:GtrVolt-green-12h");
        }

        beginTest ("channel value labels + colours (the old parse cases, value-level now)");
        {
            expect (channelValueLabel ("green") == "Green" && channelValueColour ("green") == 0xff57c06a);
            expect (channelValueLabel ("RED")   == "Red"   && channelValueColour ("RED")   == 0xffe0524e);
            expect (channelValueLabel ("ch2")   == "Ch 2"  && channelValueColour ("ch2")   == 0);
            expect (channelValueLabel ("+")     == "+"     && channelValueColour ("+")     == 0);
        }

        beginTest ("legacy grammar edges: whole-word tokens only; ch5/Boosted/Channel stay in the name");
        {
            PreampRig rig;
            rig.build ({ src ("ch5 amp"), src ("Channel Two"), src ("Boosted"), src ("Reactor X1") });
            expect (rig.devices.size() == 4);   // none of these parse as control tokens → 4 singletons
            for (const auto& e : rig.entries)
                expect (e.variant.isEmpty());   // and none grew a bogus position badge
        }

        beginTest ("metadata path: opaque filenames, controls spec verbatim, generic switch works");
        {
            // Two channels × two modes, filenames deliberately meaningless — everything from meta.
            const char* spec = "channel:channel=green|red; mode:generic=classic|modern";
            PreampRig rig;
            rig.build ({ metaSrc ("take-001", spec, { { "channel", "green" }, { "mode", "classic" } }),
                         metaSrc ("take-002", spec, { { "channel", "green" }, { "mode", "modern" } }),
                         metaSrc ("take-003", spec, { { "channel", "red" },   { "mode", "classic" } }),
                         metaSrc ("take-004", spec, { { "channel", "red" },   { "mode", "modern" } }) });
            expect (rig.devices.size() == 1);
            if (rig.devices.size() == 1)
            {
                const auto& d = rig.devices.front();
                expect (d.rigId == "dc-test-rig" && d.family == "ReVolt Guitar");
                expect (d.controls.size() == 2 && d.controls[1].role == namz::rig::Role::Generic);
            }
            // display entries carry the family + the position badge
            const auto* e = rig.entryById ("up:take-001");
            expect (e != nullptr && e->name == "ReVolt Guitar" && e->variant == juce::String::fromUTF8 ("Green \xc2\xb7 classic"));
            // turning the GENERIC switch — the whole point of the migration
            expect (rig.resolveControl ("up:take-001", "mode", "modern") == "up:take-002");
            expect (rig.resolveControl ("up:take-002", "channel", "red") == "up:take-004");   // keeps modern
        }

        beginTest ("rig_id grouping: a file missing rig_id merges by gear_model; display renames survive");
        {
            const char* spec = "gain:gain=07h|12h";
            PreampRig rig;
            rig.build ({ metaSrc ("A-07", spec, { { "gain", "07h" } }),
                         metaSrc ("B-12-renamed", spec, { { "gain", "12h" } }, nullptr /*no rig_id*/) });
            expect (rig.devices.size() == 1 && rig.devices.front().files.size() == 2);
        }

        beginTest ("sparse matrix: a value no file carries resolves to \"\" — never a contradicting file");
        {
            const char* spec = "channel:channel=green|red; gain:gain=07h|12h|17h";   // 17h declared, never captured
            PreampRig rig;
            rig.build ({ metaSrc ("t1", spec, { { "channel", "green" }, { "gain", "07h" } }),
                         metaSrc ("t2", spec, { { "channel", "green" }, { "gain", "12h" } }),
                         metaSrc ("t3", spec, { { "channel", "red" },   { "gain", "12h" } }) });
            expect (rig.resolveControl ("up:t1", "gain", "17h").isEmpty());
            // …but the declared value still SHOWS on the dial (the spec is the author's dial).
            auto v = rig.viewFor ("up:t1");
            for (const auto& cv : v.controls)
                if (cv.name == "gain") expect (cv.values.size() == 3 && cv.visible);
            // resolveDevice never strands the user on a device with files (sparse desired → first file).
            expect (rig.resolveDevice ("", rig.deviceIndexForId ("up:t1")).isNotEmpty());
        }

        beginTest ("legacy variant badges mirror the old descriptor (Green · 12h · boost)");
        {
            PreampRig rig;
            rig.build ({ src ("GtrVolt-green-12h-boost"), src ("GtrVolt-green-12h"), src ("GtrVolt-red-07h") });
            const auto* e = rig.entryById ("up:GtrVolt-green-12h-boost");
            expect (e != nullptr && e->name == "GtrVolt");
            expect (e != nullptr && e->variant == juce::String::fromUTF8 ("Green \xc2\xb7 boost \xc2\xb7 12h"));
        }

        beginTest ("scanPreampLibrary: *.nam + *.namz enumerated, sorted, id = up:<base>, meta empty");
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("orbitcab_pre_rig_" + juce::String (juce::Time::currentTimeMillis()));
            dir.createDirectory();
            dir.getChildFile ("Voltage ch2 12h boost.nam").replaceWithText ("{}");
            dir.getChildFile ("Studio Pre 9h.namz").replaceWithText ("x");        // enumerated, content unread
            dir.getChildFile ("notes.txt").replaceWithText ("ignore me");         // non-capture is ignored

            auto srcs = scanPreampLibrary (dir);
            expect (srcs.size() == 2);
            if (srcs.size() == 2)
            {
                expect (srcs[0].base == "Studio Pre 9h" && srcs[0].id == "up:Studio Pre 9h");
                expect (srcs[1].base == "Voltage ch2 12h boost" && ! srcs[1].factory
                        && srcs[1].file.existsAsFile() && srcs[1].meta.size() == 0);
            }
            expect (scanPreampLibrary (dir.getChildFile ("nope")).empty());
            dir.deleteRecursively();
        }
    }
};

static PreampRigTest preampRigTest;
