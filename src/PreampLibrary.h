// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <vector>

namespace orbitcab
{

//==============================================================================
// The preamp .nam library the editor's PREAMP selector lists — a sibling of the POWERAMP
// library (see PowerampLibrary.h), but for the SECOND neural stage that runs IN FRONT of the
// poweramp (input → PREAMP → POWERAMP → cab). Same two-source merge:
//   • FACTORY — models embedded in the build from resources/preamps/ (gitignored content; a
//     public build ships none). They live in a SEPARATE binary-data namespace (PreampBinaryData)
//     so they never bleed into the poweramp's BinaryData enumeration. id = "fp:<base>".
//   • USER    — the system-wide, per-machine folder AppPreferences::preampDir(), shared across
//     instances, managed from the gear panel. id = "up:<base>"; `file` is valid.
// The "fp:"/"up:" prefixes are distinct from the poweramp's "f:"/"u:" so the two selections /
// pools never collide. scanPreampLibrary() enumerates only the USER folder; the processor
// prepends the factory entries. `id` is the stable selection key persisted in session state.
//
// A preamp has MORE selectable dimensions than the poweramp, all parsed from the filename and all
// OPTIONAL (a control only appears when the current choice has ≥2 values for that dimension):
//   • name    — the model (filename minus every token below).
//   • channel — the channel switch. EITHER a whole-word "ch1".."ch4" token → 1..4, OR a recognised
//               colour word (red/green/blue/yellow/orange/purple/white) → that colour's stable index,
//               so a unit's real channels (e.g. a red/green-channel amp) read — and glow — as colours
//               rather than an opaque "ch1/ch2". 0 = none. (chN and colours never mix in one family.)
//   • gain    — a "<N>h" token (clock position, 7h…17h) → hours; 0 = none. (Same token grammar as
//               the poweramp's hours, just surfaced as a "gain" slider.)
//   • boost   — a whole-word "boost" token → boost on. Absent = boost off.
// Every matched token is dropped from the display name, in any order:
//   "Voltage ch2 12h boost.nam" → name "Voltage", channel 2 ("Ch 2"),  gain 12h, boost on
//   "V4KRAK red 9h.nam"           → name "V4KRAK",    channel 1 ("Red", glows red), gain 9h
//   "Voltage 16h.nam"           → name "Voltage", channel 0, gain 16h, boost off (only a gain slider)
//   "Studio Pre.nam"            → name "Studio Pre", channel 0, gain 0, boost off (just the name)
//==============================================================================
struct PreampEntry
{
    juce::String id;          // stable selection key: "fp:<base>" (factory) | "up:<base>" (user folder)
    juce::String name;        // display name (filename minus the chN / colour / <N>h / boost tokens)
    int          channel = 0; // 1..4 from a "chN" token OR a colour's index; 0 = none → no channel switch
    juce::String channelLabel;    // channel switch caption: "" (none) | "Ch 2" (chN) | "Red"/"Green" (colour)
    juce::uint32 channelColour = 0; // ARGB tint for a colour channel (button glows this); 0 = plain chN
    int          hours   = 0; // gain clock-position from a "<N>h" token (7…17); 0 = none → no gain slider
    bool         boost   = false; // "boost" token present → boost on
    bool         factory = false; // true → embedded (PreampBinaryData); false → user folder
    juce::File   file;        // valid only when ! factory (the source file in preampDir)
};

// Recognised channel-colour words → a stable order index (used as the channel value) + an ARGB tint
// the channel button glows in. Matched whole-word, case-insensitive — same robustness contract as the
// "boost" token. Keep the set small and standalone; a model whose display name needs one of these
// words should use "chN" (or rename) so it isn't mistaken for a channel. Returns nullptr if no match.
struct PreampChannelColour { const char* word; int index; juce::uint32 argb; };
inline const PreampChannelColour* findPreampChannelColour (const juce::String& wordLower)
{
    static const PreampChannelColour table[] = {
        { "red",    1, 0xffe0524e },
        { "green",  2, 0xff57c06a },
        { "blue",   3, 0xff4e8fe0 },
        { "yellow", 4, 0xffe0c64e },
        { "orange", 5, 0xffe08a4e },
        { "purple", 6, 0xff9a6ae0 },
        { "white",  7, 0xffd8d8e0 },
    };
    for (const auto& c : table)
        if (wordLower == c.word) return &c;
    return nullptr;
}

// Split a base filename into (display name, channel + its label/colour, hours, boost) on tokens
// separated by space / dash / underscore. Tokens recognised (any case, whole-word) and dropped from
// the display name:
//   "chN" (N=1..4) → channel;  a colour word (red/green/…) → channel by colour;  "<N>h" → gain hours;
//   "boost" → boost on.
// No channel token → 0 (empty label, no colour); no <N>h → hours 0; no "boost" → false. If the tokens
// were the whole name, the base is kept so there's always something to show.
//   "Voltage ch2 12h boost" → name "Voltage", channel 2 ("Ch 2"),       hours 12, boost true
//   "V4KRAK red 9h"           → name "V4KRAK",    channel 1 ("Red", glow),  hours 9
//   "Vortex 9h"             → name "Vortex",  channel 0,                hours 9,  boost false
inline void parsePreampName (const juce::String& base, juce::String& nameOut,
                             int& channelOut, int& hoursOut, bool& boostOut,
                             juce::String& channelLabelOut, juce::uint32& channelColourOut)
{
    channelOut       = 0;
    channelLabelOut  = {};
    channelColourOut = 0;
    hoursOut         = 0;
    boostOut         = false;
    nameOut          = base;   // default: no tags → name unchanged. Set up front so a reused out-string
                               // can't return stale (mirrors parsePowerampName's contract).
    juce::StringArray words;
    words.addTokens (base, " -_", "");
    words.removeEmptyStrings();

    // channel tag — "chN" (N = 1..4) OR a colour word (red/green/…); dropped from the display name.
    // The first channel-type token wins; chN and colours are mutually exclusive within a name.
    for (int i = 0; i < words.size(); ++i)
    {
        const auto w = words[i].toLowerCase();
        if (w.length() == 3 && w.startsWith ("ch")
            && w.getLastCharacter() >= (juce::juce_wchar) '1' && w.getLastCharacter() <= (juce::juce_wchar) '4')
        {
            channelOut      = w.getLastCharacter() - (juce::juce_wchar) '0';
            channelLabelOut = "Ch " + juce::String (channelOut);
            words.remove (i);
            break;
        }
        if (const auto* c = findPreampChannelColour (w))
        {
            channelOut       = c->index;
            channelColourOut = c->argb;
            channelLabelOut  = w.substring (0, 1).toUpperCase() + w.substring (1);   // "red" → "Red"
            words.remove (i);
            break;
        }
    }

    // gain tag "<N>h" (7h…17h) → hours; dropped from the display name.
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

    // boost tag → boost on; dropped from the display name.
    for (int i = 0; i < words.size(); ++i)
        if (words[i].equalsIgnoreCase ("boost"))
        {
            boostOut = true;
            words.remove (i);
            break;
        }

    const auto joined = words.joinIntoString (" ").trim();
    if (joined.isNotEmpty())
        nameOut = joined;   // else keep base (the tags were the whole name)
}

// Back-compat overload for callers that don't need the channel label/colour (e.g. the unit tests that
// assert only name/channel/hours/boost). Forwards to the full parser and discards the display extras.
inline void parsePreampName (const juce::String& base, juce::String& nameOut,
                             int& channelOut, int& hoursOut, bool& boostOut)
{
    juce::String  label;
    juce::uint32  colour;
    parsePreampName (base, nameOut, channelOut, hoursOut, boostOut, label, colour);
}

inline std::vector<PreampEntry> scanPreampLibrary (const juce::File& dir)
{
    std::vector<PreampEntry> out;
    if (! dir.isDirectory())
        return out;

    for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.nam"))
    {
        PreampEntry e;
        e.factory = false;
        e.file    = f;
        const auto base = f.getFileNameWithoutExtension();
        e.id = "up:" + base;
        parsePreampName (base, e.name, e.channel, e.hours, e.boost, e.channelLabel, e.channelColour);
        out.push_back (std::move (e));
    }
    // Sort by name, then by the sub-dimensions, so a group's variants list deterministically.
    std::sort (out.begin(), out.end(), [] (const PreampEntry& a, const PreampEntry& b)
    {
        if (const int c = a.name.compareIgnoreCase (b.name); c != 0) return c < 0;
        if (a.channel != b.channel) return a.channel < b.channel;
        if (a.hours   != b.hours)   return a.hours   < b.hours;
        return (int) a.boost < (int) b.boost;
    });
    return out;
}

} // namespace orbitcab
