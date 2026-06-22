// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "SlotComponent.h"
#include "OrbitCabLookAndFeel.h"
#include "../IRLibrary.h"

#include <algorithm>

namespace
{
    // Shortest tail of a folder path (leaf first) that's unique among `all` — so two
    // folders sharing a leaf name (…/96k/Mixes/BLND vs …/44.1k/Mixes/BLND) get distinct,
    // readable labels instead of merging.
    juce::String minimalUniqueLabel (const juce::String& path, const juce::StringArray& all)
    {
        auto comps = juce::StringArray::fromTokens (path, "/\\", "");
        comps.removeEmptyStrings();
        auto tailOf = [] (const juce::String& p, int depth)
        {
            auto c = juce::StringArray::fromTokens (p, "/\\", "");
            c.removeEmptyStrings();
            juce::String t;
            for (int k = juce::jmax (0, c.size() - depth); k < c.size(); ++k)
                t += (t.isEmpty() ? "" : "/") + c[k];
            return t;
        };
        for (int depth = 1; depth <= comps.size(); ++depth)
        {
            const auto tail = tailOf (path, depth);
            int n = 0;
            for (const auto& p : all)
                if (tailOf (p, depth) == tail)
                    ++n;
            if (n <= 1)
                return tail;
        }
        return path;
    }
}

//==============================================================================
SlotComponent::SlotComponent (OrbitCabAudioProcessor& processor, int slotIndex)
    : proc (processor), index (slotIndex)
{
    const juce::String s    = sfx();
    const juce::String disp = isA() ? "I" : "II";   // display label (A/B freed for snapshots)

    // badge doubles as the MUTE toggle: bright when playing, dim when muted
    badge.setButtonText (disp);
    badge.setClickingTogglesState (true);
    badge.setColour (juce::TextButton::buttonColourId,   juce::Colour (isA() ? OrbitCabLookAndFeel::kAccent : OrbitCabLookAndFeel::kAccentB));
    badge.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2a2a2e));
    badge.setColour (juce::TextButton::textColourOffId,  juce::Colours::black);
    badge.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff80808a));
    badge.setTooltip ("Mute box " + disp);
    muteAtt = std::make_unique<BAtt> (proc.apvts, "mute" + s, badge);
    addAndMakeVisible (badge);

    name.setColour (juce::TextButton::buttonColourId, juce::Colour (OrbitCabLookAndFeel::kPanel));
    name.setColour (juce::TextButton::textColourOffId, juce::Colour (OrbitCabLookAndFeel::kText));
    name.onClick = [this] { showIRMenu(); };
    addAndMakeVisible (name);

    folder.onClick = [this] { chooseIR(); };
    prev.onClick   = [this] { stepIR (-1); };
    next.onClick   = [this] { stepIR (+1); };
    addAndMakeVisible (folder);
    addAndMakeVisible (prev);
    addAndMakeVisible (next);

    addAndMakeVisible (wave);
    wave.setTrimInteractive (true);
    wave.setEqVisible (true);
    wave.onTrimChanged   = [this] (float frac)        { proc.setTrim (frac, isA()); };
    wave.onHpfChanged    = [this] (bool on, float hz) { setHpfFromCurve (on, hz); };
    wave.onLpfChanged    = [this] (bool on, float hz) { setLpfFromCurve (on, hz); };

    addAndMakeVisible (hpfOn);  hpfOn.setTooltip ("High-pass filter");
    hpfOnAtt  = std::make_unique<BAtt> (proc.apvts, "hpfOn"  + s, hpfOn);
    addAndMakeVisible (lpfOn);  lpfOn.setTooltip ("Low-pass filter");
    lpfOnAtt  = std::make_unique<BAtt> (proc.apvts, "lpfOn"  + s, lpfOn);
    addAndMakeVisible (trimOn); trimOn.setTooltip ("Trim IR tail");
    trimOnAtt = std::make_unique<BAtt> (proc.apvts, "trimOn" + s, trimOn);
    addAndMakeVisible (phase);  phase.setTooltip ("Invert polarity");
    phaseAtt  = std::make_unique<BAtt> (proc.apvts, "phase"  + s, phase);

    // Dry/Wet slider (hidden until the gear panel enables it; setDryWetVisible drives it).
    dwLabel.setJustificationType (juce::Justification::centredLeft);
    dwLabel.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    dwLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb0b0b0));
    dwLabel.setVisible (false);
    addChildComponent (dwLabel);
    dwSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    dwSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
    dwSlider.setTooltip ("Dry / Wet mix");
    dwSlider.setVisible (false);
    addChildComponent (dwSlider);
    dwAtt = std::make_unique<SAtt> (proc.apvts, "mix" + s, dwSlider);

    // per-side accent: Slot A violet, Slot B orange (incl. the waveform)
    const juce::Colour accent (isA() ? OrbitCabLookAndFeel::kAccent : OrbitCabLookAndFeel::kAccentB);
    wave.setAccent (accent);
    for (auto* t : { &hpfOn, &lpfOn, &trimOn, &phase })
        t->setColour (juce::ToggleButton::tickColourId, accent);
    dwSlider.setColour (juce::Slider::thumbColourId, accent);
    dwSlider.setColour (juce::Slider::trackColourId, accent.withAlpha (0.55f));
    for (auto* btn : { &name, &folder, &prev, &next })
        btn->setColour (OrbitCabLookAndFeel::accentBorderColourId, accent);
}

