// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <felitronics/appkit/UpdateChecker.h>

#include "AppPreferences.h"

//==============================================================================
// orbitcab::UpdateChecker — a thin ADAPTER over the family's shared checker (felitronics-appkit),
// which is this checker PROMOTED and consolidated with TabbyEQ's copy, then crew-hardened. All
// policy (user-click only, silent failure, owned-thread join on destruction) and the whole GitHub
// flow live in the appkit header; this adapter only bakes in OrbitCab's Config.
//
// keyLatest/keyEpoch OVERRIDE appkit's namespaced defaults with OrbitCab's HISTORICAL bare keys —
// existing users have "lastSeenLatest" in their PropertiesFile, and the stored badge must survive
// the migration. Do not "clean this up": renaming the keys would silently drop every stored badge.
//==============================================================================
namespace orbitcab
{

struct UpdateChecker : felitronics::appkit::UpdateChecker
{
    // `currentVersion` = JucePlugin_VersionString; `prefs` must outlive this checker (the
    // processor declares appPreferencesInstance first — see PluginProcessor.h).
    UpdateChecker (juce::String currentVersion, AppPreferences& prefs)
        : felitronics::appkit::UpdateChecker ({ .ownerRepo      = "darwinscat/orbitcab",
                                                .productName    = "OrbitCab",
                                                .currentVersion = std::move (currentVersion),
                                                .settings       = [&prefs] { return prefs.file(); },
                                                .keyLatest      = "lastSeenLatest",
                                                .keyEpoch       = "lastCheckEpoch" }) {}
};

} // namespace orbitcab
