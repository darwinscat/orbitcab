// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "UpdateChecker.h"

namespace orbitcab
{

namespace
{
    constexpr int  kTimeoutMs   = 5000;   // short — opt-in, non-blocking
    constexpr char kEndpoint[]  = "https://darwinscat.com/api/latest";
    constexpr char kProduct[]   = "orbitcab";   // product slug — must match the server's product id
    constexpr char kKeyLatest[] = "lastSeenLatest";
    constexpr char kKeyEpoch[]  = "lastCheckEpoch";
}

UpdateChecker::UpdateChecker (juce::String currentVersion)
    : current (std::move (currentVersion))
{
    // Global (NOT the DAW session) — shared across instances, survives restarts.
    juce::PropertiesFile::Options o;
    o.applicationName     = "OrbitCab";
    o.folderName          = "Darwin's Cat" + juce::String (juce::File::getSeparatorString()) + "OrbitCab";
    o.filenameSuffix      = "settings";
    o.osxLibrarySubFolder = "Application Support";
    props.setStorageParameters (o);

    // Badge-clear: if the installed version has caught up to (or passed) a previously
    // seen "latest", drop the stored value so no stale badge shows. The ONLY place the
    // plugin compares versions itself.
    const juce::String seen = storedLatest();
    if (seen.isNotEmpty() && ! isNewer (seen, current))
        clearStored();
}

juce::PropertiesFile* UpdateChecker::settings()
{
    return props.getUserSettings();
}

juce::String UpdateChecker::storedLatest() const
{
    // getUserSettings() is non-const; this read is logically const.
    if (auto* s = const_cast<UpdateChecker*> (this)->settings())
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
    if (auto* s = settings())
    {
        s->setValue (kKeyLatest, latest);
        s->setValue (kKeyEpoch, juce::String (juce::Time::getCurrentTime().toMilliseconds()));
        s->saveIfNeeded();
    }
}

void UpdateChecker::clearStored()
{
    if (auto* s = settings())
    {
        s->removeValue (kKeyLatest);
        s->saveIfNeeded();
    }
}

UpdateChecker::Result UpdateChecker::fetch (const juce::String& version)
{
    Result r;

    juce::URL url = juce::URL (kEndpoint)
                        .withParameter ("product", kProduct)
                        .withParameter ("v", version);

    juce::WebInputStream web (url, false);
    web.withConnectionTimeout (kTimeoutMs).withNumRedirectsToFollow (3);
    if (! web.connect (nullptr))            // no network / DNS / TLS failure
        return r;
    if (web.getStatusCode() != 200)         // 400 bad version / 404 unknown product / etc.
        return r;

    const juce::var json = juce::JSON::parse (web.readEntireStreamAsString());
    if (! json.isObject())
        return r;

    r.ok       = true;
    r.outdated = (bool) json.getProperty ("outdated", false);
    r.latest   = json.getProperty ("latest", juce::var()).toString();
    r.url      = json.getProperty ("url",    juce::var()).toString();
    r.notes    = json.getProperty ("notes",  juce::var()).toString();   // optional — empty if absent
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
