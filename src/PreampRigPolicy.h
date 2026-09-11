// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <namz_rig.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

//==============================================================================
// orbitcab::rigpolicy — THE PREAMP DEVICE POLICY, OWNED HERE: how a library of capture files becomes devices
// with switches, where a device lands, and which file a turned switch plays.
//
// It is the OrbitCab grammar and OrbitCab's selection rules, and until now it lived in namz (namz_rig.h —
// "generalized from OrbitCab's PreampSelector"). namz stopped carrying it: v2.0.0 dropped the filename-token
// fallback that EVERY factory preamp is named in (GtrVolt-blue-07h-boost, V4KRAK-red-12h, …), and v4 moved the
// gain default from the clock's noon to a midpoint in degrees, where "12h" is not a number and the FIRST value
// wins. Built against a core that pins namz v4, the shipped library stopped forming devices at all.
//
// So the policy comes home as namz v1.1.1 stated it — the last namz this plugin shipped against — with its logic
// unchanged (namz v1.1.1, include/namz_rig.h:143-448; MIT, same authors). Only the form differs: names qualified,
// the two string helpers it shared with the rest of that header declared here, the structs it builds filled field
// by field. And it is PROVED equal: the golden rendering in tests/PreampRigGoldenTests.cpp holds every answer of
// the device model to the one `main` gave on core v0.13.1, on every core the tree builds against.
//
// What stays with namz is the FORMAT: the types (FileMeta, FileEntry, Device, Control, Role, Settings — v4 only
// appended fields to them, which this file never touches) and parseControlsSpec(), the reader of the `controls`
// metadata key that namz owns as the capture convention. What namz decides about a DEVICE, this plugin no longer
// asks it.
//==============================================================================

namespace orbitcab::rigpolicy
{

namespace detail
{
    inline std::string trim (const std::string& s)
    {
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace ((unsigned char) s[a])) ++a;
        while (b > a && std::isspace ((unsigned char) s[b - 1])) --b;
        return s.substr (a, b - a);
    }

    inline std::string lower (std::string s)
    {
        std::transform (s.begin(), s.end(), s.begin(),
                        [] (unsigned char c) { return (char) std::tolower (c); });
        return s;
    }

    //--- the legacy filename grammar: the fallback for files without metadata --------------------------------
    inline bool isColour (const std::string& l)
    {
        for (const char* c : { "red", "green", "blue", "yellow", "orange", "purple", "white" })
            if (l == c) return true;
        return false;
    }

    inline bool isChN (const std::string& l)
    {
        return l.size() == 3 && l.rfind ("ch", 0) == 0 && l[2] >= '1' && l[2] <= '4';
    }

    inline bool isClock (const std::string& l)
    {
        if (l.size() < 2 || l.size() > 3 || l.back() != 'h') return false;
        for (std::size_t i = 0; i + 1 < l.size(); ++i)
            if (! std::isdigit ((unsigned char) l[i])) return false;
        return true;
    }

    inline int clockHours (const std::string& v)   // "07h" → 7; non-clock → -1
    {
        const auto l = lower (trim (v));
        return isClock (l) ? std::stoi (l.substr (0, l.size() - 1)) : -1;
    }

    inline bool isTopology (const std::string& l) { return l == "pp" || l == "se"; }

    inline bool isFalsy (const std::string& l)
    {
        return l == "off" || l == "no" || l == "false" || l == "0" || l.empty();
    }

    // Split a basename on the token separators (space/dash/underscore).
    inline std::vector<std::string> tokens (const std::string& base)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : base)
        {
            if (ch == ' ' || ch == '-' || ch == '_')
            {
                if (! cur.empty()) { out.push_back (cur); cur.clear(); }
            }
            else cur += ch;
        }
        if (! cur.empty()) out.push_back (cur);
        return out;
    }

    // Synthesize settings from filename tokens; returns the family (the leftover words).
    inline std::string legacyParse (const std::string& base, namz::rig::Settings& s)
    {
        std::string family;
        bool boostSeen = false;
        for (const auto& tok : tokens (base))
        {
            const auto l = lower (tok);
            if (isColour (l) || isChN (l)) s["channel"] = l;
            else if (isClock (l))          s["gain"] = l;
            else if (l == "boost")         { s["boost"] = "on"; boostSeen = true; }
            else if (isTopology (l))       s["topology"] = tok;
            else family += (family.empty() ? "" : " ") + tok;
        }
        if (! boostSeen) s["boost"] = "off";
        return family.empty() ? base : family;
    }

    // True when EVERY token of the basename is a recognized control token (no leftover name word) —
    // buildDevices then groups such files together instead of making each its own device.
    inline bool legacyIsAllTokens (const std::string& base)
    {
        const auto ts = tokens (base);
        if (ts.empty()) return false;
        for (const auto& tok : ts)
        {
            const auto l = lower (tok);
            if (! (isColour (l) || isChN (l) || isClock (l) || l == "boost" || isTopology (l)))
                return false;
        }
        return true;
    }

    inline void addValue (std::vector<std::string>& values, const std::string& v)
    {
        if (std::find (values.begin(), values.end(), v) == values.end()) values.push_back (v);
    }

    // Built field by field rather than brace-initialised: namz v4 appended members to both types, and a
    // brace list naming only the old ones is a -Wmissing-field-initializers warning on that version.
    inline namz::rig::Control control (const char* name, namz::rig::Role role, std::vector<std::string> values)
    {
        namz::rig::Control c;
        c.name   = name;
        c.role   = role;
        c.values = std::move (values);
        return c;
    }

    inline namz::rig::FileEntry fileEntry (const std::string& id, namz::rig::Settings settings)
    {
        namz::rig::FileEntry fe;
        fe.id       = id;
        fe.settings = std::move (settings);
        return fe;
    }
} // namespace detail

