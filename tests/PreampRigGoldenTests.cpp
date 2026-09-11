// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// PreampRig GOLDEN — every answer the preamp device model gives, over the SHIPPED factory library and a set of
// hand-built libraries, rendered as one canonical text and compared byte-for-byte with tests/golden/preamp-rig.txt.
//
// Why a rendering and not more assertions: the policy behind PreampRig used to live in namz, and namz changed it
// under this plugin — v2.0.0 dropped the filename grammar every factory capture is named in, v4 moved the gain
// default from the clock to degrees — while the targeted tests were silent about half of it (their gains happened
// to put noon first). A rendering of ALL answers cannot be half-silent: a lost control, a re-ordered value, a
// moved default or a different fallback file each change a line.
//
// The reference was rendered by `main` @ e3f02a7 against felitronics-core v0.13.1 (namz v1.1.1 — the last namz
// that carried the grammar): the model must answer exactly as the shipped plugin did, whichever core the tree
// builds against. ORBITCAB_GOLDEN_OUT=<file> writes the rendering there, for diffing or for a DELIBERATE
// regeneration — never as a side effect of a failing run.
#include <juce_core/juce_core.h>

#include "PreampRig.h"
#include "core/NamCodec.h"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <utility>
#include <vector>

using namespace orbitcab;

namespace
{
    // A file with no metadata — the legacy grammar's input. `prefix` "fp:" marks it factory.
    PreampSource bare (const char* base, const char* prefix = "up:")
    {
        PreampSource s;
        s.base    = juce::String::fromUTF8 (base);
        s.id      = juce::String (prefix) + s.base;
        s.factory = juce::String (prefix) == "fp:";
        return s;
    }

    // A meta-stamped capture: the device's `controls` spec + this file's `settings.*` (+ identity keys).
    PreampSource stamped (const char* base, const char* controlsSpec,
                          std::initializer_list<std::pair<const char*, const char*>> settings,
                          const char* rigId, const char* gearModel)
    {
        auto s = bare (base);
        if (controlsSpec != nullptr) s.meta.set ("controls", controlsSpec);
        if (rigId != nullptr)        s.meta.set ("rig_id", rigId);
        if (gearModel != nullptr)    s.meta.set ("gear_model", gearModel);
        for (const auto& [k, v] : settings)
            s.meta.set ("settings." + juce::String (k), v);
        return s;
    }

    // The SHIPPED factory library as OrbitCabAudioProcessor::preampRig() feeds it: every packed capture in
    // resources/preamps, id "fp:<stem>", its header metadata, ordered by stem case-insensitively (as preampSources()
    // orders it). A private build also embeds resources/preamps-local/ — gitignored, so it is not rendered here.
    std::vector<PreampSource> factoryLibrary()
    {
        std::vector<PreampSource> out;
       #ifdef ORBITCAB_RES_DIR
        for (const auto& f : juce::File (ORBITCAB_RES_DIR).getChildFile ("preamps")
                                 .findChildFiles (juce::File::findFiles, false, "*.namz"))
        {
            PreampSource s;
            s.base    = f.getFileNameWithoutExtension();
            s.id      = "fp:" + s.base;
            s.factory = true;
            juce::MemoryBlock mb;
            if (f.loadFileAsData (mb))
                s.meta = ocnam::readMeta (mb.getData(), mb.getSize());
            out.push_back (std::move (s));
        }
       #endif
        std::sort (out.begin(), out.end(), [] (const PreampSource& a, const PreampSource& b)
                   { return a.base.compareIgnoreCase (b.base) < 0; });
        return out;
    }

    juce::String quoted (const std::string& s) { return "\"" + rigdetail::fromStd (s) + "\""; }

    juce::String settingsText (const namz::rig::Settings& s)
    {
        juce::StringArray parts;
        for (const auto& [k, v] : s)                       // std::map — already in key order
            parts.add (rigdetail::fromStd (k) + "=" + rigdetail::fromStd (v));
        return "{" + parts.joinIntoString (",") + "}";
    }