//==============================================================================
void SlotComponent::rebuildList()
{
    list.clear();
    // bundled packs first (already natural-sorted + pack-split by the shared IRLibrary)
    for (const auto& b : orbitcab::bundledIRs())
        list.push_back ({ b.name, b.pack, true, b.data, b.size, {} });

    for (const auto& path : proc.getUserIRPaths())
    {
        const juce::File f (path);
        // group key = full folder PATH so same-named folders don't merge; the popup shows
        // a minimal unique path suffix as the label.
        list.push_back ({ f.getFileName(), f.getParentDirectory().getFullPathName(), false, nullptr, 0, f });
    }
}

void SlotComponent::selectIRByFile (const juce::File& file)
{
    for (int i = 0; i < (int) list.size(); ++i)
        if (! list[(size_t) i].bundled && list[(size_t) i].file == file)
        {
            selectIR (i);
            return;
        }
}

void SlotComponent::selectIR (int i, bool sendToProcessor)
{
    if (i < 0 || i >= (int) list.size())
        return;

    listIndex = i;
    const auto& e = list[(size_t) i];
    name.setButtonText (e.name);

    const bool a = isA();
    const bool wasBLoaded = proc.isSlotBLoaded();

    if (e.bundled)
    {
        wave.setFromMemory (e.data, (size_t) e.dataSize, e.name);
        if (sendToProcessor) proc.loadIRFromMemory (e.data, (size_t) e.dataSize, a, e.name);
    }
    else
    {
        wave.setFromFile (e.file);
        if (sendToProcessor) proc.loadIRFromFile (e.file, a);
    }

    const bool many = list.size() > 1;
    prev.setEnabled (many);
    next.setEnabled (many);

    if (! a && sendToProcessor && ! wasBLoaded && onFirstBLoad)
        onFirstBLoad();                                // B's first IR → editor snaps MIX to centre
}

void SlotComponent::stepIR (int delta)
{
    const int n = (int) list.size();
    if (n <= 0)
        return;
    const int cur = juce::jmax (0, listIndex);
    selectIR (((cur + delta) % n + n) % n);
}

void SlotComponent::showIRMenu()
{
    if (list.empty())
        return;

    juce::PopupMenu menu;
    juce::StringArray packOrder;
    std::vector<juce::PopupMenu> subs;
    std::vector<bool> bundledPack;

    for (int i = 0; i < (int) list.size(); ++i)
    {
        const auto& e = list[(size_t) i];
        int g = packOrder.indexOf (e.pack);
        if (g < 0) { g = packOrder.size(); packOrder.add (e.pack); subs.emplace_back(); bundledPack.push_back (e.bundled); }
        subs[(size_t) g].addItem (i + 1, e.name, true, i == listIndex);
    }

    // Clear THIS slot's IR (→ no cab, dry passthrough). At the very top, where you'd look to
    // change the IR. Distinct from "Clear recent list" below (which only wipes the recents).
    constexpr int clearSlotId = 1000001;
    menu.addItem (clearSlotId, juce::String::fromUTF8 ("\xe2\x9c\x95  No cabinet (clear)"));
    menu.addSeparator();

    for (int g = 0; g < packOrder.size(); ++g)
        if (bundledPack[(size_t) g])
            menu.addSubMenu (packOrder[g], subs[(size_t) g]);

    bool anyUser = false;
    for (int g = 0; g < packOrder.size(); ++g)
        anyUser = anyUser || ! bundledPack[(size_t) g];

    constexpr int clearId = 1000000;
    if (anyUser)
    {
        juce::StringArray userPaths;
        for (int g = 0; g < packOrder.size(); ++g)
            if (! bundledPack[(size_t) g])
                userPaths.add (packOrder[g]);

        menu.addSeparator();
        for (int g = 0; g < packOrder.size(); ++g)
            if (! bundledPack[(size_t) g])
            {
                const auto label = packOrder[g].isEmpty() ? juce::String ("User")
                                                          : minimalUniqueLabel (packOrder[g], userPaths);
                menu.addSubMenu (label, subs[(size_t) g]);
            }
        menu.addSeparator();
        menu.addItem (clearId, "Clear recent list");
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (name),
                        [this, safe = juce::Component::SafePointer<SlotComponent> (this)] (int result)
                        {
                            if (safe == nullptr) return;   // editor closed before the menu was dismissed
                            if (result == clearSlotId)
                            {
                                if (isA()) proc.clearSlotA(); else proc.clearSlotB();
                                syncFromProcessor();                       // name → "No IR", clear the waveform
                                if (onUserIRsChanged) onUserIRsChanged();  // editor re-syncs enablement / MIX
                            }
                            else if (result == clearId) { proc.clearUserIRs(); if (onUserIRsChanged) onUserIRsChanged(); }
                            else if (result > 0)        selectIR (result - 1);
                        });
}

