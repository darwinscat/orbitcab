// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "PreampLibrary.h"

#include <algorithm>
#include <vector>

namespace orbitcab
{

//==============================================================================
// orbitcab::PreampSelector — the PURE policy behind the editor's PREAMP row: given a library
// snapshot + the currently-selected id, it answers two questions the GUI used to compute inline:
//
//   • RESOLUTION — "the user picked dimension value X; which entry id is now selected?" Switching
//     one dimension (name / channel / gain / boost) keeps the OTHER dimensions if they still exist
//     under the new choice, else falls back to a sensible default. Returns "" when nothing matches.
//   • VIEW — "for this selection, which contextual controls should the row show, with what values?"
//     A control appears only when the current choice has ≥2 values for that dimension.
//
// No JUCE GUI here — only juce_core (via PreampLibrary.h) — so it builds into the headless test
// target and is unit-tested directly (mirrors PreampLibrary.h / StateModel.h). The editor is a thin
// binding layer: it calls resolve*()/viewFor() and pushes the result onto the buttons/slider/combo.
// (The poweramp's equivalent logic still lives in the editor; this is the preamp extraction.)
//==============================================================================
struct PreampSelector
{
    std::vector<PreampEntry> lib;   // merged factory + user snapshot (rebuilt on add/remove)

    //--- queries --------------------------------------------------------------
    const PreampEntry* entryById (const juce::String& id) const
    {
        for (const auto& e : lib) if (e.id == id) return &e;
        return nullptr;
    }

    bool isGroupName (const juce::String& name) const   // ≥2 entries share this display name → a "family"
    {
        int n = 0;
        for (const auto& e : lib) if (e.name == name && ++n >= 2) return true;
        return false;
    }

    // Distinct display names that are groups, in first-seen order (the name-button row).
    std::vector<juce::String> groupNames() const
    {
        std::vector<juce::String> out;
        for (const auto& e : lib)
            if (isGroupName (e.name) && std::find (out.begin(), out.end(), e.name) == out.end())
                out.push_back (e.name);
        return out;
    }

    std::vector<int> channelsForName (const juce::String& name) const
    {
        std::vector<int> out;
        for (const auto& e : lib)
            if (e.name == name && std::find (out.begin(), out.end(), e.channel) == out.end())
                out.push_back (e.channel);
        std::sort (out.begin(), out.end());
        return out;
    }

    std::vector<int> gainsForNameChannel (const juce::String& name, int channel) const
    {
        std::vector<int> out;
        for (const auto& e : lib)
            if (e.name == name && e.channel == channel && std::find (out.begin(), out.end(), e.hours) == out.end())
                out.push_back (e.hours);
        std::sort (out.begin(), out.end());
        return out;
    }

    std::vector<bool> boostsForNameChGain (const juce::String& name, int channel, int hours) const
    {
        std::vector<bool> out;
        for (const auto& e : lib)
            if (e.name == name && e.channel == channel && e.hours == hours
                && std::find (out.begin(), out.end(), e.boost) == out.end())
                out.push_back (e.boost);
        std::sort (out.begin(), out.end());   // false (clean) before true (boost)
        return out;
    }

    juce::String findId (const juce::String& name, int channel, int hours, bool boost) const
    {
        for (const auto& e : lib)
            if (e.name == name && e.channel == channel && e.hours == hours && e.boost == boost) return e.id;
        return {};
    }

    //--- defaults (pick a value from the available candidates) ----------------
    // Gain: prefer noon (12h), else the middle of the available sweep, else 0 (no gain).
    static int defaultGain (const std::vector<int>& hrs)
    {
        if (hrs.empty()) return 0;
        for (int h : hrs) if (h == 12) return 12;
        return hrs[hrs.size() / 2];
    }
    // Channel: keep "none" (0) if that's all there is, else the lowest available channel.
    static int defaultChannel (const std::vector<int>& chs) { return chs.empty() ? 0 : chs.front(); }
    // Boost: prefer OFF (the cleaner capture) when available, else whatever's there.
    static bool defaultBoost (const std::vector<bool>& bs)
    {
        if (bs.empty()) return false;
        for (bool b : bs) if (! b) return false;
        return bs.front();
    }

