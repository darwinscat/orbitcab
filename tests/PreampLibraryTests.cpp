// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

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
            parse ("ch4 amp");     expect (ch == 0 && n == "ch4 amp");     // ch4 out of range → not a channel
            parse ("Channel Two"); expect (ch == 0 && n == "Channel Two"); // 'Channel' isn't 'chN'
            parse ("Boosted");     expect (! boost && n == "Boosted");     // contains 'boost' but not whole word
        }

        beginTest ("tokens are the whole name → keep something to show");
        {
            parse ("ch2");   expect (ch == 2 && n.isNotEmpty());
            parse ("boost"); expect (boost  && n.isNotEmpty());
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
    }
};

static PreampLibraryTest preampLibraryTest;