void SlotComponent::chooseIR()
{
    chooser = std::make_unique<juce::FileChooser> ("Select an IR (or a folder of IRs)",
                                                   lastFolder, "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectDirectories,
        [this, safe = juce::Component::SafePointer<SlotComponent> (this)] (const juce::FileChooser& fc)
        {
            if (safe == nullptr) return;   // editor closed before the chooser returned
            const auto chosen = fc.getResult();
            if (chosen == juce::File())
                return;

            // Add the WHOLE folder — opening one file brings its siblings too; deduped.
            const juce::File folderF = chosen.isDirectory() ? chosen : chosen.getParentDirectory();
            lastFolder = folderF;

            auto files = folderF.findChildFiles (juce::File::findFiles, false, "*.wav;*.aif;*.aiff;*.flac");
            std::sort (files.begin(), files.end(),
                       [] (const juce::File& a, const juce::File& b)
                       { return a.getFileName().compareNatural (b.getFileName()) < 0; });
            for (const auto& f : files)
                proc.addUserIR (f);

            if (onUserIRsChanged)
                onUserIRsChanged();                    // shared history → rebuild BOTH slots' lists

            const juce::File toSelect = chosen.isDirectory() ? (files.isEmpty() ? juce::File() : files[0])
                                                             : chosen;
            if (toSelect != juce::File())
                selectIRByFile (toSelect);
        });
}

void SlotComponent::syncFromProcessor()
{
    const bool a = isA();
    const auto& s = proc.getSlotIR (a);
    using Status = orbitcab::state::SlotIR::Status;

    if (s.status == Status::empty)                             // no cab → dry passthrough
    {
        name.setButtonText ("No IR / Bypass");
        wave.clearIR();
        listIndex = -1;
        prev.setEnabled (! list.empty());
        next.setEnabled (! list.empty());
        return;
    }

    // The display name comes ONLY from the slot identity (never the raw ref / content id) — the
    // root fix for "ID instead of filename". A missing IR (bytes gone, file gone) shows ⚠ + name.
    const bool missing = (s.status == Status::missing);
    name.setButtonText (missing ? juce::String::fromUTF8 ("\xe2\x9a\xa0  ") + s.displayName
                                : s.displayName);

    // Resolve the popup-highlight index from the identity, not a remembered position: bundled →
    // by name, external → by the recovery path. (A rebuilt recents list never strands it now.)
    listIndex = -1;
    for (int i = 0; i < (int) list.size(); ++i)
    {
        const auto& e = list[(size_t) i];
        if (s.bundled ? (e.bundled && e.name == s.ref)
                      : (! e.bundled && s.localPath.isNotEmpty() && e.file.getFullPathName() == s.localPath))
        { listIndex = i; break; }
    }

    // Waveform source: bundled bytes from the matched entry; external from the embedded pool
    // (keyed by content id) or the original file on disk; missing → nothing to draw.
    if (missing)
        wave.clearIR();
    else if (s.bundled)
    {
        if (listIndex >= 0) wave.setFromMemory (list[(size_t) listIndex].data, (size_t) list[(size_t) listIndex].dataSize, s.displayName);
        else                wave.clearIR();
    }
    else if (auto* mb = proc.embeddedIRBytes (s.ref))
        wave.setFromMemory (mb->getData(), mb->getSize(), s.displayName);
    else if (s.localPath.isNotEmpty() && juce::File (s.localPath).existsAsFile())
        wave.setFromFile (juce::File (s.localPath));
    else
        wave.clearIR();

    wave.setTrimFraction (s.trim);
    prev.setEnabled (list.size() > 1);
    next.setEnabled (list.size() > 1);
}

