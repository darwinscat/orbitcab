// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for orbitcab::PreampLibrary — the filename → (display name, channel, gain
// hours, boost) parsing that drives the PREAMP selector, plus the user-folder scan. Pure
// (juce_core), no engine, no .nam content. Mirrors PowerampLibraryTests.cpp.
#include <juce_core/juce_core.h>

#include "PreampLibrary.h"

using namespace orbitcab;

struct PreampLibraryTest : juce::UnitTest
{
    PreampLibraryTest() : juce::UnitTest ("PreampLibrary") {}

    void runTest() override
    {
        juce::String n; int ch = -1, h = -1; bool boost = true;
        auto parse = [&] (const char* base) { parsePreampName (base, n, ch, h, boost); };

        beginTest ("channel token chN → channel, dropped from the display name");
        {
            parse ("Voltage ch1"); expect (n == "Voltage" && ch == 1 && h == 0 && ! boost);
            parse ("Voltage ch2"); expect (n == "Voltage" && ch == 2);
            parse ("Voltage ch3"); expect (n == "Voltage" && ch == 3);
            parse ("Vortex");      expect (n == "Vortex"  && ch == 0);   // no token → 0
        }

        beginTest ("<N>h gain token → hours, dropped from the display name");
        {
            parse ("Voltage 12h"); expect (n == "Voltage" && h == 12 && ch == 0);
            parse ("Voltage 7h");  expect (n == "Voltage" && h == 7);
            parse ("Voltage 17h"); expect (n == "Voltage" && h == 17);
            parse ("Voltage");     expect (h == 0);                     // no token → 0
        }

        beginTest ("boost token → boost on, dropped from the display name");
        {
            parse ("Voltage boost"); expect (n == "Voltage" && boost);
            parse ("Voltage BOOST"); expect (n == "Voltage" && boost);   // case-insensitive
            parse ("Voltage");       expect (! boost);                  // absent → off
        }

        beginTest ("all dimensions together, any order, all stripped from the name");
        {
            parse ("Voltage ch2 12h boost"); expect (n == "Voltage" && ch == 2 && h == 12 && boost);
            parse ("boost 9h ch3 Vortex");   expect (n == "Vortex"  && ch == 3 && h == 9  && boost);
            parse ("Voltage 16h");           expect (n == "Voltage" && ch == 0 && h == 16 && ! boost);  // gain only
        }

        beginTest ("no tag → name unchanged, defaults");
        {
            parse ("Studio Pre"); expect (n == "Studio Pre" && ch == 0 && h == 0 && ! boost);
            parse ("Reactor X1");   expect (n == "Reactor X1"   && ch == 0);   // 'X1' isn't a channel tag
        }

        beginTest ("token match is whole-word / range-checked, else part of the name");
        {
            parse ("ch5 amp");     expect (ch == 0 && n == "ch5 amp");     // ch5 out of range (max ch4) → not a channel
            parse ("Channel Two"); expect (ch == 0 && n == "Channel Two"); // 'Channel' isn't 'chN'
            parse ("Boosted");     expect (! boost && n == "Boosted");     // contains 'boost' but not whole word
        }

        beginTest ("channel: chN widened to ch4, and colour words map to a tinted channel + label");
        {
            juce::String lbl = "x"; juce::uint32 col = 0xdead;
            auto parseFull = [&] (const char* base) { parsePreampName (base, n, ch, h, boost, lbl, col); };

            parseFull ("Mesa ch4");      expect (n == "Mesa" && ch == 4 && lbl == "Ch 4" && col == 0);   // ch4 now in range, plain (no tint)
            parseFull ("Mesa ch1");      expect (ch == 1 && lbl == "Ch 1" && col == 0);

            parseFull ("V4KRAK red 9h");                                                                    // colour → channel index + tint
            expect (n == "V4KRAK" && ch == 4 && h == 9 && lbl == "Red" && col == 0xffe0524e);
            parseFull ("V4KRAK GREEN 12h");                                                                 // case-insensitive; label Title-cased
            expect (n == "V4KRAK" && ch == 1 && h == 12 && lbl == "Green" && col == 0xff57c06a);
            parseFull ("V4KRAK-blue-17h");                                                                  // dash separators + colour
            expect (n == "V4KRAK" && ch == 3 && h == 17 && lbl == "Blue");

            // GtrVolt (Two Notes ReVolt) production naming: GtrVolt-<colour>-<Nh>[-boost].
            parseFull ("GtrVolt-green-07h");        expect (n == "GtrVolt" && ch == 1 && h == 7  && ! boost && lbl == "Green");
            parseFull ("GtrVolt-orange-12h");       expect (n == "GtrVolt" && ch == 2 && h == 12 && ! boost && lbl == "Orange");
            parseFull ("GtrVolt-blue-16h-boost");   expect (n == "GtrVolt" && ch == 3 && h == 16 &&   boost && lbl == "Blue");

            parseFull ("Studio Pre");    expect (ch == 0 && lbl.isEmpty() && col == 0);                   // no channel → empty label, no tint
            parseFull ("Red Llama 12h"); expect (ch == 4 && n == "Llama" && lbl == "Red");               // a colour word IS taken as the channel (documented: use chN to avoid)
        }

        beginTest ("tokens are the whole name → keep something to show");
        {
            parse ("ch2");   expect (ch == 2 && n.isNotEmpty());
            parse ("boost"); expect (boost  && n.isNotEmpty());
        }

        beginTest ("first whole-word token of each type wins; separators trimmed; h is case-insensitive");
        {
            parse ("Voltage ch1 ch2");  expect (ch == 1 && n == "Voltage ch2");  // first chN wins; the rest stays in the name
            parse ("  Voltage   12h  "); expect (n == "Voltage" && h == 12);      // leading/trailing/extra separators trimmed
            parse ("-Voltage-ch2-");     expect (n == "Voltage" && ch == 2);      // dash/underscore separators tokenise too
            parse ("Voltage 12H");       expect (n == "Voltage" && h == 12);      // uppercase 'H' token still matches
        }

        beginTest ("scanPreampLibrary: only *.nam, parsed, sorted by name, id = up:<base>");
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("orbitcab_pre_test_" + juce::String (juce::Time::currentTimeMillis()));
            dir.createDirectory();
            dir.getChildFile ("Voltage ch2 12h boost.nam").replaceWithText ("{}");
            dir.getChildFile ("Studio Pre 9h.nam").replaceWithText ("{}");
            dir.getChildFile ("notes.txt").replaceWithText ("ignore me");   // non-.nam is ignored

            auto lib = scanPreampLibrary (dir);
            expect (lib.size() == 2);                                       // the two .nam only
            if (lib.size() == 2)
            {
                expect (lib[0].name == "Studio Pre" && lib[0].hours == 9 && lib[0].channel == 0 && ! lib[0].boost);
                expect (lib[1].name == "Voltage"  && lib[1].channel == 2 && lib[1].hours == 12 && lib[1].boost);
                expect (lib[1].id == "up:Voltage ch2 12h boost" && ! lib[1].factory && lib[1].file.existsAsFile());
            }

            // empty / non-existent folder → empty library (the public-build / fresh-machine case)
            expect (scanPreampLibrary (dir.getChildFile ("nope")).empty());
            dir.deleteRecursively();
        }

        beginTest ("scanPreampLibrary: a shared name sorts by channel, then gain (the variant order)");
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("orbitcab_pre_sort_" + juce::String (juce::Time::currentTimeMillis()));
            dir.createDirectory();
            dir.getChildFile ("Voltage ch2 12h.nam").replaceWithText ("{}");   // deliberately out of order on disk
            dir.getChildFile ("Voltage ch1 16h.nam").replaceWithText ("{}");
            dir.getChildFile ("Voltage ch1 12h.nam").replaceWithText ("{}");

            auto lib = scanPreampLibrary (dir);
            expect (lib.size() == 3);
            if (lib.size() == 3)
            {
                expect (lib[0].channel == 1 && lib[0].hours == 12);   // same name → channel asc, then gain asc
                expect (lib[1].channel == 1 && lib[1].hours == 16);
                expect (lib[2].channel == 2 && lib[2].hours == 12);
            }
            dir.deleteRecursively();
        }
    }
};

static PreampLibraryTest preampLibraryTest;