// Per-control default: noon-most gain (the clock grammar's "12h"), falsy boost, first value otherwise.
inline std::string defaultValue (const namz::rig::Control& c)
{
    if (c.values.empty()) return {};
    if (c.role == namz::rig::Role::Gain)
    {
        std::string best = c.values.front();
        int bestD = 1 << 30;
        for (const auto& v : c.values)
            if (const int h = detail::clockHours (v); h >= 0 && std::abs (h - 12) < bestD)
            {
                bestD = std::abs (h - 12);
                best = v;
            }
        return best;
    }
    if (c.role == namz::rig::Role::Boost)
        for (const auto& v : c.values)
            if (detail::isFalsy (detail::lower (detail::trim (v)))) return v;
    return c.values.front();
}

inline namz::rig::Settings defaultSettings (const namz::rig::Device& d)
{
    namz::rig::Settings s;
    // Qualified on purpose: an unqualified call finds namz::rig::defaultValue too, by argument-dependent lookup
    // on the namz type — and namz's answers a different question now (degrees). Never let the lookup choose.
    for (const auto& c : d.controls) s[c.name] = orbitcab::rigpolicy::defaultValue (c);
    return s;
}

// Build devices from a set of files. Grouping key: `rig_id` when stamped, else the family name (metadata
// `gear_model`, else the filename leftovers). Meta-driven files take their control spec verbatim (first spec
// seen wins); legacy files grow a channel/gain/boost/topology control per dimension that shows ≥2 distinct
// values (single-valued dimensions stay invisible — OrbitCab's rule), with gains sorted by the clock and boost
// normalized to off|on.
inline std::vector<namz::rig::Device> buildDevices (const std::vector<namz::rig::FileMeta>& files)
{
    struct Acc
    {
        namz::rig::Device d;
        bool metaDriven = false;
        std::string gearModel;                             // to re-merge files that omit rig_id
        std::vector<std::string> channels, gains, topos;   // legacy dimension unions, insertion order
        bool anyBoostOn = false, anyBoostOff = false;
    };
    std::vector<Acc> accs;

    // Group key priority: a stamped rig_id, else the gear_model / family name. A file that omits rig_id but
    // shares a family's gear_model MERGES into it (rig_id grouping "survives" inconsistent stamping).
    auto accFor = [&accs] (const std::string& rid, const std::string& gearModel,
                           const std::string& family) -> Acc& {
        for (auto& a : accs)
        {
            if (! rid.empty() && a.d.rigId == rid) return a;                     // same explicit id
            if (rid.empty() && ! a.d.rigId.empty() && ! gearModel.empty()
                && a.gearModel == gearModel) return a;                           // adopt into the id'd acc
            if (rid.empty() && a.d.rigId.empty() && a.d.family == family) return a;
        }
        accs.emplace_back();
        accs.back().d.rigId   = rid;
        accs.back().gearModel = gearModel;
        accs.back().d.family  = family;
        return accs.back();
    };

    for (const auto& f : files)
    {
        const auto metaAt = [&f] (const char* k) {
            const auto it = f.meta.find (k);
            return it == f.meta.end() ? std::string() : it->second;
        };
        const auto spec = metaAt ("controls");
        if (! spec.empty())
        {
            namz::rig::Settings s;
            for (const auto& [k, v] : f.meta)
                if (k.rfind ("settings.", 0) == 0) s[k.substr (9)] = v;
            const auto gm = metaAt ("gear_model");
            const auto family = ! gm.empty() ? gm : f.filenameBase;
            const auto rid = metaAt ("rig_id");
            auto& a = accFor (rid, gm, family);
            a.metaDriven = true;
            if (a.d.rigId.empty() && ! rid.empty()) a.d.rigId = rid;   // a later id'd file names the group
            if (a.gearModel.empty()) a.gearModel = gm;
            if (a.d.slot.empty())
                a.d.slot = ! metaAt ("slot").empty() ? metaAt ("slot") : metaAt ("gear_type");
            if (a.d.controls.empty()) a.d.controls = namz::rig::parseControlsSpec (spec);
            a.d.files.push_back (detail::fileEntry (f.id, std::move (s)));
            continue;
        }
        // Legacy filename fallback. When the basename is ALL tokens (no leftover name), group such files under
        // one key so a nameless legacy pack still forms ONE device (else every combination splits into its own
        // control-less device).
        namz::rig::Settings s;
        std::string family = detail::legacyParse (f.filenameBase, s);
        std::string groupFamily = family;
        if (family == f.filenameBase && detail::legacyIsAllTokens (f.filenameBase))
            groupFamily = std::string();                              // token-only: shared empty key
        auto& a = accFor (std::string(), std::string(), groupFamily);
        detail::addValue (a.channels, s.count ("channel") ? s["channel"] : std::string());
        detail::addValue (a.gains,    s.count ("gain") ? s["gain"] : std::string());
        detail::addValue (a.topos,    s.count ("topology") ? s["topology"] : std::string());
        (s["boost"] == "on" ? a.anyBoostOn : a.anyBoostOff) = true;
        a.d.files.push_back (detail::fileEntry (f.id, std::move (s)));
    }

    // The settings-trim keeps files matchable against the visible controls — needed for BOTH paths (meta files
    // carry settings.* for controls the group may not expose; without trimming, find() and defaults miss and
    // resolve() leaks stray keys).
    auto trimToControls = [] (namz::rig::Device& d) {
        for (auto& fe : d.files)
        {
            namz::rig::Settings kept;
            for (const auto& c : d.controls)
                if (const auto it = fe.settings.find (c.name); it != fe.settings.end()) kept[c.name] = it->second;
            fe.settings = std::move (kept);
        }
    };

    std::vector<namz::rig::Device> out;
    for (auto& a : accs)
    {
        if (! a.metaDriven)
        {
            auto strip = [] (std::vector<std::string>& v) { v.erase (std::remove (v.begin(), v.end(), std::string()), v.end()); };
            strip (a.channels); strip (a.gains); strip (a.topos);
            std::sort (a.gains.begin(), a.gains.end(),
                       [] (const std::string& x, const std::string& y) { return detail::clockHours (x) < detail::clockHours (y); });
            if (a.channels.size() > 1) a.d.controls.push_back (detail::control ("channel", namz::rig::Role::Channel, a.channels));
            if (a.topos.size() > 1)    a.d.controls.push_back (detail::control ("topology", namz::rig::Role::Topology, a.topos));
            if (a.anyBoostOn && a.anyBoostOff) a.d.controls.push_back (detail::control ("boost", namz::rig::Role::Boost, { "off", "on" }));
            if (a.gains.size() > 1)    a.d.controls.push_back (detail::control ("gain", namz::rig::Role::Gain, a.gains));
        }
        trimToControls (a.d);
        out.push_back (std::move (a.d));
    }
    return out;
}

