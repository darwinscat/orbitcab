// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "UpdateChecker.h"

namespace orbitcab
{

namespace
{
    constexpr int  kTimeoutMs     = 5000;   // short — opt-in, non-blocking
    constexpr char kReleasesApi[] = "https://api.github.com/repos/darwinscat/orbitcab/releases/latest";
    constexpr char kKeyLatest[]   = "lastSeenLatest";
    constexpr char kKeyEpoch[]    = "lastCheckEpoch";
}

UpdateChecker::UpdateChecker (juce::String currentVersion, AppPreferences& prefsRef)
    : current (std::move (currentVersion)), prefs (prefsRef)
{
    // Badge-clear: if the installed version has caught up to (or passed) a previously
    // seen "latest", drop the stored value so no stale badge shows. The ONLY place the
    // plugin compares versions itself.
    const juce::String seen = storedLatest();
    if (seen.isNotEmpty() && ! isNewer (seen, current))
        clearStored();
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

UpdateChecker::Result UpdateChecker::fetch (const juce::String& version)
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
    if (! web.connect (nullptr))            // no network / DNS / TLS failure
        return r;
    if (web.getStatusCode() != 200)         // 404 = no releases yet, 403 = rate-limited, etc.
        return r;

    const juce::var json = juce::JSON::parse (web.readEntireStreamAsString());
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
    r.outdated = isNewer (r.latest, version);
    return r;
}

void UpdateChecker::checkNow (std::function<void (Result)> onDone)
{
    const juce::String ver = current;                       // capture by value (thread-safe)
    juce::WeakReference<UpdateChecker> weak (this);
    juce::Thread::launch ([weak, ver, onDone]
    {
        const Result r = fetch (ver);                       // static — touches no instance state
        // Store + notify on the message thread (PropertiesFile + UI are not RT/thread-safe).
        juce::MessageManager::callAsync ([weak, onDone, r]
        {
            if (auto* self = weak.get())                    // no-op if the plugin was removed meanwhile
                if (r.ok && r.outdated && r.latest.isNotEmpty())
                    self->storeOutdated (r.latest);
            if (onDone)
                onDone (r);
        });
    });
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
