// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "PreampLibrary.h"

#include <namz_rig.h>

#include <algorithm>
#include <vector>

namespace orbitcab
{

//==============================================================================
// orbitcab::PreampRig — the metadata-first PREAMP device model, replacing the filename-parsing
// PreampSelector. The grouping/selection brains live in namz::rig (namz_rig.h — the player-side
// twin of the capture conventions in NAMZ-FORMAT.md): files that carry `controls` + `settings.*`
// metadata in their .namz header become a device with REAL controls (any names, any values —
// generic switches included), and files without metadata fall back to the legacy filename-token
// grammar (colour/chN channel, NNh gain, "boost") INSIDE the lib, so every pre-convention library
// keeps working unchanged.
//
// This header is the thin juce_core adapter + editor policy around it:
//   • build()  — feed the merged factory+user file list (with each file's meta, read cheaply via
//                ocnam::readMeta at enumeration time) → devices + display entries. Entry ids stay
//                the stable "fp:<base>"/"up:<base>" keys persisted in session state ("preampSel").
//   • viewFor() — the editor's row: the selected device's controls IN CAPTURE ORDER, each visible
//                only when it offers ≥2 values (the OrbitCab rule); channel values are presented
//                in colour order (green → orange → blue → red — clean to hi-gain), chN numerically.
//   • resolveControl() / resolveDevice() — "the user turned ONE control / picked another device";
//                namz::rig pins the turned control, keeps every other where it is, and falls back
//                to the closest captured combination. A value no file carries resolves to "" (the
//                UI simply stays put) — sparse matrices never mis-select a contradicting file.
//
// No JUCE GUI — juce_core only — so it builds into the headless test target (PreampRigTests) and
// the editor stays a dumb binding layer over View/resolve, whatever controls a device brings.
//==============================================================================

namespace rigdetail
{
    inline std::string toStd (const juce::String& s)       { return s.toStdString(); }
    inline juce::String fromStd (const std::string& s)     { return juce::String (juce::CharPointer_UTF8 (s.c_str())); }

    inline bool isTruthyValue (const juce::String& v)
    {
        const auto l = v.trim().toLowerCase();
        return l == "on" || l == "yes" || l == "true" || l == "1" || l == "boost";
    }

    // Channel-value presentation order: a colour word ranks by the fixed clean→hi-gain colour
    // index, "chN" ranks numerically after the colours, anything else keeps capture order at the
    // end. Mirrors the old PreampSelector's sorted channel switch.
    inline int channelValueRank (const juce::String& v, int captureIndex)
    {
        const auto l = v.trim().toLowerCase();
        if (const auto* c = findPreampChannelColour (l)) return c->index;
        if (l.length() == 3 && l.startsWith ("ch") && juce::CharacterFunctions::isDigit (l[2]))
            return 100 + (int) (l[2] - '0');
        return 1000 + captureIndex;
    }
} // namespace rigdetail

// "green" → "Green" (+ its glow colour), "ch2" → "Ch 2", anything else verbatim ("+", "mid", …).
inline juce::String channelValueLabel (const juce::String& v)
{
    const auto l = v.trim().toLowerCase();
    if (findPreampChannelColour (l) != nullptr)
        return l.substring (0, 1).toUpperCase() + l.substring (1);
    if (l.length() == 3 && l.startsWith ("ch") && juce::CharacterFunctions::isDigit (l[2]))
        return "Ch " + l.substring (2);
    return v;
}

inline juce::uint32 channelValueColour (const juce::String& v)
{
    const auto* c = findPreampChannelColour (v.trim().toLowerCase());
    return c != nullptr ? c->argb : 0;
}

struct PreampRig
{
    std::vector<PreampEntry>       entries;   // display list (manager rows, byte lookups) — device order
    std::vector<namz::rig::Device> devices;   // the selectable matrices (files reference entry ids)