// The user changed `changed` to `value`: pin it, keep everything else where it is, fall back to the closest
// captured combination. `settings` is updated to the CHOSEN file's real combination. Returns nullptr for a
// device with no files, and when no file carries the turned value (the pin is honoured, never contradicted).
inline const namz::rig::FileEntry* resolve (const namz::rig::Device& d, namz::rig::Settings& settings,
                                            const std::string& changed, const std::string& value)
{
    if (d.files.empty()) return nullptr;
    const bool hadChanged = settings.count (changed) != 0;
    const std::string prevChanged = hadChanged ? settings.at (changed) : std::string();
    settings[changed] = value;
    if (const auto* exact = d.find (settings)) return exact;

    const auto defaults = orbitcab::rigpolicy::defaultSettings (d);   // qualified: see defaultSettings()
    const namz::rig::FileEntry* best = nullptr;
    long bestScore = -1, bestDef = -1;
    for (const auto& f : d.files)
    {
        const auto it = f.settings.find (changed);
        if (it == f.settings.end() || it->second != value) continue;   // the turned control is law
        long score = 0, defMatch = 0;
        for (const auto& c : d.controls)
        {
            if (c.name == changed) continue;
            const auto fv = f.settings.count (c.name) ? f.settings.at (c.name) : std::string();
            const auto rv = settings.count (c.name) ? settings.at (c.name) : std::string();
            const auto dv = defaults.count (c.name) ? defaults.at (c.name) : std::string();
            if (fv == rv) score += 4;                                  // keep what the user had
            else if (fv == dv) score += 1;                             // else prefer the default
            if (fv == dv) ++defMatch;
        }
        // Deterministic: higher score wins; on a TIE, prefer the file sitting on more defaults.
        if (score > bestScore || (score == bestScore && defMatch > bestDef))
        {
            bestScore = score; bestDef = defMatch; best = &f;
        }
    }
    if (best == nullptr)
    {
        // The turned value was never captured on this control. Honour the pin — do NOT return a file that
        // contradicts the user's turn; leave settings as they were and report "no such file".
        if (hadChanged) settings[changed] = prevChanged; else settings.erase (changed);
        return nullptr;
    }
    settings = best->settings;
    return best;
}

} // namespace orbitcab::rigpolicy