    //--- resolution: switch ONE dimension, keep the rest if they still exist else default → new id =
    juce::String resolveName (const juce::String& currentId, const juce::String& name) const
    {
        const auto chs = channelsForName (name);
        if (chs.empty()) return {};
        int  channel  = defaultChannel (chs);
        int  curHours = 0;
        bool curBoost = false;
        if (auto* cur = entryById (currentId))
        {
            if (contains (chs, cur->channel)) channel = cur->channel;   // keep the current channel if it exists here
            curHours = cur->hours; curBoost = cur->boost;
        }
        const auto hrs   = gainsForNameChannel (name, channel);
        const int  hours = contains (hrs, curHours) ? curHours : defaultGain (hrs);
        const auto bs    = boostsForNameChGain (name, channel, hours);
        const bool boost = contains (bs, curBoost) ? curBoost : defaultBoost (bs);
        return findId (name, channel, hours, boost);
    }

    juce::String resolveChannel (const juce::String& currentId, int channel) const
    {
        auto* cur = entryById (currentId);
        if (cur == nullptr) return {};
        const auto hrs   = gainsForNameChannel (cur->name, channel);
        const int  hours = contains (hrs, cur->hours) ? cur->hours : defaultGain (hrs);
        const auto bs    = boostsForNameChGain (cur->name, channel, hours);
        const bool boost = contains (bs, cur->boost) ? cur->boost : defaultBoost (bs);
        return findId (cur->name, channel, hours, boost);
    }

    juce::String resolveGain (const juce::String& currentId, int hours) const
    {
        auto* cur = entryById (currentId);
        if (cur == nullptr) return {};
        const auto bs    = boostsForNameChGain (cur->name, cur->channel, hours);
        const bool boost = contains (bs, cur->boost) ? cur->boost : defaultBoost (bs);
        return findId (cur->name, cur->channel, hours, boost);
    }

    juce::String resolveBoost (const juce::String& currentId, bool boost) const
    {
        auto* cur = entryById (currentId);
        if (cur == nullptr) return {};
        return findId (cur->name, cur->channel, cur->hours, boost);
    }

    //--- view model: which contextual controls show, with what values, for a selection -----------
    struct View
    {
        bool             group        = false;   // the selected name is a family (≥2 variants)
        std::vector<int> channels;               // distinct channels of the family (sorted)
        bool             showChannels = false;   // ≥2 channels → render the channel switch
        int              currentChannel = 0;
        std::vector<int> gains;                  // gain stops for the current name+channel (sorted)
        bool             showGain     = false;   // ≥2 gains → render the gain slider
        int              currentGain  = 0;
        bool             showBoost    = false;   // both an on- and an off-capture exist → render the boost toggle
        bool             currentBoost = false;
    };

    View viewFor (const juce::String& currentId) const
    {
        View v;
        auto* cur = entryById (currentId);
        v.group = cur != nullptr && isGroupName (cur->name);
        if (! v.group)
            return v;                            // a singleton (or nothing) → no contextual controls

        v.channels       = channelsForName (cur->name);
        v.showChannels   = v.channels.size() >= 2;
        v.currentChannel = cur->channel;

        v.gains       = gainsForNameChannel (cur->name, cur->channel);
        v.showGain    = v.gains.size() >= 2;
        v.currentGain = cur->hours;

        const auto bs   = boostsForNameChGain (cur->name, cur->channel, cur->hours);
        v.showBoost     = contains (bs, false) && contains (bs, true);
        v.currentBoost  = cur->boost;
        return v;
    }

private:
    template <typename T>
    static bool contains (const std::vector<T>& v, const T& x)
    {
        return std::find (v.begin(), v.end(), x) != v.end();
    }
};

} // namespace orbitcab
