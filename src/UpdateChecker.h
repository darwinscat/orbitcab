// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_events/juce_events.h>                    // MessageManager (callAsync)
#include <juce_data_structures/juce_data_structures.h>  // ApplicationProperties / PropertiesFile (pulls juce_core: URL, WebInputStream, JSON, Thread)
#include <functional>

//==============================================================================
// orbitcab::UpdateChecker — opt-in update check. Queries the live
// version controller ONLY when checkNow() is called from a user
// click — NEVER silently. The plugin does not compare versions; the server returns
// `outdated` (bool) and the plugin obeys. Result is persisted in a global
// PropertiesFile so the "update available" badge survives restarts and is shared
// across all instances of the plugin.
//
// Contract (prod):
//   GET https://darwinscat.com/api/latest?product=orbitcab&v=<M.M.P>
//   200 -> { schema, latest, outdated, url, notes? }   (notes optional)
//   400 = bad version, 404 = unknown product. Any non-200 / timeout / no network
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
    explicit UpdateChecker (juce::String currentVersion);   // e.g. JucePlugin_VersionString

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
    juce::PropertiesFile* settings();
    void storeOutdated (const juce::String& latest);
    void clearStored();
    static Result fetch (const juce::String& version);      // blocking, no instance state — background thread only
    static bool isNewer (const juce::String& latest, const juce::String& current);

    juce::String current;
    juce::ApplicationProperties props;

    // The network reply lands on the message thread via callAsync; if this checker (i.e.
    // the plugin instance) was destroyed meanwhile, the weak ref makes the callback a no-op.
    JUCE_DECLARE_WEAK_REFERENCEABLE (UpdateChecker)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};

} // namespace orbitcab