    //--- build ----------------------------------------------------------------
    // Feed the merged factory+user list (factory first — a device's combo section follows its
    // FIRST file's source, as before). Meta-driven and legacy files group inside namz::rig; the
    // returned entries carry the device's display family + a per-file variant badge.
    void build (const std::vector<PreampSource>& files)
    {
        entries.clear();
        devices.clear();

        std::vector<namz::rig::FileMeta> fm;
        fm.reserve (files.size());
        for (const auto& f : files)
        {
            namz::rig::FileMeta m;
            m.id           = rigdetail::toStd (f.id);
            m.filenameBase = rigdetail::toStd (f.base);
            for (const auto& k : f.meta.getAllKeys())
                m.meta[rigdetail::toStd (k)] = rigdetail::toStd (f.meta[k]);
            fm.push_back (std::move (m));
        }
        devices = namz::rig::buildDevices (fm);

        // Entries in device order (manager rows group naturally). Name = the device family (first
        // file's base when a token-only legacy pack left it empty); variant = the file's settings
        // badge in control order ("Green · 12h · boost").
        for (const auto& d : devices)
        {
            for (const auto& fe : d.files)
            {
                const auto id = rigdetail::fromStd (fe.id);
                const auto* src = sourceFor (files, id);
                if (src == nullptr)
                    continue;   // buildDevices never invents ids — defensive only
                PreampEntry e;
                e.id      = id;
                e.factory = src->factory;
                e.file    = src->file;
                e.name    = ! d.family.empty() ? rigdetail::fromStd (d.family) : src->base;
                e.variant = variantBadge (d, fe);
                entries.push_back (std::move (e));
            }
        }
    }

    //--- queries --------------------------------------------------------------
    const PreampEntry* entryById (const juce::String& id) const
    {
        for (const auto& e : entries) if (e.id == id) return &e;
        return nullptr;
    }

    // The device holding this file id (index into `devices`), or -1.
    int deviceIndexForId (const juce::String& id) const
    {
        const auto sid = rigdetail::toStd (id);
        for (int i = 0; i < (int) devices.size(); ++i)
            for (const auto& fe : devices[(size_t) i].files)
                if (fe.id == sid) return i;
        return -1;
    }

    bool isGroup (const namz::rig::Device& d) const { return d.files.size() >= 2; }

    // A stable per-device key for the combo rows: the stamped rig_id when present, else the family
    // name — survives a library rescan (indices don't).
    juce::String deviceKey (const namz::rig::Device& d) const
    {
        return ! d.rigId.empty() ? "rig:" + rigdetail::fromStd (d.rigId)
                                 : "fam:" + rigdetail::fromStd (d.family);
    }

    int deviceIndexForKey (const juce::String& key) const
    {
        for (int i = 0; i < (int) devices.size(); ++i)
            if (deviceKey (devices[(size_t) i]) == key) return i;
        return -1;
    }

    // First file id carrying `value` on `control` — the representative capture the editor reads
    // display metadata from (tone captions on channel buttons).
    juce::String fileForValue (const namz::rig::Device& d, const juce::String& control,
                               const juce::String& value) const
    {
        const auto c = rigdetail::toStd (control), v = rigdetail::toStd (value);
        for (const auto& fe : d.files)
            if (const auto it = fe.settings.find (c); it != fe.settings.end() && it->second == v)
                return rigdetail::fromStd (fe.id);
        return {};
    }

    //--- view model -----------------------------------------------------------
    struct ControlView
    {
        juce::String       name;      // the control's spec name (caption for generic switches)
        namz::rig::Role    role = namz::rig::Role::Generic;
        juce::StringArray  values;    // presentation order (channels colour-ranked, rest capture order)
        juce::String       current;   // the selected file's value ("" when the file omits it)
        bool               visible = false;   // ≥2 values → render (the OrbitCab rule)
    };

    struct View
    {
        bool                     group = false;   // ≥2 captures → contextual controls make sense
        int                      deviceIndex = -1;
        std::vector<ControlView> controls;        // device control order
    };

    View viewFor (const juce::String& id) const
    {
        View v;
        v.deviceIndex = deviceIndexForId (id);
        if (v.deviceIndex < 0)
            return v;
        const auto& d = devices[(size_t) v.deviceIndex];
        v.group = isGroup (d);

        const namz::rig::FileEntry* cur = nullptr;
        const auto sid = rigdetail::toStd (id);
        for (const auto& fe : d.files) if (fe.id == sid) { cur = &fe; break; }

        for (const auto& c : d.controls)
        {
            ControlView cv;
            cv.name = rigdetail::fromStd (c.name);
            cv.role = c.role;
            std::vector<std::pair<int, juce::String>> ranked;
            for (int i = 0; i < (int) c.values.size(); ++i)
            {
                const auto val = rigdetail::fromStd (c.values[(size_t) i]);
                ranked.push_back ({ c.role == namz::rig::Role::Channel
                                        ? rigdetail::channelValueRank (val, i) : i, val });
            }
            std::stable_sort (ranked.begin(), ranked.end(),
                              [] (const auto& a, const auto& b) { return a.first < b.first; });
            for (auto& [rank, val] : ranked) cv.values.add (val);
            if (cur != nullptr)
                if (const auto it = cur->settings.find (rigdetail::toStd (cv.name)); it != cur->settings.end())
                    cv.current = rigdetail::fromStd (it->second);
            cv.visible = cv.values.size() >= 2;
            v.controls.push_back (std::move (cv));
        }
        return v;
    }

