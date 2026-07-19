// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "LibraryManager.h"

//==============================================================================
// PreampManager — the "Manage library…" pop-over for the PREAMP stage: LibraryManager pointed at
// the PRE-amp library (proc.preampLibrary() / preampDir() / "preampSel") via these traits. The
// variant badge shows the parsed sub-dimensions (channel / gain / boost); "Get models…" links to
// tone3000 preamp searches. Distinct traits from PowerampManager on purpose — the preamp and
// poweramp libraries stay independent (see LibraryManager.h for the shared panel itself).
//==============================================================================
struct PreampManagerTraits
{
    using Entry = orbitcab::PreampEntry;

    // Wording.
    static constexpr const char* title              = "Preamp library";
    static constexpr const char* subtitle           = "Models the PREAMP selector lists. Drop .nam files here, or Add\xe2\x80\xa6";
    static constexpr const char* addTooltip         = "Copy one or more .nam captures into your preamp library.";
    static constexpr const char* revealTooltipStart = "Open the per-machine preamp folder in ";
    static constexpr const char* getTooltip         = "Open tone3000.com to download preamp captures (.nam) you have the right to use.";
    static constexpr const char* chooserTitle       = "Add preamp captures";

    // Library hooks — the PRE-amp side of the processor.
    static juce::File         userDir     (OrbitCabAudioProcessor& p)       { return p.appPreferences().preampDir(); }
    static std::vector<Entry> library     (const OrbitCabAudioProcessor& p) { return p.preampLibrary(); }
    static juce::String       selectedId  (const OrbitCabAudioProcessor& p) { return p.selectedPreampId(); }
    static void               select      (OrbitCabAudioProcessor& p, const juce::String& id) { p.selectPreamp (id); }
    static juce::File         importModel (OrbitCabAudioProcessor& p, const juce::File& f)    { return p.importPreamp (f); }
    static bool               removeModel (OrbitCabAudioProcessor& p, const juce::String& id) { return p.removePreamp (id); }

    // A compact descriptor of the variant's position in the device matrix ("Green · 12h · boost"),
    // pre-computed by PreampRig::build from the metadata (or the legacy filename grammar). Empty →
    // an em-dash (a plain, single-variant model).
    static juce::String describeVariant (const Entry& e)
    {
        return e.variant.isEmpty() ? juce::String::fromUTF8 ("\xe2\x80\x94") : e.variant;
    }

    // Row badge: the variant descriptor (channel / gain / boost).
    static void paintVariant (juce::Graphics& g, const Entry& e, juce::Rectangle<int>& body)
    {
        auto badge = body.removeFromRight (118);
        g.setColour (juce::Colour (0xff6a6a72));
        g.setFont (juce::FontOptions (10.5f));
        g.drawText (describeVariant (e), badge, juce::Justification::centredRight);
    }

    // "Get models…" → download sources for preamp captures. tone3000 hosts NAM captures.
    static void showGetModelsMenu (juce::TextButton& getBtn)
    {
        juce::PopupMenu m;
        m.addSectionHeader ("Download preamp captures (.nam)");
        m.addItem (1, juce::String::fromUTF8 ("Browse tone3000.com\xe2\x80\xa6"));
        m.addItem (2, juce::String::fromUTF8 ("Search preamp captures \xe2\x80\x94 tone3000"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (getBtn),
            [] (int r)
            {
                const char* url = r == 1 ? "https://www.tone3000.com"
                                : r == 2 ? "https://www.tone3000.com/tones?q=preamp"
                                         : nullptr;
                if (url != nullptr)
                    juce::URL (url).launchInDefaultBrowser();
            });
    }
};

using PreampManager = LibraryManager<PreampManagerTraits>;
