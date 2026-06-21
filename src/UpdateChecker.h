// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_events/juce_events.h>                    // MessageManager (callAsync)
#include <juce_data_structures/juce_data_structures.h>  // PropertiesFile (pulls juce_core: URL, WebInputStream, JSON, Thread)
#include "AppPreferences.h"                             // the shared global PropertiesFile owner
#include <functional>

//==============================================================================
// orbitcab::UpdateChecker — opt-in update check. Queries GitHub Releases ONLY when
// checkNow() is called from a user click — NEVER silently. The plugin reads the latest
// release tag and compares it to the installed version itself. Result is persisted in a
// global PropertiesFile so the "update available" badge survives restarts and is shared
// across all instances of the plugin.
//
// Contract:
//   GET https://api.github.com/repos/darwinscat/orbitcab/releases/latest
//   200 -> { tag_name: "vX.Y.Z", html_url, name, ... } — we read tag_name (strip 'v'),
//          compare it to the installed version, and expose html_url as the download page.
//   404 = no releases yet, 403 = rate-limited; any non-200 / timeout / no network
//   => silent no-op (this is an opt-in, non-blocking check).
//
// Lives in the adapter layer (host glue: network + storage). The cab:: DSP core is
// untouched. RT rule: the network call runs on a short-lived background thread — never
// on the audio thread.
//==============================================================================
namespace orbitcab
{

class UpdateChecker
{
public:
    UpdateChecker (juce::String currentVersion, AppPreferences& prefs);   // e.g. JucePlugin_VersionString

    struct Result
    {
        bool         ok       = false;   // got a usable 200 response
        bool         outdated = false;   // server says a newer version exists
        juce::String latest;             // e.g. "1.1.0"   (text only)
        juce::String url;                // download page  (where to send the user)
        juce::String notes;              // optional human note (empty if absent)
    };

    juce::String currentVersion() const { return current; }

    // Fire the check on a short-lived background thread; `onDone` is invoked on the
    // message thread with the result (ok=false on any failure — silent, opt-in).
    void checkNow (std::function<void (Result)> onDone);

    // Badge state: a stored "latest" that is newer than the installed version.
    bool         updateAvailable() const;
    juce::String storedLatest()    const;

private:
    void storeOutdated (const juce::String& latest);
    void clearStored();
    static Result fetch (const juce::String& version);      // blocking, no instance state — background thread only
    static bool isNewer (const juce::String& latest, const juce::String& current);

    juce::String current;
    AppPreferences& prefs;                                  // the shared global PropertiesFile owner

    // The network reply lands on the message thread via callAsync; if this checker (i.e.
    // the plugin instance) was destroyed meanwhile, the weak ref makes the callback a no-op.
    JUCE_DECLARE_WEAK_REFERENCEABLE (UpdateChecker)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};

} // namespace orbitcab
