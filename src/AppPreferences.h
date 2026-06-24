// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_data_structures/juce_data_structures.h>   // ApplicationProperties / PropertiesFile

namespace orbitcab
{

//==============================================================================
// orbitcab::AppPreferences — owns the plugin's single GLOBAL PropertiesFile:
// per-machine, shared across every instance, surviving restarts (NOT the DAW
// session state). One owner is the whole point — two PropertiesFile instances on
// the same path would race on save. It backs the app-wide UI view-prefs (the
// gear-panel Dry/Wet + Spectrum toggles) and the UpdateChecker's last-seen tag.
//
// Adapter layer (host glue: storage). The cab:: DSP core never sees it.
//==============================================================================
class AppPreferences
{
public:
    AppPreferences()
    {
        // Global (NOT the DAW session) — shared across instances, survives restarts.
        juce::PropertiesFile::Options o;
        o.applicationName     = "OrbitCab";
        o.folderName          = "Darwin's Cat" + juce::String (juce::File::getSeparatorString()) + "OrbitCab";
        o.filenameSuffix      = "settings";
        o.osxLibrarySubFolder = "Application Support";
        props.setStorageParameters (o);
    }

    // Typed convenience for boolean view-prefs (the common UI case).
    bool getFlag (juce::StringRef key, bool defaultValue) const
    {
        if (auto* s = file()) return s->getBoolValue (key, defaultValue);
        return defaultValue;
    }

    void setFlag (juce::StringRef key, bool value)
    {
        if (auto* s = file()) { s->setValue (key, value); s->saveIfNeeded(); }
    }

    // Same, for small integer view-prefs (e.g. the waveform dB floor).
    int getInt (juce::StringRef key, int defaultValue) const
    {
        if (auto* s = file()) return s->getIntValue (key, defaultValue);
        return defaultValue;
    }

    void setInt (juce::StringRef key, int value)
    {
        if (auto* s = file()) { s->setValue (key, value); s->saveIfNeeded(); }
    }

    // Same, for a small string pref (e.g. the recent-IR list, joined with newlines) —
    // makes the recents a per-machine accumulator shared across instances, not session state.
    juce::String getString (juce::StringRef key, const juce::String& defaultValue = {}) const
    {
        if (auto* s = file()) return s->getValue (key, defaultValue);
        return defaultValue;
    }

    void setString (juce::StringRef key, const juce::String& value)
    {
        if (auto* s = file()) { s->setValue (key, value); s->saveIfNeeded(); }
    }

    // Raw PropertiesFile access for richer needs (the UpdateChecker's string tag +
    // epoch + removeValue). Logically const — getUserSettings() is non-const in JUCE.
    juce::PropertiesFile* file() const
    {
        return const_cast<AppPreferences*> (this)->props.getUserSettings();
    }

    // System-wide poweramp .nam library folder (sibling of the settings file), per-machine, shared
    // across instances. The user's managed amp captures live here; created on first ask.
    juce::File powerampDir() const
    {
        auto* s = file();
        auto dir = (s != nullptr ? s->getFile().getParentDirectory()
                                 : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                       .getChildFile ("Darwin's Cat").getChildFile ("OrbitCab"))
                       .getChildFile ("Poweramps");
        dir.createDirectory();
        return dir;
    }

private:
    juce::ApplicationProperties props;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppPreferences)
};

} // namespace orbitcab
