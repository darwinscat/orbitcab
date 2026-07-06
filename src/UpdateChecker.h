// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_events/juce_events.h>                    // AsyncUpdater (message-thread delivery)
#include <juce_data_structures/juce_data_structures.h>  // PropertiesFile (pulls juce_core: Thread, URL, WebInputStream, JSON)
#include "AppPreferences.h"                             // the shared global PropertiesFile owner
#include <functional>

//==============================================================================
// orbitcab::UpdateChecker — opt-in update check. Queries GitHub Releases ONLY when
// checkNow() is called from a user click — NEVER silently. The plugin reads the latest
// release tag and compares it to the installed version itself. Result is persisted in a
// global PropertiesFile so the "update available" badge survives restarts and is shared
// across all instances of the plugin (see AppPreferences: SharedResourcePointer).
//
// Contract:
//   GET https://api.github.com/repos/darwinscat/orbitcab/releases/latest
//   200 -> { tag_name: "vX.Y.Z", html_url, name, ... } — we read tag_name (strip 'v'),
//          compare it to the installed version, and expose html_url as the download page.
//   404 = no releases yet, 403 = rate-limited; any non-200 / timeout / no network
//   => silent no-op (this is an opt-in, non-blocking check).
//
// Threading: the worker is an OWNED juce::Thread base, so destruction JOINS it —
// signalThreadShouldExit() + WebInputStream::cancel() abort a blocking connect/read
// promptly, then stopThread() waits. This is the crash-safe replacement for a detached
// juce::Thread::launch reaching back via a cross-thread WeakReference, which could keep
// executing after the host unloaded the plugin module. The result is handed to the message
// thread via AsyncUpdater (cancelPendingUpdate() in the destructor drops an undelivered
// one). Hosts destroy the processor on the message thread (JUCE wrapper guarantee), so a
// delivered callback can't race destruction.
//
// Lives in the adapter layer (host glue: network + storage). The cab:: DSP core is
// untouched. RT rule: the network call runs on the owned background thread — never on the
// audio thread.
//==============================================================================
namespace orbitcab
{

class UpdateChecker : private juce::Thread,
                      private juce::AsyncUpdater
{
public:
    UpdateChecker (juce::String currentVersion, AppPreferences& prefs);   // e.g. JucePlugin_VersionString
    ~UpdateChecker() override;

    struct Result
    {
        bool         ok       = false;   // got a usable 200 response
        bool         outdated = false;   // server says a newer version exists
        juce::String latest;             // e.g. "1.1.0"   (text only)
        juce::String url;                // download page  (where to send the user)
        juce::String notes;              // optional human note (empty if absent)
    };

    juce::String currentVersion() const { return current; }

    // Fire the check on the owned background worker; `onDone` is invoked on the message
    // thread with the result (ok=false on any failure — silent, opt-in). MESSAGE THREAD
    // ONLY. If a check is already in flight, the new callback replaces the pending one
    // (last click wins) — the in-flight result is delivered to it.
    void checkNow (std::function<void (Result)> onDone);

    // Badge state: a stored "latest" that is newer than the installed version.
    bool         updateAvailable() const;
    juce::String storedLatest()    const;

private:
    void run() override;                // worker: the blocking fetch, then triggerAsyncUpdate
    void handleAsyncUpdate() override;  // message thread: persist + notify
    Result fetch();                     // worker-only: registers the cancellable stream while blocking

    void storeOutdated (const juce::String& latest);
    void clearStored();
    static bool isNewer (const juce::String& latest, const juce::String& current);

    juce::String current;
    AppPreferences& prefs;                                  // the shared global PropertiesFile owner

    std::function<void (Result)> onDone;                    // message thread only (checkNow / handleAsyncUpdate)
    Result pending;                                         // written by the worker before triggerAsyncUpdate

    // The in-flight stream, registered so the destructor can abort a blocking connect/read from
    // another thread (WebInputStream::cancel is documented cross-thread-safe). Guarded by streamLock:
    // the worker only publishes/retires the pointer under the lock, so cancel() can never race the
    // stream's stack-scope destruction.
    juce::CriticalSection streamLock;
    juce::WebInputStream* activeStream = nullptr;           // guarded by streamLock

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};

} // namespace orbitcab