    // One library → its section of the rendering: the sources, the model (devices, controls, files, entries),
    // then every question the editor can ask of it. Titles stay ASCII: a literal reaches `title` through
    // juce::String's ASCII constructor, which asserts on anything else and would write it mangled.
    void render (juce::StringArray& out, const juce::String& title, const std::vector<PreampSource>& sources)
    {
        PreampRig rig;
        rig.build (sources);

        out.add ("## " + title);
        std::map<juce::String, int> at;                    // id → its row, so answers print short
        for (int i = 0; i < (int) sources.size(); ++i)
        {
            at[sources[(size_t) i].id] = i;
            out.add ("src #" + juce::String (i) + " " + sources[(size_t) i].id);
        }
        const auto ans = [&at] (const juce::String& id) -> juce::String
        {
            if (id.isEmpty()) return "-";
            const auto it = at.find (id);
            return it != at.end() ? "#" + juce::String (it->second) : "?" + id;
        };

        for (int di = 0; di < (int) rig.devices.size(); ++di)
        {
            const auto& d = rig.devices[(size_t) di];
            out.add ("device " + juce::String (di) + " family=" + quoted (d.family) + " rig=" + quoted (d.rigId)
                     + " slot=" + quoted (d.slot) + " key=" + rig.deviceKey (d)
                     + " group=" + juce::String ((int) rig.isGroup (d)));
            for (const auto& c : d.controls)
            {
                juce::StringArray vs;
                for (const auto& v : c.values) vs.add (rigdetail::fromStd (v));
                out.add ("  control " + rigdetail::fromStd (c.name) + ":" + namz::rig::roleToString (c.role)
                         + "=" + vs.joinIntoString ("|"));
            }
            for (const auto& fe : d.files)
                out.add ("  file " + ans (rigdetail::fromStd (fe.id)) + " " + settingsText (fe.settings));
        }
        for (const auto& e : rig.entries)
            out.add ("entry " + ans (e.id) + " name=\"" + e.name + "\" variant=\"" + e.variant
                     + "\" factory=" + juce::String ((int) e.factory));

        for (const auto& s : sources)
        {
            const auto v = rig.viewFor (s.id);
            juce::String line = "view " + ans (s.id) + " group=" + juce::String ((int) v.group)
                              + " dev=" + juce::String (v.deviceIndex);
            for (const auto& cv : v.controls)
                line << " [" << cv.name << ":" << namz::rig::roleToString (cv.role) << "="
                     << cv.values.joinIntoString ("|") << " cur=" << cv.current << " vis=" << (int) cv.visible << "]";
            out.add (line);

            if (v.deviceIndex < 0)
                continue;
            // Every value each control could be turned to, and one that no file carries.
            for (const auto& c : rig.devices[(size_t) v.deviceIndex].controls)
            {
                juce::String turn = "  turn " + rigdetail::fromStd (c.name) + ":";
                auto values = c.values;
                values.push_back ("never-captured");
                for (const auto& value : values)
                    turn << " " << rigdetail::fromStd (value) << ">"
                         << ans (rig.resolveControl (s.id, rigdetail::fromStd (c.name), rigdetail::fromStd (value)));
                out.add (turn);
            }
        }

        // A device switch from nothing and from every file, onto every device and one index past the end.
        std::vector<juce::String> froms { juce::String() };
        for (const auto& s : sources) froms.push_back (s.id);
        for (const auto& from : froms)
        {
            juce::String line = "switch " + ans (from) + ":";
            for (int di = 0; di <= (int) rig.devices.size(); ++di)
                line << " " << di << ">" << ans (rig.resolveDevice (from, di));
            out.add (line);
        }

        out.add ("unknown id: dev=" + juce::String (rig.viewFor ("nope").deviceIndex)
                 + " turn=" + ans (rig.resolveControl ("nope", "channel", "green")));
    }