    //--- resolution -----------------------------------------------------------
    // The user set `control` to `value`: pin it, keep the rest, closest captured combination wins
    // (namz::rig policy). "" when the current id is unknown or NO file carries that value (sparse
    // matrix) — the caller simply doesn't switch.
    juce::String resolveControl (const juce::String& currentId, const juce::String& control,
                                 const juce::String& value) const
    {
        const int di = deviceIndexForId (currentId);
        if (di < 0)
            return {};
        const auto& d = devices[(size_t) di];
        namz::rig::Settings s;
        const auto sid = rigdetail::toStd (currentId);
        for (const auto& fe : d.files) if (fe.id == sid) { s = fe.settings; break; }
        const auto* fe = namz::rig::resolve (d, s, rigdetail::toStd (control), rigdetail::toStd (value));
        return fe != nullptr ? rigdetail::fromStd (fe->id) : juce::String();
    }

    // The user picked another device: keep every control value that also exists there, default the
    // rest (mirrors the old resolveName semantics, generalized to any control set).
    juce::String resolveDevice (const juce::String& currentId, int deviceIndex) const
    {
        if (! juce::isPositiveAndBelow (deviceIndex, (int) devices.size()))
            return {};
        const auto& d = devices[(size_t) deviceIndex];
        if (d.files.empty())
            return {};

        namz::rig::Settings prev;
        if (const int prevIdx = deviceIndexForId (currentId); prevIdx >= 0)
        {
            const auto sid = rigdetail::toStd (currentId);
            for (const auto& fe : devices[(size_t) prevIdx].files)
                if (fe.id == sid) { prev = fe.settings; break; }
        }

        namz::rig::Settings desired;
        for (const auto& c : d.controls)
        {
            const auto it = prev.find (c.name);
            const bool keeps = it != prev.end()
                               && std::find (c.values.begin(), c.values.end(), it->second) != c.values.end();
            desired[c.name] = keeps ? it->second : rigdetail::toStd (defaultForControl (c));
        }
        if (const auto* exact = d.find (desired))
            return rigdetail::fromStd (exact->id);
        if (! d.controls.empty())
        {
            // Pin the first control to its desired value and let the lib score the rest.
            auto s = desired;
            const auto& c0 = d.controls.front();
            if (const auto* fe = namz::rig::resolve (d, s, c0.name, desired[c0.name]))
                return rigdetail::fromStd (fe->id);
        }
        return rigdetail::fromStd (d.files.front().id);   // sparse edge: never "picked but nothing loads"
    }

private:
    static const PreampSource* sourceFor (const std::vector<PreampSource>& files, const juce::String& id)
    {
        for (const auto& f : files) if (f.id == id) return &f;
        return nullptr;
    }

    // Per-control default when landing on a device: channels take the PRESENTATION-first value
    // (lowest colour rank — green before an alphabetically-earlier "blue"), everything else the
    // lib's default (noon-most gain, falsy boost, first value).
    static juce::String defaultForControl (const namz::rig::Control& c)
    {
        if (c.role == namz::rig::Role::Channel && ! c.values.empty())
        {
            int bestRank = 1 << 30;
            juce::String best;
            for (int i = 0; i < (int) c.values.size(); ++i)
            {
                const auto v = rigdetail::fromStd (c.values[(size_t) i]);
                if (const int r = rigdetail::channelValueRank (v, i); r < bestRank)
                {
                    bestRank = r;
                    best = v;
                }
            }
            return best;
        }
        return rigdetail::fromStd (namz::rig::defaultValue (c));
    }

    // "Green · 12h · boost" — the file's position in control order. Boost contributes only when
    // truthy (absence reads as the clean capture, like the old badge); channels get their label.
    static juce::String variantBadge (const namz::rig::Device& d, const namz::rig::FileEntry& fe)
    {
        juce::StringArray parts;
        for (const auto& c : d.controls)
        {
            const auto it = fe.settings.find (c.name);
            if (it == fe.settings.end())
                continue;
            const auto v = rigdetail::fromStd (it->second);
            if (c.role == namz::rig::Role::Boost)
            {
                if (rigdetail::isTruthyValue (v)) parts.add ("boost");
            }
            else if (c.role == namz::rig::Role::Channel)
                parts.add (channelValueLabel (v));
            else
                parts.add (v);
        }
        return parts.joinIntoString (juce::String::fromUTF8 (" \xc2\xb7 "));
    }
};

} // namespace orbitcab
