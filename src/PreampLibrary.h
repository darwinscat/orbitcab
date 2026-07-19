// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <vector>

namespace orbitcab
{

//==============================================================================
// The preamp .nam/.namz library the editor's PREAMP selector lists — a sibling of the POWERAMP
// library (see PowerampLibrary.h), but for the SECOND neural stage that runs IN FRONT of the
// poweramp (input → PREAMP → POWERAMP → cab). Two-source merge:
//   • FACTORY — models embedded in the build from resources/preamps/ (gitignored content; a
//     public build ships none). They live in a SEPARATE binary-data namespace (PreampBinaryData)
//     so they never bleed into the poweramp's BinaryData enumeration. id = "fp:<base>".
//   • USER    — the system-wide, per-machine folder AppPreferences::preampDir(), shared across
//     instances, managed from the gear panel. id = "up:<base>"; `file` is valid.
// The "fp:"/"up:" prefixes are distinct from the poweramp's "f:"/"u:" so the two selections /
// pools never collide. `id` is the stable selection key persisted in session state.
//
// This header is the raw FILE layer only: the source list (scan + factory enumeration feed
// PreampSource rows) and the display entry the manager rows show. WHAT a family of files means —
// devices, controls, knob positions — is decided by orbitcab::PreampRig (PreampRig.h) on top of
// namz::rig: `controls`/`settings.*` metadata first, the legacy filename-token grammar (colour/chN
// channel, NNh gain, "boost") as the fallback for files without metadata.
//==============================================================================

// One file feeding the device model: the stable id, the filename stem (display fallback + the
// legacy-grammar input), and the file's .namz display metadata (left empty by the scan — the owner
// fills it via ocnam::readMeta, which knows how to reach embedded factory bytes too).
struct PreampSource
{
    juce::String          id;        // "fp:<base>" (factory) | "up:<base>" (user folder)
    juce::String          base;      // filename stem
    bool                  factory = false;
    juce::File            file;      // valid only when ! factory
    juce::StringPairArray meta;      // ocnam::readMeta output (empty → legacy filename fallback)
};

// One row of the library as the UI lists it (manager rows, byte lookups) — built by
// PreampRig::build from the device model, NOT by the scan: `name` is the device's display family,
// `variant` the file's knob-position badge ("Green · 12h · boost", "" for a plain single capture).
struct PreampEntry
{
    juce::String id;          // stable selection key: "fp:<base>" (factory) | "up:<base>" (user folder)
    juce::String name;        // display name — the device family this file belongs to
    juce::String variant;     // the file's position badge in control order ("" → a plain model)
    bool         factory = false; // true → embedded (PreampBinaryData); false → user folder
    juce::File   file;        // valid only when ! factory (the source file in preampDir)
};

// Recognised channel-colour words → a stable order index (clean → hi-gain presentation) + an ARGB
// tint the channel button glows in. Matched whole-word, case-insensitive. Keep the set small and
// standalone; a model whose display name needs one of these words should use "chN" (or rename) so
// it isn't mistaken for a channel. Returns nullptr if no match.
struct PreampChannelColour { const char* word; int index; juce::uint32 argb; };
inline const PreampChannelColour* findPreampChannelColour (const juce::String& wordLower)
{
    // index doubles as the channel PRESENTATION order (PreampRig ranks channel values by it, and
    // the device-switch default takes the lowest). Ordered green → orange → blue → red so a family
    // reads clean → crunch → hi-gain left-to-right (GtrVolt: green/orange/blue; V4: green then red).
    static const PreampChannelColour table[] = {
        { "green",  1, 0xff57c06a },
        { "orange", 2, 0xffe08a4e },
        { "blue",   3, 0xff4e8fe0 },
        { "red",    4, 0xffe0524e },
        { "yellow", 5, 0xffe0c64e },
        { "purple", 6, 0xff9a6ae0 },
        { "white",  7, 0xffd8d8e0 },
    };
    for (const auto& c : table)
        if (wordLower == c.word) return &c;
    return nullptr;
}

// Enumerate the USER folder's captures (raw .nam + packed .namz) as PreampSource rows, metadata
// left empty (the processor fills it — reading .namz headers needs ocnam, which lives above this
// juce_core-only layer). Sorted by stem so legacy grouping order is deterministic across rescans.
inline std::vector<PreampSource> scanPreampLibrary (const juce::File& dir)
{
    std::vector<PreampSource> out;
    if (! dir.isDirectory())
        return out;

    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.nam");      // raw captures
    files.addArray (dir.findChildFiles (juce::File::findFiles, false, "*.namz")); // + packed captures
    for (const auto& f : files)
    {
        PreampSource s;
        s.factory = false;
        s.file    = f;
        s.base    = f.getFileNameWithoutExtension();
        s.id      = "up:" + s.base;
        out.push_back (std::move (s));
    }
    std::sort (out.begin(), out.end(), [] (const PreampSource& a, const PreampSource& b)
    {
        return a.base.compareIgnoreCase (b.base) < 0;
    });
    return out;
}

} // namespace orbitcab
