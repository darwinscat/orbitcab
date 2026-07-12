// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "LibraryManager.h"

//==============================================================================
// PowerampManager — the "Manage library…" pop-over for the POWERAMP stage: LibraryManager pointed
// at the poweramp library (proc.powerampLibrary() / powerampDir() / "ampSel") via these traits.
// The variant badge is the mode (PP / SE / —); "Get models…" links to the tone3000 poweramp tones
// we tested against. Distinct traits from PreampManager on purpose — the preamp and poweramp
// libraries stay independent (see LibraryManager.h for the shared panel itself).
//==============================================================================
struct PowerampManagerTraits
{
    using Entry = orbitcab::PowerampEntry;

    // Wording.
    static constexpr const char* title              = "Poweramp library";
    static constexpr const char* subtitle           = "Models the POWERAMP selector lists. Drop .nam files here, or Add\xe2\x80\xa6";
    static constexpr const char* addTooltip         = "Copy one or more .nam captures into your poweramp library.";
    static constexpr const char* revealTooltipStart = "Open the per-machine poweramp folder in ";
    static constexpr const char* getTooltip         = "Open tone3000.com to download poweramp captures (.nam) you have the right to use.";
    static constexpr const char* chooserTitle       = "Add poweramp captures";

    // Library hooks — the POWER-amp side of the processor.
    static juce::File         userDir     (OrbitCabAudioProcessor& p)       { return p.appPreferences().powerampDir(); }
    static std::vector<Entry> library     (const OrbitCabAudioProcessor& p) { return p.powerampLibrary(); }
    static juce::String       selectedId  (const OrbitCabAudioProcessor& p) { return p.selectedPowerampId(); }
    static void               select      (OrbitCabAudioProcessor& p, const juce::String& id) { p.selectPoweramp (id); }
    static juce::File         importModel (OrbitCabAudioProcessor& p, const juce::File& f)    { return p.importPoweramp (f); }
    static bool               removeModel (OrbitCabAudioProcessor& p, const juce::String& id) { return p.removePoweramp (id); }

    // Row badge: the mode (PP / SE / — for Other). NB: sets no font — it deliberately inherits the
    // FACTORY/USER tag font (9.5f bold) the row set just before, exactly as the pre-dedup panel
    // painted it. Setting a font here would change the badge's pixels.
    static void paintVariant (juce::Graphics& g, const Entry& e, juce::Rectangle<int>& body)
    {
        auto badge = body.removeFromRight (40);
        g.setColour (juce::Colour (0xff6a6a72));
        g.drawText (e.cat == orbitcab::PowerampCat::pushPull    ? "PP"
                  : e.cat == orbitcab::PowerampCat::singleEnded ? "SE"
                                                                : juce::String::fromUTF8 ("\xe2\x80\x94"),
                    badge, juce::Justification::centred);
    }

    // "Get models…" → a popup of download sources. tone3000 hosts NAM captures; the named entries
    // are the poweramp tones we tested against. All open in the default browser (user-initiated).
    static void showGetModelsMenu (juce::TextButton& getBtn)
    {
        juce::PopupMenu m;
        m.addSectionHeader ("Download poweramp captures (.nam)");
        m.addItem (1, juce::String::fromUTF8 ("Browse tone3000.com\xe2\x80\xa6"));
        m.addSeparator();
        m.addItem (2, juce::String::fromUTF8 ("Mesa Boogie Mark V \xe2\x80\x94 tone3000"));
        m.addItem (3, juce::String::fromUTF8 ("Fryette Power Station PS-1 \xe2\x80\x94 tone3000"));
        m.addItem (4, juce::String::fromUTF8 ("Peavey Classic 120 \xe2\x80\x94 tone3000"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (getBtn),
            [] (int r)
            {
                const char* url = r == 1 ? "https://www.tone3000.com"
                                : r == 2 ? "https://www.tone3000.com/tones/mesa-boogie-mark-v-poweramp-68701"
                                : r == 3 ? "https://www.tone3000.com/tones/fryette-powerstation-ps-1-6l6gc-poweramp-5075"
                                : r == 4 ? "https://www.tone3000.com/tones/peavey-classic-series-120-6l6gc-poweramp-5681"
                                         : nullptr;
                if (url != nullptr)
                    juce::URL (url).launchInDefaultBrowser();
            });
    }
};

using PowerampManager = LibraryManager<PowerampManagerTraits>;
