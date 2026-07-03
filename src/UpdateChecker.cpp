// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "UpdateChecker.h"

#include <utility>           // std::exchange

namespace orbitcab
{

namespace
{
    constexpr int  kTimeoutMs     = 5000;   // short — opt-in, non-blocking
    // Join budget on destruction: the connect timeout is 5 s and cancel() aborts a blocked
    // connect/read promptly, so 8 s is generous headroom before stopThread force-kills (last resort).
    constexpr int  kJoinMs        = 8000;
    constexpr char kReleasesApi[] = "https://api.github.com/repos/darwinscat/orbitcab/releases/latest";
    constexpr char kKeyLatest[]   = "lastSeenLatest";
    constexpr char kKeyEpoch[]    = "lastCheckEpoch";
}

UpdateChecker::UpdateChecker (juce::String currentVersion, AppPreferences& prefsRef)
    : juce::Thread ("OrbitCab UpdateChecker"),
      current (std::move (currentVersion)), prefs (prefsRef)
{
    // Cross-process freshness: same-process instances share the PropertiesFile object (see
    // AppPreferences), but another HOST PROCESS may have stored a newer tag — pick it up once here.
    prefs.reload();

    // Badge-clear: if the installed version has caught up to (or passed) a previously seen
    // "latest", drop the stored value so no stale badge shows. The ONLY place the plugin
    // compares versions at ctor time.
    const juce::String seen = storedLatest();
    if (seen.isNotEmpty() && ! isNewer (seen, current))
        clearStored();
}

UpdateChecker::~UpdateChecker()
{
    // JOIN the worker before the plugin module can unload: signal, abort any blocking connect/read,
    // wait. Then drop an undelivered async result (the AsyncUpdater base asserts on pending updates).
    signalThreadShouldExit();
    {
        const juce::ScopedLock sl (streamLock);
        if (activeStream != nullptr)
            activeStream->cancel();                 // cross-thread-safe: unblocks connect/read promptly
    }
    stopThread (kJoinMs);
    cancelPendingUpdate();
}

juce::String UpdateChecker::storedLatest() const
{
    if (auto* s = prefs.file())
        return s->getValue (kKeyLatest);
    return {};
}

bool UpdateChecker::updateAvailable() const
{
    const juce::String seen = storedLatest();
    return seen.isNotEmpty() && isNewer (seen, current);
}

void UpdateChecker::storeOutdated (const juce::String& latest)
{
    if (auto* s = prefs.file())
    {
        s->setValue (kKeyLatest, latest);
        s->setValue (kKeyEpoch, juce::String (juce::Time::getCurrentTime().toMilliseconds()));
        s->saveIfNeeded();
    }
}

void UpdateChecker::clearStored()
{
    if (auto* s = prefs.file())
    {
        s->removeValue (kKeyLatest);
        s->saveIfNeeded();
    }
}

// Worker thread. Blocking; the stream is registered under streamLock while it can block so the
// destructor can cancel() it from another thread.
UpdateChecker::Result UpdateChecker::fetch()
{
    Result r;

    // GitHub's API rejects requests without a User-Agent (403); Accept + api-version are
    // best practice. Unauthenticated → 60 req/h per IP, ample for an opt-in manual check.
    const juce::String headers =
        "User-Agent: OrbitCab-UpdateChecker\r\n"
        "Accept: application/vnd.github+json\r\n"
        "X-GitHub-Api-Version: 2022-11-28";

    juce::WebInputStream web (juce::URL (kReleasesApi), false);
    web.withConnectionTimeout (kTimeoutMs)
       .withNumRedirectsToFollow (3)
       .withExtraHeaders (headers);

    {
        const juce::ScopedLock sl (streamLock);
        if (threadShouldExit())
            return r;                               // shutting down — never even start the request
        activeStream = &web;                        // publish for cross-thread cancel()
    }

    const bool connected = web.connect (nullptr);   // no network / DNS / TLS failure → false
    juce::String body;
    if (connected && web.getStatusCode() == 200)    // 404 = no releases yet, 403 = rate-limited, etc.
        body = web.readEntireStreamAsString();

    {
        const juce::ScopedLock sl (streamLock);
        activeStream = nullptr;                     // retire BEFORE the stack-scope stream dies
    }

    if (threadShouldExit() || body.isEmpty())
        return r;

    const juce::var json = juce::JSON::parse (body);
    if (! json.isObject())
        return r;

    // /releases/latest already excludes drafts + pre-releases, so tag_name is a real,
    // numeric vX.Y.Z (D22). Strip the leading 'v'; unlike the old server, the plugin
    // itself decides "outdated" by comparing that tag to the installed version.
    juce::String tag = json.getProperty ("tag_name", juce::var()).toString().trim();
    if (tag.startsWithIgnoreCase ("v"))
        tag = tag.substring (1);
    if (tag.isEmpty())
        return r;

    r.ok       = true;
    r.latest   = tag;
    r.url      = json.getProperty ("html_url", juce::var()).toString();   // the release page
    r.notes    = json.getProperty ("name",     juce::var()).toString();   // release title (optional)
    r.outdated = isNewer (r.latest, current);
    return r;
}

void UpdateChecker::run()
{
    const Result r = fetch();
    if (threadShouldExit())
        return;                                     // shutting down — drop the result silently
    pending = r;                                    // visible to handleAsyncUpdate: the message post synchronizes
    triggerAsyncUpdate();
}

void UpdateChecker::handleAsyncUpdate()
{
    // Message thread. Persist first (PropertiesFile + UI are not RT/thread-safe), then notify the badge.
    // A successful check is the freshest truth: write the badge up when a newer release exists, and
    // clear it when it doesn't — otherwise a stored "latest" that the server later retracts (a yanked
    // release) would nag forever, since only a version bump clears it (ctor). A FAILED check (ok=false)
    // touches nothing, so an offline click never wipes a real badge.
    if (pending.ok && pending.outdated && pending.latest.isNotEmpty())
        storeOutdated (pending.latest);
    else if (pending.ok)
        clearStored();

    if (auto cb = std::exchange (onDone, nullptr))
        cb (pending);
}

void UpdateChecker::checkNow (std::function<void (Result)> cb)
{
    JUCE_ASSERT_MESSAGE_THREAD
    onDone = std::move (cb);
    // Also gate on isUpdatePending(): between run() calling triggerAsyncUpdate() and exiting, and
    // handleAsyncUpdate() actually firing, isThreadRunning() is already false while the result is
    // still queued. Without this a second click there would spin up a 2nd worker that races `pending`;
    // instead we just keep the replaced callback and deliver the in-flight result to it (last click wins).
    if (! isThreadRunning() && ! isUpdatePending())
        startThread();                              // a finished, fully-delivered worker restarts cleanly
}

bool UpdateChecker::isNewer (const juce::String& latest, const juce::String& current)
{
    auto triple = [] (const juce::String& v, int out[3])
    {
        auto parts = juce::StringArray::fromTokens (v, ".", "");
        for (int i = 0; i < 3; ++i)
            out[i] = i < parts.size() ? parts[i].getIntValue() : 0;
    };
    int a[3], b[3];
    triple (latest, a);
    triple (current, b);
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i])
            return a[i] > b[i];
    return false;
}

} // namespace orbitcab