    juce::String renderAll()
    {
        juce::StringArray out;
        render (out, "factory library (resources/preamps)", factoryLibrary());

        // PreampRigTests' libraries, answered in full rather than spot-checked.
        render (out, "legacy family + singleton", { bare ("Voltage ch1 12h"), bare ("Voltage ch2 12h"),
                bare ("Voltage ch2 12h boost"), bare ("Voltage ch2 16h"), bare ("Voltage ch3 12h"), bare ("Studio Pre") });
        render (out, "colour rank", { bare ("GtrVolt-blue-12h"), bare ("GtrVolt-green-12h"),
                bare ("GtrVolt-orange-12h"), bare ("GtrVolt-green-16h") });
        render (out, "grammar edges", { bare ("ch5 amp"), bare ("Channel Two"), bare ("Boosted"), bare ("Reactor X1") });
        render (out, "variant badges", { bare ("GtrVolt-green-12h-boost"), bare ("GtrVolt-green-12h"),
                bare ("GtrVolt-red-07h") });
        {
            const char* spec = "channel:channel=green|red; mode:generic=classic|modern";
            render (out, "metadata path", {
                stamped ("take-001", spec, { { "channel", "green" }, { "mode", "classic" } }, "dc-test-rig", "ReVolt Guitar"),
                stamped ("take-002", spec, { { "channel", "green" }, { "mode", "modern" } },  "dc-test-rig", "ReVolt Guitar"),
                stamped ("take-003", spec, { { "channel", "red" },   { "mode", "classic" } }, "dc-test-rig", "ReVolt Guitar"),
                stamped ("take-004", spec, { { "channel", "red" },   { "mode", "modern" } },  "dc-test-rig", "ReVolt Guitar") });
        }
        render (out, "rig_id grouping", {
            stamped ("A-07", "gain:gain=07h|12h", { { "gain", "07h" } }, "dc-test-rig", "ReVolt Guitar"),
            stamped ("B-12-renamed", "gain:gain=07h|12h", { { "gain", "12h" } }, nullptr, "ReVolt Guitar") });
        // …and the same two files in the OTHER order: an id'd file that arrives after an un-id'd family opens a
        // second device (the policy's "a later id'd file names the group" can never fire). Rendered so the order
        // dependence stands on the record instead of being certified by the one order that happens to merge.
        render (out, "rig_id grouping, reversed", {
            stamped ("B-12-renamed", "gain:gain=07h|12h", { { "gain", "12h" } }, nullptr, "ReVolt Guitar"),
            stamped ("A-07", "gain:gain=07h|12h", { { "gain", "07h" } }, "dc-test-rig", "ReVolt Guitar") });
        {
            const char* spec = "channel:channel=green|red; gain:gain=07h|12h|17h";
            render (out, "sparse metadata matrix", {
                stamped ("t1", spec, { { "channel", "green" }, { "gain", "07h" } }, "dc-test-rig", "ReVolt Guitar"),
                stamped ("t2", spec, { { "channel", "green" }, { "gain", "12h" } }, "dc-test-rig", "ReVolt Guitar"),
                stamped ("t3", spec, { { "channel", "red" },   { "gain", "12h" } }, "dc-test-rig", "ReVolt Guitar") });
        }

        // Libraries the targeted tests never built — each aims at one rule of the policy.
        // The gain DEFAULT decides a sparse fallback: green 17h → red has no red 17h, and 12h beats 07h only while
        // the default is the clock's noon (namz v4's default would be 07h, the first value).
        render (out, "sparse legacy: the default decides", { bare ("Amp green 07h"), bare ("Amp green 17h"),
                bare ("Amp red 07h"), bare ("Amp red 12h") });
        // A legacy family and a stamped file of the same name merge — in either order.
        render (out, "legacy then stamped, one family", { bare ("Voltage ch1 12h"), bare ("Voltage ch2 16h"),
                stamped ("Voltage stamped", "gain:gain=12h|16h", { { "gain", "12h" } }, nullptr, "Voltage") });
        render (out, "stamped then legacy, one family", {
                stamped ("Voltage stamped", "gain:gain=12h|16h", { { "gain", "12h" } }, nullptr, "Voltage"),
                bare ("Voltage ch1 12h"), bare ("Voltage ch2 16h") });
        // A stamp whose dial speaks DEGREES joining a legacy family by name: the family takes the stamp's controls
        // (no channel, no boost any more) and the legacy files keep a clock gain that is not on the dial.
        render (out, "legacy family + degree stamp of the same name", { bare ("Voltage ch1 12h"),
                bare ("Voltage ch2 16h boost"),
                stamped ("Voltage stamped", "gain:gain=0|150|300", { { "gain", "150" } }, nullptr, "Voltage") });
        // Names that are nothing but tokens form ONE nameless device; a named neighbour stays apart.
        render (out, "token-only pack", { bare ("green 12h"), bare ("red 12h"), bare ("red 12h boost"),
                bare ("green 17h"), bare ("Clean 12h") });
        // Topology keeps its case; chN is 1..4 only; a colour inside the name is still a channel.
        render (out, "topology, chN, colour mid-name", { bare ("PowerX PP 12h"), bare ("PowerX SE 12h"),
                bare ("PowerX pp 16h"), bare ("Duo ch1"), bare ("Duo ch2"), bare ("Duo ch4"), bare ("Bad ch5 thing"),
                bare ("Studio green Pre 12h"), bare ("Studio red Pre 12h") });
        // A factory capture and a user copy of it: one family, identical settings — find() takes the first.
        render (out, "factory + user copy", { bare ("Amp green 12h", "fp:"), bare ("Amp red 12h", "fp:"),
                bare ("Amp green 12h"), bare ("Amp green 16h BOOST") });
        // Degree-stamped dials (the current capture convention): the default the plugin shipped with.
        {
            const char* spec = "channel:channel=green|red; gain:gain=0|150|300";
            render (out, "degree-stamped dial", {
                stamped ("d1", spec, { { "channel", "green" }, { "gain", "0" } },   "deg-rig", "Deg Amp"),
                stamped ("d2", spec, { { "channel", "green" }, { "gain", "300" } }, "deg-rig", "Deg Amp"),
                stamped ("d3", spec, { { "channel", "red" },   { "gain", "150" } }, "deg-rig", "Deg Amp") });
        }
        // One file both branches could claim: STAMPED, yet named in the grammar. The metadata branch takes it and
        // never parses the name; a bare file whose grammar family equals the stamp's gear_model joins that device.
        render (out, "stamped file with a grammar name", {
                stamped ("Voltage ch2 12h boost", "gain:gain=12h|16h", { { "gain", "16h" } }, nullptr, "V4"),
                bare ("V4 ch1 12h"), bare ("Voltage ch1 12h") });

        // Four libraries the first mutation round asked for — each one a mutant that survived every library above.
        // A clock token is two or three characters: "100h" is part of a NAME, not a gain.
        render (out, "clock edges", { bare ("Amp 7h"), bare ("Amp 12h"), bare ("Amp 100h"), bare ("Amp 17h") });
        // Keeping what the user had outweighs sitting on defaults: from green 17h boost, a turn to red keeps 17h
        // (red 17h) rather than take red 12h, which sits on two defaults and keeps nothing.
        render (out, "keeping outweighs defaults", { bare ("Q green 17h boost"), bare ("Q green 12h"),
                bare ("Q red 17h"), bare ("Q red 12h") });
        // A TIE on score goes to the file sitting on more defaults: from (x, q, u), turning c to v finds (x, r, v),
        // which keeps a — a's default — and (y, q, v), which keeps b — not b's default — both at a score of 4.
        {
            const char* spec = "a:generic=x|y|z; b:generic=p|q|r; c:generic=u|v";
            render (out, "a tie on score", {
                stamped ("T-user",    spec, { { "a", "x" }, { "b", "q" }, { "c", "u" } }, "tie-rig", "Tie Amp"),
                stamped ("T-keeps-a", spec, { { "a", "x" }, { "b", "r" }, { "c", "v" } }, "tie-rig", "Tie Amp"),
                stamped ("T-keeps-b", spec, { { "a", "y" }, { "b", "q" }, { "c", "v" } }, "tie-rig", "Tie Amp") });
        }
        // Two files of one device stamped with DIFFERENT specs: the first spec seen is the device's controls.
        render (out, "two specs, one device", {
                stamped ("S1", "gain:gain=07h|12h",     { { "gain", "07h" } }, "two-spec", "Spec Amp"),
                stamped ("S2", "gain:gain=07h|12h|17h", { { "gain", "17h" } }, "two-spec", "Spec Amp") });
        return out.joinIntoString ("\n") + "\n";
    }
}

