// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <vector>

namespace orbitcab
{

//==============================================================================
// The poweramp .nam library the editor's POWERAMP selector lists. TWO sources, merged
// into one list by the processor (which owns BinaryData access):
//   • FACTORY — models embedded in the build from resources/poweramps/ (gitignored content;
//     a public build ships none). id = "f:<base>".
//   • USER    — the system-wide, per-machine folder AppPreferences::powerampDir(), shared
//     across instances, managed from the gear panel. id = "u:<base>"; `file` is valid.
// scanPowerampLibrary() enumerates only the USER folder; the processor prepends the factory
// entries. `id` is the stable selection key persisted in session state (resolved back to a
// source on load; absent if the model is gone).
//
// CATEGORY drives the editor's 3-position mode switch (PP / SE / Other) — it's parsed from a
// whole-word "PP" or "SE" token in the filename, which is then dropped from the display name:
//   "6L6 PP.nam"        → category PP, display "6L6"   (push-pull → 2 glowing tubes)
//   "6L6 SE.nam"        → category SE, display "6L6"   (single-ended → 1 tube)
//   "Marshall Plexi.nam"→ category Other, display "Marshall Plexi"   (amp icon only, no tubes)
//==============================================================================
enum class PowerampCat { pushPull, singleEnded, other };

struct PowerampEntry
{
    juce::String id;          // stable selection key: "f:<base>" (factory) | "u:<base>" (user folder)
    juce::String name;        // display name (filename minus extension, minus the PP/SE + <N>h tokens)
    PowerampCat  cat;         // PP / SE / Other — from the filename token; drives the mode switch + tube count
    int          hours = 0;   // clock-position knob setting from a "<N>h" token (9/12/15); 0 = none → no slider
    bool         factory;     // true → embedded (BinaryData); false → user folder
    juce::File   file;        // valid only when ! factory (the source file in powerampDir)
};

// Split a base filename into (display name, category, hours) on tokens separated by space / dash /
// underscore: a whole-word "PP"/"SE" (any case) → category; a "<N>h" (e.g. 12h) → the clock-position
// knob setting. Both tokens are dropped from the display name. No PP/SE → Other; no <N>h → hours 0.
//   "6L6 PP 12h"  → name "6L6",  cat PP,    hours 12   (one of several positions of the same capture)
//   "6L6 PP-2"    → name "6L6",  cat PP,    hours 0    (legacy: a trailing tube count is still stripped)
//   "Marshall"    → name "Marshall", cat Other, hours 0
inline void parsePowerampName (const juce::String& base, juce::String& nameOut, PowerampCat& catOut, int& hoursOut)
{
    catOut   = PowerampCat::other;
    hoursOut = 0;
    nameOut  = base;   // default: no tags → name unchanged. Set up front so the result never depends
                       // on what the caller passed in (a reused out-string used to return stale).
    juce::StringArray words;
    words.addTokens (base, " -_", "");
    words.removeEmptyStrings();

    // PP / SE tag → category (+ drop a legacy "-N" tube count tokenised right after it: "6L6 PP-2").
    for (int i = 0; i < words.size(); ++i)
    {
        const auto w = words[i].toUpperCase();
        if (w == "PP" || w == "SE")
        {
            catOut = (w == "PP" ? PowerampCat::pushPull : PowerampCat::singleEnded);
            if (i + 1 < words.size() && words[i + 1].containsOnly ("0123456789"))
                words.remove (i + 1);
            words.remove (i);
            break;
        }
    }

    // Clock-position tag "<N>h" (9h / 12h / 15h) → hours; dropped from the display name.
    for (int i = 0; i < words.size(); ++i)
    {
        const auto w = words[i].toLowerCase();
        if (w.length() >= 2 && w.getLastCharacter() == (juce::juce_wchar) 'h'
            && w.dropLastCharacters (1).containsOnly ("0123456789"))
        {
            hoursOut = w.dropLastCharacters (1).getIntValue();
            words.remove (i);
            break;
        }
    }

    const auto joined = words.joinIntoString (" ").trim();
    if (joined.isNotEmpty())
        nameOut = joined;   // else keep base (the tags were the whole name)
}

// Tubes drawn for a category: push-pull = a pair, single-ended = one, other = none (amp icon only).
inline int tubeCountForCat (PowerampCat c)
{
    return c == PowerampCat::pushPull ? 2 : c == PowerampCat::singleEnded ? 1 : 0;
}

inline std::vector<PowerampEntry> scanPowerampLibrary (const juce::File& dir)
{
    std::vector<PowerampEntry> out;
    if (! dir.isDirectory())
        return out;

    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.nam");      // raw captures
    files.addArray (dir.findChildFiles (juce::File::findFiles, false, "*.namz")); // + packed captures
    for (const auto& f : files)
    {
        PowerampEntry e;
        e.factory = false;
        e.file    = f;
        const auto base = f.getFileNameWithoutExtension();
        e.id = "u:" + base;
        parsePowerampName (base, e.name, e.cat, e.hours);
        out.push_back (std::move (e));
    }
    std::sort (out.begin(), out.end(),
               [] (const PowerampEntry& a, const PowerampEntry& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return out;
}

} // namespace orbitcab
