// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for orbitcab::PreampSelector — the pure resolve/view-model the editor's PREAMP
// row binds to (extracted from the GUI so the tricky bits are testable): the "show a control only
// when the current choice has ≥2 values for that dimension" rule, the keep-other-dimensions-else-
// default resolution, the default pickers, and the channel-0 mixed-tag edge case. Pure (juce_core).
#include <juce_core/juce_core.h>

#include "PreampSelector.h"

using namespace orbitcab;

namespace
{
    PreampEntry mk (const char* name, int channel, int hours, bool boost)
    {
        PreampEntry e;
        e.name = name; e.channel = channel; e.hours = hours; e.boost = boost;
        // id mirrors the user-folder convention; uniqueness is all the model needs.
        e.id = "up:" + juce::String (name)
                 + (channel ? " ch" + juce::String (channel) : juce::String())
                 + (hours   ? " "   + juce::String (hours) + "h" : juce::String())
                 + (boost   ? juce::String (" boost") : juce::String());
        return e;
    }

    // A "Voltage" family across all three dimensions + a "Studio Pre" singleton.
    PreampSelector makeSel()
    {
        PreampSelector s;
        s.lib = {
            mk ("Voltage", 1, 12, false),
            mk ("Voltage", 2, 12, false),
            mk ("Voltage", 2, 12, true),
            mk ("Voltage", 2, 16, false),
            mk ("Voltage", 3, 12, false),
            mk ("Studio Pre", 0, 0, false),   // a one-off (no shared name) → singleton
        };
        return s;
    }
}

struct PreampSelectorTest : juce::UnitTest
{
    PreampSelectorTest() : juce::UnitTest ("PreampSelector") {}

    void runTest() override
    {
        beginTest ("default pickers");
        {
            expect (PreampSelector::defaultGain ({}) == 0);
            expect (PreampSelector::defaultGain ({ 9, 12, 15 }) == 12);          // prefer noon
            expect (PreampSelector::defaultGain ({ 9, 15, 16 }) == 15);          // else the middle stop
            expect (PreampSelector::defaultChannel ({}) == 0);
            expect (PreampSelector::defaultChannel ({ 2, 3 }) == 2);             // lowest available
            expect (PreampSelector::defaultBoost ({}) == false);
            expect (PreampSelector::defaultBoost ({ false, true }) == false);    // prefer the clean capture
            expect (PreampSelector::defaultBoost ({ true }) == true);            // only boost exists → boost
        }

        beginTest ("queries: groups, distinct dimension values");
        {
            auto s = makeSel();
            expect (s.isGroupName ("Voltage"));
            expect (! s.isGroupName ("Studio Pre"));
            expect (s.groupNames() == std::vector<juce::String> { "Voltage" });
            expect ((s.channelsForName ("Voltage") == std::vector<int> { 1, 2, 3 }));
            expect ((s.gainsForNameChannel ("Voltage", 1) == std::vector<int> { 12 }));
            expect ((s.gainsForNameChannel ("Voltage", 2) == std::vector<int> { 12, 16 }));
            expect ((s.boostsForNameChGain ("Voltage", 2, 12) == std::vector<bool> { false, true }));
            expect ((s.boostsForNameChGain ("Voltage", 2, 16) == std::vector<bool> { false }));
            expect (s.findId ("Voltage", 2, 12, true) == "up:Voltage ch2 12h boost");
            expect (s.findId ("Voltage", 2, 99, false).isEmpty());              // no such variant
        }

        beginTest ("viewFor: a control shows only when its dimension has ≥2 values");
        {
            auto s = makeSel();

            auto v1 = s.viewFor ("up:Voltage ch1 12h");
            expect (v1.group && v1.showChannels && v1.currentChannel == 1);     // 3 channels → switch shown
            expect (! v1.showGain);                                             // ch1 has only 12h → no slider
            expect (! v1.showBoost);                                            // ch1 12h has no boost variant

            auto v2 = s.viewFor ("up:Voltage ch2 12h");
            expect (v2.showGain && (v2.gains == std::vector<int> { 12, 16 }) && v2.currentGain == 12);
            expect (v2.showBoost && v2.currentBoost == false);                  // ch2 12h has clean + boost

            auto v2b = s.viewFor ("up:Voltage ch2 16h");
            expect (v2b.showGain && ! v2b.showBoost);                           // ch2 16h has only the clean capture

            auto vs = s.viewFor ("up:Studio Pre");
            expect (! vs.group && ! vs.showChannels && ! vs.showGain && ! vs.showBoost);
            auto vNone = s.viewFor ("nope");
            expect (! vNone.group && ! vNone.showChannels);
        }

        beginTest ("resolveName: keep the current channel/gain/boost if they exist there, else default");
        {
            auto s = makeSel();
            // from nothing selected → defaults: lowest channel (1), its only gain (12), clean.
            expect (s.resolveName ("", "Voltage") == "up:Voltage ch1 12h");
            // currently on ch2/16h/clean → keep ch2 + 16h under Voltage (they exist), boost stays clean.
            expect (s.resolveName ("up:Voltage ch2 16h", "Voltage") == "up:Voltage ch2 16h");
        }

        beginTest ("resolveChannel/Gain/Boost: switch one dimension, drop the rest only if absent");
        {
            auto s = makeSel();
            // ch2/12h/boost → ch1: ch1 keeps 12h (exists) but has no boost → falls back to clean.
            expect (s.resolveChannel ("up:Voltage ch2 12h boost", 1) == "up:Voltage ch1 12h");
            // ch2/12h/boost → gain 16h: ch2/16h has no boost → clean.
            expect (s.resolveGain ("up:Voltage ch2 12h boost", 16) == "up:Voltage ch2 16h");
            // ch2/12h/clean → boost on: the boosted twin exists.
            expect (s.resolveBoost ("up:Voltage ch2 12h", true) == "up:Voltage ch2 12h boost");
            // resolving against no current selection is a safe no-op.
            expect (s.resolveChannel ("", 2).isEmpty());
        }

        beginTest ("channel-0 mixed-tag family is reported (the editor renders only ch1..3) — parity case");
        {
            PreampSelector s;
            s.lib = { mk ("Mix", 0, 0, false), mk ("Mix", 2, 0, false) };       // untagged + ch2 under one name
            expect (s.isGroupName ("Mix"));
            expect ((s.channelsForName ("Mix") == std::vector<int> { 0, 2 }));  // 0 is a real value to the model
            auto v = s.viewFor ("up:Mix");                                       // selecting the untagged variant
            expect (v.showChannels && v.currentChannel == 0);                    // switch shows; channel 0 has no button (UI)
            // selecting the name defaults to the lowest channel — here that's the untagged 0.
            expect (s.resolveName ("", "Mix") == "up:Mix");
            expect (s.resolveChannel ("up:Mix", 2) == "up:Mix ch2");
        }
    }
};

static PreampSelectorTest preampSelectorTest;