struct PreampRigGoldenTest : juce::UnitTest
{
    PreampRigGoldenTest() : juce::UnitTest ("PreampRig golden") {}

    void runTest() override
    {
        beginTest ("every answer of the device model is the shipped plugin's (tests/golden/preamp-rig.txt)");

        // The fixture is live only if the factory library was found AND its .namz headers were read.
        const auto factory = factoryLibrary();
        expect (! factory.empty() && factory.front().meta.size() > 0,
                "the factory library must be found and its .namz headers read");

        const auto text = renderAll();
        // juce's accessor, not std::getenv — which MSVC deprecates (C4996).
        if (const auto path = juce::SystemStats::getEnvironmentVariable ("ORBITCAB_GOLDEN_OUT", {}); path.isNotEmpty())
            juce::File (path).replaceWithText (text, false, false, "\n");

       #ifdef ORBITCAB_RES_DIR
        const auto golden = juce::File (ORBITCAB_RES_DIR).getSiblingFile ("tests")
                                .getChildFile ("golden").getChildFile ("preamp-rig.txt");
        expect (golden.existsAsFile(), "golden reference missing: " + golden.getFullPathName());
        const auto want = golden.loadFileAsString().removeCharacters ("\r");   // a CRLF checkout is the same text
        if (want != text)
        {
            const auto w = juce::StringArray::fromLines (want);
            const auto g = juce::StringArray::fromLines (text);
            int line = 0;
            while (line < juce::jmin (w.size(), g.size()) && w[line] == g[line]) ++line;
            expect (false, "rendering departs from the golden at line " + juce::String (line + 1)
                           + "\n  golden:   " + w[line] + "\n  rendered: " + g[line]
                           + "\n  (ORBITCAB_GOLDEN_OUT=<file> writes the full rendering)");
        }
       #else
        expect (false, "ORBITCAB_RES_DIR is not defined, so the golden cannot run");
       #endif
    }
};

static PreampRigGoldenTest preampRigGoldenTest;
