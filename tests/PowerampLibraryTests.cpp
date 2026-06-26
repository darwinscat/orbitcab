// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for orbitcab::PowerampLibrary — the filename → (display name, mode, tube
// count) parsing that drives the POWERAMP selector, plus the user-folder scan. Pure (juce_core),
// no engine, no .nam content.
#include <juce_core/juce_core.h>

#include "PowerampLibrary.h"

using namespace orbitcab;

struct PowerampLibraryTest : juce::UnitTest
{
    PowerampLibraryTest() : juce::UnitTest ("PowerampLibrary") {}

    void runTest() override
    {
        juce::String n; PowerampCat c; int h = -1;
        auto parse = [&] (const char* base) { parsePowerampName (base, n, c, h); };

        beginTest ("PP / SE token → mode, dropped from the display name");
        {
            parse ("6L6 PP");  expect (c == PowerampCat::pushPull    && n == "6L6" && h == 0);
            parse ("6L6 SE");  expect (c == PowerampCat::singleEnded && n == "6L6" && h == 0);
            parse ("EL34 PP"); expect (c == PowerampCat::pushPull    && n == "EL34");
        }

        beginTest ("<N>h clock-position token → hours, dropped from the display name");
        {
            parse ("6L6 PP 12h"); expect (c == PowerampCat::pushPull    && n == "6L6" && h == 12);
            parse ("6L6 SE 9h");  expect (c == PowerampCat::singleEnded && n == "6L6" && h == 9);
            parse ("EL34 PP 15h");expect (c == PowerampCat::pushPull    && n == "EL34" && h == 15);
            parse ("6L6 12h PP"); expect (c == PowerampCat::pushPull    && n == "6L6" && h == 12);   // order-independent
            parse ("Marshall 12h");expect (c == PowerampCat::other      && n == "Marshall" && h == 12);  // hours without a mode
            parse ("6L6 PP");     expect (h == 0);                                                    // no token → 0
        }

        beginTest ("no tag → Other, name unchanged");
        {
            parse ("Marshall Plexi"); expect (c == PowerampCat::other && n == "Marshall Plexi");
            parse ("Fryette PS1");    expect (c == PowerampCat::other && n == "Fryette PS1");
        }

        beginTest ("legacy trailing -N count after the tag is stripped");
        {
            parse ("6L6 PP-2");  expect (c == PowerampCat::pushPull    && n == "6L6");
            parse ("KT88 SE-1"); expect (c == PowerampCat::singleEnded && n == "KT88");
        }

        beginTest ("a number BEFORE the tag is part of the name, not the count");
        {
            parse ("JCM 800 PP");    expect (c == PowerampCat::pushPull && n == "JCM 800");
            parse ("Peavey 120 PP"); expect (c == PowerampCat::pushPull && n == "Peavey 120");
        }

        beginTest ("token match is case-insensitive + whole-word only");
        {
            parse ("6L6 pp");     expect (c == PowerampCat::pushPull && n == "6L6");   // lowercase tag
            parse ("PPamp");      expect (c == PowerampCat::other);                     // 'PP' inside a word → not a tag
            parse ("Suppressor"); expect (c == PowerampCat::other);                     // contains 'pp' but not whole word
        }

        beginTest ("token is the whole name → keep something to show");
        {
            parse ("PP"); expect (c == PowerampCat::pushPull    && n.isNotEmpty());
            parse ("SE"); expect (c == PowerampCat::singleEnded && n.isNotEmpty());
        }

        beginTest ("tube count follows the mode (PP 2 / SE 1 / Other 0)");
        {
            expect (tubeCountForCat (PowerampCat::pushPull)    == 2);
            expect (tubeCountForCat (PowerampCat::singleEnded) == 1);
            expect (tubeCountForCat (PowerampCat::other)       == 0);
        }

        beginTest ("scanPowerampLibrary: only *.nam, parsed, sorted by name, id = u:<base>");
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("orbitcab_pa_test_" + juce::String (juce::Time::currentTimeMillis()));
            dir.createDirectory();
            dir.getChildFile ("EL34 PP.nam").replaceWithText ("{}");
            dir.getChildFile ("6L6 SE.nam").replaceWithText ("{}");
            dir.getChildFile ("notes.txt").replaceWithText ("ignore me");   // non-.nam is ignored

            auto lib = scanPowerampLibrary (dir);
            expect (lib.size() == 2);                                       // the two .nam only
            if (lib.size() == 2)
            {
                expect (lib[0].name == "6L6"  && lib[0].cat == PowerampCat::singleEnded);   // "6L6" sorts before "EL34"
                expect (lib[1].name == "EL34" && lib[1].cat == PowerampCat::pushPull);
                expect (lib[0].id == "u:6L6 SE" && ! lib[0].factory && lib[0].file.existsAsFile());
            }

            // empty / non-existent folder → empty library (the public-build / fresh-machine case)
            expect (scanPowerampLibrary (dir.getChildFile ("nope")).empty());
            dir.deleteRecursively();
        }
    }
};

static PowerampLibraryTest powerampLibraryTest;