//==============================================================================
void SlotComponent::pushFiltersToWave()
{
    auto& ap = proc.apvts;
    const juce::String s = sfx();
    const auto hr = ap.getParameterRange ("hpfFreq" + s);
    const auto lr = ap.getParameterRange ("lpfFreq" + s);
    wave.setFilters (ap.getRawParameterValue ("hpfOn" + s)->load() > 0.5f,
                     ap.getRawParameterValue ("hpfFreq" + s)->load(), hr.start, hr.end,
                     ap.getRawParameterValue ("lpfOn" + s)->load() > 0.5f,
                     ap.getRawParameterValue ("lpfFreq" + s)->load(), lr.start, lr.end);
    wave.setTrimEnabled (ap.getRawParameterValue ("trimOn" + s)->load() > 0.5f);
    wave.setHeadEnabled (proc.getHeadTrim());          // global, on-by-default session setting
}

void SlotComponent::setHpfFromCurve (bool on, float hz)
{
    auto& ap = proc.apvts;
    const juce::String s = sfx();
    if (auto* q = ap.getParameter ("hpfOn" + s))   q->setValueNotifyingHost (on ? 1.0f : 0.0f);
    if (auto* q = ap.getParameter ("hpfFreq" + s)) q->setValueNotifyingHost (ap.getParameterRange ("hpfFreq" + s).convertTo0to1 (hz));
}

void SlotComponent::setLpfFromCurve (bool on, float hz)
{
    auto& ap = proc.apvts;
    const juce::String s = sfx();
    if (auto* q = ap.getParameter ("lpfOn" + s))   q->setValueNotifyingHost (on ? 1.0f : 0.0f);
    if (auto* q = ap.getParameter ("lpfFreq" + s)) q->setValueNotifyingHost (ap.getParameterRange ("lpfFreq" + s).convertTo0to1 (hz));
}

void SlotComponent::setActive (bool on)
{
    wave.setEnabled (on);                              // wave dims via its own overlay
    const float a = on ? 1.0f : 0.42f;                 // muted/empty controls go dim
    for (auto* c : { &hpfOn, &lpfOn, &trimOn, &phase })
    {
        c->setEnabled (on);
        c->setAlpha (a);
    }
    dwSlider.setEnabled (on);
    dwSlider.setAlpha (a);
    dwLabel.setAlpha (a);
}

void SlotComponent::setDryWetVisible (bool shouldShow)
{
    dwLabel.setVisible (shouldShow);
    dwSlider.setVisible (shouldShow);
    resized();                                         // wave reclaims the row when hidden
}

void SlotComponent::selectBundledStartingWith (const juce::String& namePrefix)
{
    for (int i = 0; i < (int) list.size(); ++i)
        if (list[(size_t) i].bundled && list[(size_t) i].name.startsWithIgnoreCase (namePrefix))
        {
            selectIR (i);
            return;
        }
}

//==============================================================================
void SlotComponent::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    auto top = area.removeFromTop (30);
    badge.setBounds  (top.removeFromLeft (30));
    next.setBounds   (top.removeFromRight (30));
    prev.setBounds   (top.removeFromRight (30));
    folder.setBounds (top.removeFromRight (52).reduced (3, 2));
    name.setBounds   (top.reduced (8, 0));
    area.removeFromTop (6);

    // Dry/Wet slider docked under the checkbox row (only when visible — else the wave
    // reclaims the space). Sits below the checkboxes, so remove it from the bottom first.
    if (dwSlider.isVisible())
    {
        auto dwRow = area.removeFromBottom (24);
        dwLabel.setBounds  (dwRow.removeFromLeft (58));
        dwSlider.setBounds (dwRow);
        area.removeFromBottom (4);
    }

    auto cbRow = area.removeFromBottom (26);
    const int w = cbRow.getWidth() / 4;
    hpfOn.setBounds  (cbRow.removeFromLeft (w));
    lpfOn.setBounds  (cbRow.removeFromLeft (w));
    trimOn.setBounds (cbRow.removeFromLeft (w));
    phase.setBounds  (cbRow);

    area.removeFromBottom (6);
    wave.setBounds (area);
}
