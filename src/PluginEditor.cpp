// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "PluginEditor.h"
#include "IRLibrary.h"    // bundled-IR enumeration (shared with the processor)
#include "BinaryData.h"

#include <algorithm>

namespace
{
    // Parse a typed number tolerantly: accept '.' or ',' as the decimal point, plus the
    // Cyrillic keys that sit on the same caps as ./, (б Б ю Ю), and leading/trailing
    // separators (",2" -> 0.2, "234б" -> 234). Keeps digits, sign and separators only.
    double parseLooseNumber (const juce::String& text)
    {
        static const juce::String seps = juce::String::fromUTF8 (",\xd0\xb1\xd0\x91\xd1\x8e\xd0\xae"); // , б Б ю Ю
        auto s = text.retainCharacters (juce::String ("0123456789.-") + seps);
        s = s.replaceCharacters (seps, juce::String::repeatedString (".", seps.length()));
        return s.getDoubleValue();
    }
}

//==============================================================================
OrbitCabAudioProcessorEditor::OrbitCabAudioProcessorEditor (OrbitCabAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setLookAndFeel (&lnf);

    // ---- header ----
    logo = juce::Drawable::createFromImageData (BinaryData::logodarwinscat_svg,
                                                (size_t) BinaryData::logodarwinscat_svgSize);

    // Whole brand strip (cat + (◎)rbitCab wordmark + "— IR Cabinet Loader · Felitronics by
    // Darwin's Cat") links to the OrbitCab page, hover halo; sized to its text.
    brand.logo   = logo.get();
    brand.wordmarkTypeface = juce::Typeface::createSystemTypefaceFor (
        BinaryData::MichromaRegular_ttf, (size_t) BinaryData::MichromaRegular_ttfSize);   // embedded Michroma (OFL)
    brand.accent = juce::Colour (OrbitCabLookAndFeel::kAccentHover);
    brand.onLaunch = [] { juce::URL ("https://darwinscat.com/orbitcab?utm_source=orbitcab&utm_medium=plugin").launchInDefaultBrowser(); };
    brand.setTooltip ("darwinscat.com/orbitcab — OrbitCab by Darwin's Cat");
    addAndMakeVisible (brand);

    addAndMakeVisible (versionBadge);   // bottom-left version + opt-in update check

    addAndMakeVisible (presetBox);
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if      (id == 2)                                             applyDefaultPreset();                                  // Default (= the bundled default)
        else if (id >= 100  && id - 100  < (int) factoryList.size())  loadFactoryPreset (factoryList[(size_t) (id - 100)]);  // OrbitCab bank
        else if (id >= 1000 && id - 1000 < presetFiles.size())        loadPresetFile (presetFiles[id - 1000]);              // User
    };
    saveBtn.framed = true;                 // floppy icon, framed like the A/B/C/D + trash group
    saveBtn.onClick = [this] { saveCurrentPreset(); };
    saveBtn.setTooltip ("Save changes to the current preset (disabled for the factory Default — use Save As)");
    addAndMakeVisible (saveBtn);
    saveAsBtn.framed = true;
    saveAsBtn.onClick = [this] { promptSavePreset (nextCopyName (processorRef.presetMeta().name)); };
    saveAsBtn.setTooltip ("Save as a new preset in the library");
    addAndMakeVisible (saveAsBtn);

    // Preset-centric: make sure a dirty baseline exists for the live state, so edits to the
    // first-start Default immediately read as "Default *". (The processor already captured one
    // on construction; this is a no-op safety net.)
    processorRef.ensureBaselineCaptured();

    // Keyboard: 1/2/3/4 recall snapshot A/B/C/D. Want focus so the editor receives key events
    // (a bubbled key from a child lands here); some hosts forward keys, some don't.
    setWantsKeyboardFocus (true);

    // Export / Import a .orbitcab preset file (IR embedded). Icon-only buttons, right of Save.
    exportBtn.setTooltip ("Export preset to a .orbitcab file (IR included)");
    exportBtn.onClick = [this] { exportPreset(); };
    addAndMakeVisible (exportBtn);
    importBtn.setTooltip ("Import a .orbitcab preset");
    importBtn.onClick = [this] { importPreset(); };
    addAndMakeVisible (importBtn);

    // Delete the current preset (to the Trash). Enabled only for a user preset — a factory
    // preset (Default) or an external one is read-only; updatePresetDisplay keeps this in sync.
    // Framed (bordered) so it groups with the Save / Save As actions, not the file icons.
    trashBtn.framed = true;
    trashBtn.setTooltip ("Delete the current preset");
    trashBtn.onClick = [this] { deleteCurrentPreset(); };
    addAndMakeVisible (trashBtn);

    // Undo / redo (full-state snapshot stack — the processor owns it). Framed like the
    // A/B/C/D buttons; start disabled (nothing to undo/redo yet) — the 30 Hz timer keeps
    // their enabled state (and so their dimmed border) in sync with canUndo()/canRedo().
    undoBtn.framed = true;
    undoBtn.setEnabled (false);
    undoBtn.setTooltip ("Undo");
    undoBtn.onClick = [this] { if (processorRef.undo()) afterUndoRedo(); };
    addAndMakeVisible (undoBtn);
    redoBtn.framed = true;
    redoBtn.setEnabled (false);
    redoBtn.setTooltip ("Redo");
    redoBtn.onClick = [this] { if (processorRef.redo()) afterUndoRedo(); };
    addAndMakeVisible (redoBtn);

    settingsBtn.setTooltip ("Settings");
    settingsBtn.colour = juce::Colour (0xffc0c0c8);
    settingsBtn.onClick = [this] { openSettings(); };
    addAndMakeVisible (settingsBtn);
    spectrumEnabled = processorRef.appPreferences().getFlag ("spectrumOn", true);   // global view pref
    processorRef.setSpectrumActive (spectrumEnabled);                    // editor open + analyser on
    waveLogPref   = processorRef.appPreferences().getFlag ("waveLog",  true);       // global view prefs
    waveFloorPref = processorRef.appPreferences().getInt  ("waveFloor", -48);       // default log, −48 dB
    applyWaveformScale();

    // ---- A/B/C/D compare (snapshot registers) ----
    // Distinct from the violet/orange IR-slot badges (I/II): a neutral toggle group in
    // the header. Each holds a full snapshot of every setting; one is active at a time.
    static const char* const snapNames[OrbitCabAudioProcessor::kNumSnapshots] = { "A", "B", "C", "D" };
    for (int i = 0; i < OrbitCabAudioProcessor::kNumSnapshots; ++i)
    {
        auto& b = snapBtn[i];
        b.setButtonText (snapNames[i]);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (0x10C1500 + 1);                          // exclusive group
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (OrbitCabLookAndFeel::kPanel));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff8a8a92));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::black);
        b.setColour (OrbitCabLookAndFeel::accentBorderColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));
        b.setTooltip ("Snapshot " + juce::String (snapNames[i]) + "  (key " + juce::String (i + 1) + ")");
        b.onClick = [this, i] { switchSnapshot (i); };
        addAndMakeVisible (b);
    }
    updateSnapshotButtons();

    // ---- slots ----  (two self-contained SlotComponents; the editor only wires the
    // two cross-slot hooks: rebuild BOTH lists when the shared user-IR history changes,
    // and snap MIX to centre when B's first IR loads.)
    for (auto& slot : slots)
    {
        slot.onUserIRsChanged = [this]
        {
            slots[0].rebuildList(); slots[1].rebuildList();
            slots[0].syncFromProcessor(); slots[1].syncFromProcessor();   // re-resolve both selections (bug D)
        };
        slot.onFirstBLoad     = [this] { snapMixToCentre(); };
        addAndMakeVisible (slot);
    }

    // ---- INPUT block (bypass + input fader + IN meter) ----
    addAndMakeVisible (bypassBtn);
    bypassAtt = std::make_unique<BAtt> (processorRef.apvts, "bypass", bypassBtn);
    bypassBtn.setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));
    bypassBtn.setTooltip ("Bypass");
    inGainFader.setTooltip ("Input gain");
    inGainFader.setSliderStyle (juce::Slider::LinearVertical);
    inGainFader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 15);
    inGainFader.setColour (juce::Slider::trackColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));   // global = neutral, not side-coloured
    inGainFader.setColour (juce::Slider::thumbColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));
    addAndMakeVisible (inGainFader);
    inGainAtt = std::make_unique<SAtt> (processorRef.apvts, "inputGain", inGainFader);
    inGainFader.valueFromTextFunction = [] (const juce::String& t) { return parseLooseNumber (t); };
    addAndMakeVisible (inMeter);
    styleLabel (inLabel, "IN");

    // ---- OUTPUT block (auto + master/mix-volume fader + OUT meter) ----
    addAndMakeVisible (autoBtn);
    autoAtt = std::make_unique<BAtt> (processorRef.apvts, "autoLevel", autoBtn);
    autoBtn.setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));
    autoBtn.setTooltip ("Auto level");
    masterFader.setTooltip ("Output level");
    masterFader.setSliderStyle (juce::Slider::LinearVertical);
    masterFader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 15);
    masterFader.setColour (juce::Slider::trackColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));   // global = neutral
    masterFader.setColour (juce::Slider::thumbColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));
    addAndMakeVisible (masterFader);
    masterAtt = std::make_unique<SAtt> (processorRef.apvts, "gain", masterFader);
    masterFader.valueFromTextFunction = [] (const juce::String& t) { return parseLooseNumber (t); };
    addAndMakeVisible (outMeter);
    styleLabel (outLabel, "OUT");

    // ---- MIX (A<->B) bottom centre ----
    mixABSlider.setTooltip ("Box I / II balance");
    mixABSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    mixABSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 18);   // ±dB readout/input
    mixABSlider.setSliderSnapsToMousePosition (false);                         // drag relative; click won't jump to A/B
    // track is drawn as an A→B gradient in paint(); keep the slider's own track clear
    mixABSlider.setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
    mixABSlider.setColour (juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
    mixABSlider.setColour (juce::Slider::thumbColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));
    mixABSlider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (OrbitCabLookAndFeel::kText));
    addAndMakeVisible (mixABSlider);
    mixABAtt = std::make_unique<SAtt> (processorRef.apvts, "mixAB", mixABSlider);
    // show/accept the A↔B balance as ±dB (0 = centre, + toward A, − toward B). Set after
    // the attachment, which otherwise installs the param's "%" text.
    mixABSlider.textFromValueFunction = [] (double v)
    {
        if (v <= 0.5)  return juce::String ("I");      // full IR box I
        if (v >= 99.5) return juce::String ("II");     // full IR box II
        const double frac = v / 100.0;
        const double db   = juce::jlimit (-24.0, 24.0, 20.0 * std::log10 ((1.0 - frac) / frac));
        return juce::String (db, 1) + " dB";
    };
    mixABSlider.valueFromTextFunction = [] (const juce::String& t)
    {
        const auto s = t.trim();
        if (s.startsWithIgnoreCase ("ii")) return 100.0;     // box II (check before "i")
        if (s.startsWithIgnoreCase ("i"))  return 0.0;       // box I
        const double r = std::pow (10.0, parseLooseNumber (s) / 20.0);
        return juce::jlimit (0.0, 100.0, 100.0 / (1.0 + r));
    };
    mixABSlider.updateText();
    // set AFTER the attachment so 50 is interpreted in the 0..100 range (the attachment
    // resets the range, which would otherwise clamp the return value to A).
    mixABSlider.setDoubleClickReturnValue (true, 50.0);                        // double-click → centre
    styleLabel (mixABLabel, juce::String::fromUTF8 ("I    \xe2\x80\x94    MIX    \xe2\x80\x94    II"));

    // ---- POWERAMP (NAM): power checkbox (bottom strip) + revealed selector row ----
    ampPowerBtn.setTooltip ("Power the NAM poweramp stage in front of the cab \xe2\x80\x94 reveals the model selector below.");
    ampPowerBtn.setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
    addAndMakeVisible (ampPowerBtn);
    ampPowerAtt = std::make_unique<BAtt> (processorRef.apvts, "ampOn", ampPowerBtn);
    ampPowerBtn.onClick = [this] { updateAmpRow(); };          // reveal/hide the row + resize on toggle

    addAndMakeVisible (tubeDisplay);   // symbolic amp + glowing tube(s) in the revealed row

    // Contextual PP / SE toggle — shown only when the selected NAME has both modes. Param-free,
    // driven by syncAmpSelector (no self-toggle). [0] = push-pull, [1] = single-ended.
    static const char* const kModeNames[2] = { "PP", "SE" };
    for (int m = 0; m < 2; ++m)
    {
        ampModeBtn[m].setButtonText (kModeNames[m]);
        ampModeBtn[m].setClickingTogglesState (false);
        ampModeBtn[m].setColour (juce::TextButton::buttonOnColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
        ampModeBtn[m].setTooltip (m == 0 ? "Push-pull capture \xe2\x80\x94 a pair of power tubes."
                                         : "Single-ended capture \xe2\x80\x94 one power tube.");
        ampModeBtn[m].onClick = [this, m] { selectAmpMode (m == 0 ? orbitcab::PowerampCat::pushPull
                                                                   : orbitcab::PowerampCat::singleEnded); };
        addChildComponent (ampModeBtn[m]);
    }

    // Singletons combo (one-off amps by full filename). Selecting an item picks that capture.
    ampSingleBox.setTextWhenNothingSelected ("Amps\xe2\x80\xa6");
    ampSingleBox.setTooltip ("One-off poweramp captures (not part of a tube family).");
    ampSingleBox.onChange = [this]
    {
        const int idx = ampSingleBox.getSelectedId() - 1;            // ids are 1-based → entry index
        if (juce::isPositiveAndBelow (idx, (int) ampLib.size()))
        { processorRef.selectPoweramp (ampLib[(size_t) idx].id); syncAmpSelector(); }
    };
    addChildComponent (ampSingleBox);

    rebuildAmpSelector();              // scan the merged library → name buttons + combo + hasPoweramps + power-btn visibility

    // ---- IR library + restore display ----
    slots[0].rebuildList();
    slots[1].rebuildList();
    slots[0].syncFromProcessor();
    slots[1].syncFromProcessor();
    dryWetPref   = processorRef.appPreferences().getFlag ("dryWetShown", false);   // global view pref
    showTubesPref = processorRef.appPreferences().getFlag ("showTubes", true);      // tube graphics (default on)
    refreshDryWetVisibility();   // applies the pref, or force-shows if a loaded blend has mix ≠ 100%
    pushFiltersToWave();
    refreshPresets();

    // Seed the revision caches to the current values so the first timer tick doesn't re-sync
    // what the ctor just synced (only genuine later changes trip the poll).
    lastSoundRev     = processorRef.soundRevision();
    lastWorkspaceRev = processorRef.workspaceRevision();
    lastUserIRRev    = processorRef.userIRRevision();

    startTimerHz (30);
    updateAmpRow();   // initial reveal state + window size (amp off by default → 620)
}

OrbitCabAudioProcessorEditor::~OrbitCabAudioProcessorEditor()
{
    stopTimer();
    processorRef.setSpectrumActive (false);   // editor gone → stop feeding the analyser
    setLookAndFeel (nullptr);
}

void OrbitCabAudioProcessorEditor::timerCallback()
{
    inMeter.setLevel  (processorRef.getInputLevel());
    outMeter.setLevel (processorRef.getOutputLevel());
    pushFiltersToWave();
    updateEnablement();
    refreshDryWetVisibility();                     // catch a blend (mix ≠ 100%) loaded by the host
    updateSpectrum();

    // Reflect poweramp params onto the controls (host automation / state restore don't fire the
    // button onClicks). A change in ampOn re-runs the reveal + resize; otherwise just keep the
    // tube radio buttons in sync with the choice param.
    if (hasPoweramps)                              // no bundled models → no POWERAMP UI to sync
    {
        if ((processorRef.apvts.getRawParameterValue ("ampOn")->load() > 0.5f) != ampOnCache)
            updateAmpRow();                        // ampOn flipped → reveal/hide + resize
        else if (processorRef.selectedPowerampId() != ampSyncedId)
            syncAmpSelector();                     // selection changed externally (automation / restore) → re-sync
        if (ampPowerBtn.getToggleState())
            tubeDisplay.tick();                    // advance the warm heater flicker (~30 Hz)
    }

    processorRef.undoTick();                       // coalesce edits into undo steps
    undoBtn.setEnabled (processorRef.canUndo());
    redoBtn.setEnabled (processorRef.canRedo());

    // Catch processor-side state changes WITHOUT a push callback (revision-counter poll): a
    // host-driven setStateInformation, or any path that didn't already re-sync the editor.
    // Editor-initiated actions re-sync immediately AND bump these, so this is mostly a no-op
    // safety net — but it's what keeps the slot display honest after a host reload.
    if (const auto r = processorRef.userIRRevision(); r != lastUserIRRev)
    {
        lastUserIRRev = r;
        slots[0].rebuildList(); slots[1].rebuildList();
        slots[0].syncFromProcessor(); slots[1].syncFromProcessor();
    }
    if (const auto r = processorRef.workspaceRevision(); r != lastWorkspaceRev)
    {
        lastWorkspaceRev = r;
        updateSnapshotButtons();
        slots[0].syncFromProcessor(); slots[1].syncFromProcessor();
    }
    if (const auto r = processorRef.soundRevision(); r != lastSoundRev)
    {
        lastSoundRev = r;
        slots[0].syncFromProcessor(); slots[1].syncFromProcessor();
        pushFiltersToWave();
    }

    updatePresetDisplay();                         // live dirty marker ("Default *") as knobs move
}

void OrbitCabAudioProcessorEditor::updateEnablement()
{
    auto& ap = processorRef.apvts;
    const bool aMuted = ap.getRawParameterValue ("muteA")->load() > 0.5f;
    const bool bMuted = ap.getRawParameterValue ("muteB")->load() > 0.5f;
    const bool bLoaded = processorRef.isSlotBLoaded();

    for (int slot = 0; slot < 2; ++slot)
    {
        const bool isASlot = (slot == 0);
        const bool on = (isASlot || bLoaded) && ! (isASlot ? aMuted : bMuted);
        if (on == slotOnCache[slot])
            continue;                              // only re-dim on change
        slotOnCache[slot] = on;
        slots[slot].setActive (on);                // dim the wave + checkbox row when muted/empty
    }

    // MIX is meaningful (and gradient-coloured) only when both slots actually play
    const bool both = ! aMuted && bLoaded && ! bMuted;
    if (both != mixOnCache)
    {
        mixOnCache = both;
        mixABSlider.setEnabled (both);
        repaint (mixStripBounds);                  // refresh the A→B gradient / neutral track
    }
}

void OrbitCabAudioProcessorEditor::refreshDryWetVisibility()
{
    // A Dry/Wet mix below 100% means raw input is actually blended in — never let that hide
    // behind the gear pref as an invisible "ghost knob". Effective visibility = the user's
    // pref OR an active blend on either slot, so a hidden pref only holds while both sit at
    // full wet. The mix step is 0.1, so < 99.95 cleanly excludes the 100% default.
    auto& ap = processorRef.apvts;
    const bool blendActive = ap.getRawParameterValue ("mixA")->load() < 99.95f
                          || ap.getRawParameterValue ("mixB")->load() < 99.95f;
    const bool show = dryWetPref || blendActive;
    if (show == dryWetShownCache)
        return;                                    // only re-lay-out on change
    dryWetShownCache = show;
    slots[0].setDryWetVisible (show);
    slots[1].setDryWetVisible (show);
}

void OrbitCabAudioProcessorEditor::updateSpectrum()
{
    if (! spectrumEnabled)                           // analyser off
        return;

    spectrum.update (processorRef);
    slots[0].setSpectrum (spectrum.pre(), spectrum.post());
    slots[1].setSpectrum (spectrum.pre(), spectrum.post());
}

void OrbitCabAudioProcessorEditor::applyWaveformScale()
{
    slots[0].setWaveformScale (waveLogPref, (float) waveFloorPref);
    slots[1].setWaveformScale (waveLogPref, (float) waveFloorPref);
}

void OrbitCabAudioProcessorEditor::openSettings()
{
    auto panel = std::make_unique<SettingsPanel> (
        processorRef.getHeadTrim(), dryWetPref, spectrumEnabled, waveLogPref, waveFloorPref,
        showTubesPref,
        [this] (bool on)                                   // HEAD: persisted session setting
        {
            processorRef.setHeadTrim (on);
            pushFiltersToWave();                           // refresh the waveform head overlay
        },
        [this] (bool on)                                   // Dry/Wet sliders: global view preference
        {
            dryWetPref = on;
            processorRef.appPreferences().setFlag ("dryWetShown", on);   // persist across sessions
            refreshDryWetVisibility();   // a hidden pref only sticks while both slots sit at 100% wet
        },
        [this] (bool on)                                   // Spectrum: global view preference
        {
            spectrumEnabled = on;
            processorRef.appPreferences().setFlag ("spectrumOn", on);   // persist across sessions
            processorRef.setSpectrumActive (on);           // stop the audio-thread feed when off
            if (! on)                                      // turning off → clear both displays
            {
                spectrum.clear();
                slots[0].setSpectrum (spectrum.pre(), spectrum.post());
                slots[1].setSpectrum (spectrum.pre(), spectrum.post());
            }
        },
        [this] (bool on)                                   // Log waveform (dB): global view pref
        {
            waveLogPref = on;
            processorRef.appPreferences().setFlag ("waveLog", on);
            applyWaveformScale();
        },
        [this] (int db)                                    // dB floor: global view pref
        {
            waveFloorPref = db;
            processorRef.appPreferences().setInt ("waveFloor", db);
            applyWaveformScale();
        },
        [this] (bool on)                                   // Show tubes: global view pref
        {
            showTubesPref = on;
            processorRef.appPreferences().setFlag ("showTubes", on);
            updateAmpRow();                                // re-flow + resize the row (tall ↔ slim strip)
        },
        [this]                                             // Manage library…: open the poweramp manager
        {
            if (settingsCallout != nullptr)
                settingsCallout->dismiss();                // close the settings pop-over first (no nested call-outs)
            openPowerampManager();
        });

    // Parent the pop-over to the editor (not the desktop) so it can't outlive the window
    // (the version call-out orphan bug, #52). areaToPointTo is in the editor's coords.
    settingsCallout = &juce::CallOutBox::launchAsynchronously (std::move (panel), settingsBtn.getBounds(), this);
}

void OrbitCabAudioProcessorEditor::openPowerampManager()
{
    // The library manager: Add / Remove the user .nam captures (powerampDir). On any change it
    // rebuilds the bottom-strip selector + re-flows the row — so adding the first model makes the
    // whole POWERAMP UI appear, and removing the active one falls back cleanly.
    auto mgr = std::make_unique<PowerampManager> (processorRef, [this]
    {
        rebuildAmpSelector();   // rescan merged library → model buttons + hasPoweramps + power-btn visibility
        updateAmpRow();         // reveal/hide + resize to match the new library / selection
    });
    juce::CallOutBox::launchAsynchronously (std::move (mgr), settingsBtn.getBounds(), this);
}

//==============================================================================
void OrbitCabAudioProcessorEditor::styleLabel (juce::Label& l, const juce::String& t)
{
    l.setText (t, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    addAndMakeVisible (l);
}

// Guess a tube silhouette (0=6L6 / 1=EL34 / 2=EL84 / 3=KT88) from the model's display name, so a
// well-named capture still draws its real glass; unknown → the 6L6 coke-bottle. Cosmetic only.
static int tubeTypeFromName (const juce::String& name)
{
    const auto u = name.toUpperCase();
    if (u.contains ("EL34")) return 1;
    if (u.contains ("EL84")) return 2;
    if (u.contains ("KT88") || u.contains ("KT66") || u.contains ("6550")) return 3;
    return 0;   // 6L6 / 6V6 / unknown
}

// Default position when the current hours don't exist in the target group: prefer noon (12h),
// else the middle of the available sweep, else 0 (no hours).
static int defaultAmpHour (const std::vector<int>& hrs)
{
    if (hrs.empty()) return 0;
    for (int h : hrs) if (h == 12) return 12;
    return hrs[hrs.size() / 2];
}

//==============================================================================
// Library queries (computed from the cached ampLib — small, no caching needed).
const orbitcab::PowerampEntry* OrbitCabAudioProcessorEditor::ampEntryById (const juce::String& id) const
{
    for (const auto& e : ampLib) if (e.id == id) return &e;
    return nullptr;
}

bool OrbitCabAudioProcessorEditor::isGroupName (const juce::String& name) const
{
    int n = 0;
    for (const auto& e : ampLib) if (e.name == name && ++n >= 2) return true;
    return false;
}

std::vector<orbitcab::PowerampCat> OrbitCabAudioProcessorEditor::catsForName (const juce::String& name) const
{
    std::vector<orbitcab::PowerampCat> out;
    for (const auto& e : ampLib)
        if (e.name == name && std::find (out.begin(), out.end(), e.cat) == out.end())
            out.push_back (e.cat);
    std::sort (out.begin(), out.end(), [] (auto a, auto b) { return (int) a < (int) b; });   // PP, SE, Other
    return out;
}

std::vector<int> OrbitCabAudioProcessorEditor::hoursForNameCat (const juce::String& name, orbitcab::PowerampCat c) const
{
    std::vector<int> out;
    for (const auto& e : ampLib)
        if (e.name == name && e.cat == c && std::find (out.begin(), out.end(), e.hours) == out.end())
            out.push_back (e.hours);
    std::sort (out.begin(), out.end());
    return out;
}

juce::String OrbitCabAudioProcessorEditor::findAmpId (const juce::String& name, orbitcab::PowerampCat c, int hours) const
{
    for (const auto& e : ampLib)
        if (e.name == name && e.cat == c && e.hours == hours) return e.id;
    return {};
}

//==============================================================================
void OrbitCabAudioProcessorEditor::rebuildAmpSelector()
{
    for (auto& b : ampNameBtns) removeChildComponent (b.get());
    ampNameBtns.clear();
    ampGroupNames.clear();

    ampLib       = processorRef.powerampLibrary();   // factory (BinaryData) + user (powerampDir), merged
    hasPoweramps = ! ampLib.empty();

    // GROUPS: a display name shared by ≥2 captures → one name button (tube families: 6L6 / EL34 / …).
    {
        juce::StringArray seen;
        for (const auto& e : ampLib)
            if (isGroupName (e.name) && ! seen.contains (e.name))
            { seen.add (e.name); ampGroupNames.push_back (e.name); }
    }
    for (const auto& nm : ampGroupNames)
    {
        auto b = std::make_unique<juce::TextButton> (nm);
        b->setClickingTogglesState (false);   // visual state driven by syncAmpSelector (no stuck-fill bug)
        b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
        b->setTooltip ("Poweramp capture family (pick mode / position below).");
        const auto name = nm;
        b->onClick = [this, name] { selectAmpName (name); };
        addChildComponent (*b);
        ampNameBtns.push_back (std::move (b));
    }

    // SINGLETONS: a one-off capture → the combo, shown by its full filename. Item id = entry index + 1.
    ampSingleBox.clear (juce::dontSendNotification);
    for (int i = 0; i < (int) ampLib.size(); ++i)
        if (! isGroupName (ampLib[(size_t) i].name))
            ampSingleBox.addItem (ampLib[(size_t) i].id.fromFirstOccurrenceOf (":", false, false), i + 1);

    ampPowerBtn.setVisible (hasPoweramps);
}

// Pick a GROUP by name — keep the current mode/hours if they exist there, else sensible defaults.
void OrbitCabAudioProcessorEditor::selectAmpName (const juce::String& name)
{
    const auto cats = catsForName (name);
    if (cats.empty()) return;
    auto cat = cats.front();
    int  curHours = 0;
    if (auto* cur = ampEntryById (processorRef.selectedPowerampId()))
    {
        if (std::find (cats.begin(), cats.end(), cur->cat) != cats.end()) cat = cur->cat;
        curHours = cur->hours;
    }
    const auto hrs = hoursForNameCat (name, cat);
    const int hours = (std::find (hrs.begin(), hrs.end(), curHours) != hrs.end()) ? curHours : defaultAmpHour (hrs);
    const auto id = findAmpId (name, cat, hours);
    if (id.isNotEmpty()) { processorRef.selectPoweramp (id); syncAmpSelector(); }
}

void OrbitCabAudioProcessorEditor::selectAmpMode (orbitcab::PowerampCat c)
{
    auto* cur = ampEntryById (processorRef.selectedPowerampId());
    if (cur == nullptr) return;
    const auto hrs = hoursForNameCat (cur->name, c);
    const int hours = (std::find (hrs.begin(), hrs.end(), cur->hours) != hrs.end()) ? cur->hours : defaultAmpHour (hrs);
    const auto id = findAmpId (cur->name, c, hours);
    if (id.isNotEmpty()) { processorRef.selectPoweramp (id); syncAmpSelector(); }
}

void OrbitCabAudioProcessorEditor::selectAmpHours (int hours)
{
    auto* cur = ampEntryById (processorRef.selectedPowerampId());
    if (cur == nullptr) return;
    const auto id = findAmpId (cur->name, cur->cat, hours);
    if (id.isNotEmpty()) { processorRef.selectPoweramp (id); syncAmpSelector(); }
}

// (Re)create the hour segment buttons for the current name+mode (only when ≥2 positions exist).
void OrbitCabAudioProcessorEditor::rebuildHourSegments()
{
    for (auto& b : ampHourBtns) removeChildComponent (b.get());
    ampHourBtns.clear();
    ampHourVals.clear();

    auto* cur = ampEntryById (processorRef.selectedPowerampId());
    if (cur == nullptr) return;
    const auto hrs = hoursForNameCat (cur->name, cur->cat);
    if (hrs.size() < 2) return;   // 0/1 position → no segments

    for (int h : hrs)
    {
        auto b = std::make_unique<juce::TextButton> (juce::String (h) + "h");
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
        b->setTooltip ("Knob at " + juce::String (h) + " o'clock.");
        b->onClick = [this, h] { selectAmpHours (h); };
        addChildComponent (*b);
        ampHourBtns.push_back (std::move (b));
        ampHourVals.push_back (h);
    }
}

// Reflect the "ampSel" selection onto the whole selector: highlight the right name button (or the
// combo), show the contextual PP/SE toggle (only if the name has both) + hours segments (only if the
// name+mode has several), and the tubes. Called on a real change (click / restore), then re-flows.
void OrbitCabAudioProcessorEditor::syncAmpSelector()
{
    ampSyncedId = processorRef.selectedPowerampId();
    const bool on  = ampPowerBtn.getToggleState();
    auto* cur = ampEntryById (ampSyncedId);
    const bool group = cur != nullptr && isGroupName (cur->name);

    // name buttons — highlight the selected group's name
    for (size_t i = 0; i < ampNameBtns.size(); ++i)
        ampNameBtns[i]->setToggleState (group && cur->name == ampGroupNames[i], juce::dontSendNotification);

    // singletons combo — reflect (or clear) the selection
    if (cur != nullptr && ! group)
    {
        for (int i = 0; i < (int) ampLib.size(); ++i)
            if (ampLib[(size_t) i].id == cur->id) { ampSingleBox.setSelectedId (i + 1, juce::dontSendNotification); break; }
    }
    else
        ampSingleBox.setSelectedId (0, juce::dontSendNotification);

    // contextual PP/SE toggle — only for a group that actually has both modes
    const auto cats = (group ? catsForName (cur->name) : std::vector<orbitcab::PowerampCat>{});
    const bool showMode = on && group && cats.size() >= 2;
    for (int m = 0; m < 2; ++m)
    {
        const auto mc = (m == 0 ? orbitcab::PowerampCat::pushPull : orbitcab::PowerampCat::singleEnded);
        ampModeBtn[m].setVisible (showMode);
        ampModeBtn[m].setToggleState (cur != nullptr && cur->cat == mc, juce::dontSendNotification);
    }

    // contextual hours segments — rebuilt for this name+mode (only when ≥2 positions)
    rebuildHourSegments();
    for (size_t i = 0; i < ampHourBtns.size(); ++i)
    {
        ampHourBtns[i]->setVisible (on && group);
        ampHourBtns[i]->setToggleState (cur != nullptr && cur->hours == ampHourVals[i], juce::dontSendNotification);
    }

    // tubes: silhouette from the name, count from the mode (PP 2 / SE 1 / Other 0)
    tubeDisplay.setSelection (cur != nullptr ? tubeTypeFromName (cur->name) : 0,
                              cur != nullptr ? orbitcab::tubeCountForCat (cur->cat) : 0, on);

    resized();   // contextual controls appeared/disappeared → re-flow the row
}

void OrbitCabAudioProcessorEditor::updateAmpRow()
{
    const bool on = hasPoweramps && ampPowerBtn.getToggleState();   // empty library → never reveal
    ampOnCache = ampPowerBtn.getToggleState();

    // Powering on with no resolvable selection → default to the first library entry (so the stage
    // is never "on but silent"). A restored session that already carries a valid "ampSel" keeps it.
    if (on && ! ampLib.empty() && ampEntryById (processorRef.selectedPowerampId()) == nullptr)
        processorRef.selectPoweramp (ampLib.front().id);

    tubeDisplay.setShowTubes (showTubesPref);         // "Show tubes" hides the tubes but keeps the amp icon
    tubeDisplay.setVisible (on);                      // amp icon stays whenever the poweramp is on

    for (auto& b : ampNameBtns) b->setVisible (on);                       // name buttons always visible when on
    ampSingleBox.setVisible (on && ampSingleBox.getNumItems() > 0);       // combo only if there are singletons

    setSize (1040, kBaseHeight + (on ? ampRowH() : 0));   // grow/shrink (triggers resized when size changes)
    syncAmpSelector();                                    // contextual controls + tubes + final re-flow
}

void OrbitCabAudioProcessorEditor::pushFiltersToWave()
{
    slots[0].pushFiltersToWave();
    slots[1].pushFiltersToWave();
}

void OrbitCabAudioProcessorEditor::snapMixToCentre()
{
    if (auto* q = processorRef.apvts.getParameter ("mixAB"))
        q->setValueNotifyingHost (0.5f);
}

void OrbitCabAudioProcessorEditor::updateSnapshotButtons()
{
    const int a = processorRef.getActiveSnapshot();
    for (int i = 0; i < OrbitCabAudioProcessor::kNumSnapshots; ++i)
        snapBtn[i].setToggleState (i == a, juce::dontSendNotification);
}

void OrbitCabAudioProcessorEditor::switchSnapshot (int i)
{
    // Recall register i (the processor captures the live state into the active register first),
    // then re-sync the editor — shared by the A/B/C/D buttons and the 1-4 keyboard shortcuts.
    processorRef.switchToSnapshot (i);
    slots[0].syncFromProcessor();          // recall may have swapped the IRs / refs
    slots[1].syncFromProcessor();
    pushFiltersToWave();
    updateSnapshotButtons();
}

bool OrbitCabAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    // 1/2/3/4 → snapshot A/B/C/D. Only the digit row (no modifiers) so it won't fight typing
    // in a text field (a modal name prompt grabs focus anyway) or common host shortcuts.
    for (int i = 0; i < OrbitCabAudioProcessor::kNumSnapshots; ++i)
        if (key == juce::KeyPress ((juce::juce_wchar) ('1' + i)))
        {
            switchSnapshot (i);
            return true;
        }
    return false;
}

void OrbitCabAudioProcessorEditor::afterUndoRedo()
{
    // undo/redo replaced params + IR refs in the processor; reflect it in the editor.
    slots[0].syncFromProcessor();
    slots[1].syncFromProcessor();
    pushFiltersToWave();
    updateSnapshotButtons();
    undoBtn.setEnabled (processorRef.canUndo());
    redoBtn.setEnabled (processorRef.canRedo());
}

//==============================================================================
// Presets
//==============================================================================
void OrbitCabAudioProcessorEditor::refreshPresets()
{
    presetFiles.clear();
    presetBox.clear (juce::dontSendNotification);

    // FACTORY section (read-only, bundled). "Default" (id 2) is an alias to the bundled default
    // (kDefaultPresetName); the rest live in the "OrbitCab" submenu (ids 100+ = factoryList index).
    // ComboBox::getItemForId searches the menu recursively, so submenu items select/display fine.
    // (Future: bucket into more banks — e.g. a "Darwin's Cat" submenu — by a meta field.)
    factoryList = orbitcab::factoryPresets();
    presetBox.addItem ("Default", 2);
    juce::PopupMenu orbitcabBank;
    for (int i = 0; i < (int) factoryList.size(); ++i)
        if (factoryList[(size_t) i].name != orbitcab::kDefaultPresetName)   // the default shows once, as "Default"
            orbitcabBank.addItem (100 + i, factoryList[(size_t) i].name);
    presetBox.getRootMenu()->addSubMenu ("OrbitCab", orbitcabBank);

    // USER section (ids 1000+). Hide any user file whose name matches a bundled factory preset —
    // factory takes precedence, so the author's own copies of now-shipped presets don't double up.
    juce::StringArray facNames;
    for (const auto& fp : factoryList) facNames.add (fp.name);
    juce::Array<juce::File> userFiles;
    for (const auto& f : presets.list())
        if (! facNames.contains (f.getFileNameWithoutExtension()))
            userFiles.add (f);
    if (! userFiles.isEmpty())
    {
        presetBox.addSeparator();                      // factory ──────── user
        int id = 1000;                                 // 1 = current-external, 100+ = factory, 1000+ = user files
        for (const auto& f : userFiles)
        {
            presetFiles.add (f);
            presetBox.addItem (f.getFileNameWithoutExtension(), id++);
        }
    }

    // Only when the current preset is "external" (imported/dropped, or a pre-v3 session — not
    // Default and not a library file) do we add a slot for it (labelled by its name, or
    // "(Custom)" if unnamed). No permanent ghost entry in the common case.
    if (! processorRef.isPresetFactory() && currentPresetFile() == juce::File())
    {
        const auto cur = processorRef.presetMeta().name;
        presetBox.addSeparator();
        presetBox.addItem (cur.isNotEmpty() ? cur : "(Custom)", 1);
    }

    presetShownId = -1;                                 // force a re-apply after the rebuild
    updatePresetDisplay();                              // select the current preset by name (+ dirty)
}

void OrbitCabAudioProcessorEditor::promptSavePreset (const juce::String& initialName)
{
    saveDialog = std::make_unique<juce::AlertWindow> ("Save preset", "Preset name:",
                                                      juce::MessageBoxIconType::NoIcon);
    saveDialog->addTextEditor ("name", initialName.isNotEmpty() ? initialName : juce::String ("My Preset"));
    saveDialog->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
    saveDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    saveDialog->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, safe = juce::Component::SafePointer<OrbitCabAudioProcessorEditor> (this)] (int result)
        {
            if (safe == nullptr) return;   // editor gone before the dialog closed
            const auto name = saveDialog->getTextEditorContents ("name").trim();
            saveDialog.reset();
            if (result != 1 || name.isEmpty())
                return;

            // Never shadow a factory preset name, and never silently overwrite an existing user
            // preset: show an error and re-prompt (prefilled) so the user picks a different name.
            bool reserved = false;
            for (const auto& fp : factoryList)
                if (name.equalsIgnoreCase (fp.name)) { reserved = true; break; }
            const auto target   = orbitcab::PresetManager::directory()
                                      .getChildFile (juce::File::createLegalFileName (name) + ".orbitcab");
            if (reserved || target.existsAsFile())
            {
                const juce::String msg = (reserved ? "\"" + name + "\" is a factory preset name."
                                                   : "A preset named \"" + name + "\" already exists.")
                                         + "\nChoose a different name.";
                saveDialog = std::make_unique<juce::AlertWindow> ("Name already in use", msg,
                                                                  juce::MessageBoxIconType::WarningIcon);
                saveDialog->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                saveDialog->enterModalState (true, juce::ModalCallbackFunction::create (
                    [this, name, safe2 = juce::Component::SafePointer<OrbitCabAudioProcessorEditor> (this)] (int)
                    {
                        if (safe2 == nullptr) return;
                        saveDialog.reset();
                        promptSavePreset (name);   // re-prompt, prefilled, so they can tweak it
                    }), false);
                return;
            }

            loadedPresetFile = presets.saveAs (name);   // becomes the current backing file (Save target)
            refreshPresets();          // rebuild list; updatePresetDisplay selects it by name
        }), false);
}

void OrbitCabAudioProcessorEditor::loadPresetFile (const juce::File& file)
{
    if (! presets.loadFrom (file))                 // applies state + re-baselines (clean, non-factory)
        return;
    loadedPresetFile = file;                        // the Save/Delete target (library file → writeable; external → not)
    slots[0].rebuildList();                        // restored user-IR history may differ
    slots[1].rebuildList();
    slots[0].syncFromProcessor();
    slots[1].syncFromProcessor();
    pushFiltersToWave();
    updateSnapshotButtons();
    refreshPresets();                              // rebuild (adds/removes the external slot) + select
}

void OrbitCabAudioProcessorEditor::loadFactoryPreset (const orbitcab::FactoryPreset& p)
{
    // Bundled read-only preset: the processor loads it + forces the factory flag; then re-sync the
    // editor. No backing file (Save disabled; Save As forks). updatePresetDisplay selects it by name.
    processorRef.loadFactoryPresetState (p.data, p.size);
    loadedPresetFile = juce::File();
    slots[0].rebuildList();
    slots[1].rebuildList();
    slots[0].syncFromProcessor();
    slots[1].syncFromProcessor();
    pushFiltersToWave();
    updateSnapshotButtons();
    refreshPresets();
}

void OrbitCabAudioProcessorEditor::exportPreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Export preset",
                                                   orbitcab::PresetManager::directory().getChildFile ("My Preset.orbitcab"), "*.orbitcab");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
        [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file == juce::File())
                return;                                   // cancelled
            presets.writeTo (file);

            // Light, factual heads-up — only when the preset ACTUALLY carries embedded audio:
            // an external (user) IR and/or the poweramp .nam (v5). A preset of only bundled refs
            // with no amp embeds nothing → no popup. Wording reflects what's really in there.
            const bool embIR  = processorRef.exportEmbedsIR();
            const bool embAmp = processorRef.exportEmbedsAmp();
            if (embIR || embAmp)
            {
                const juce::String what = (embIR && embAmp) ? "its IR and the poweramp capture"
                                        : embAmp             ? "the poweramp capture"
                                                             : "its IR audio";
                juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                    "Preset carries embedded audio",
                    "This preset embeds " + what + ", so it travels inside the .orbitcab file.");
            }
        });
}

void OrbitCabAudioProcessorEditor::importPreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Import preset", orbitcab::PresetManager::directory(), "*.orbitcab");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (! file.existsAsFile())
                return;
            loadPresetFile (file);     // updatePresetDisplay reflects the imported preset's name
        });
}

bool OrbitCabAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".orbitcab"))
            return true;
    return false;
}

void OrbitCabAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".orbitcab"))
        {
            loadPresetFile (juce::File (f));   // updatePresetDisplay reflects the dropped preset's name
            break;
        }
}

void OrbitCabAudioProcessorEditor::applyDefaultPreset()
{
    // The factory "Default" is defined once in the processor (preset-centric: it's also the
    // first-start preset — Emerald IR #16 + HPF, single box, read-only). Apply it there, then
    // re-sync the editor to the new IR/params and mark the combo Default (clean).
    processorRef.applyFactoryDefault();
    loadedPresetFile = juce::File();               // the factory Default has no backing file
    slots[0].syncFromProcessor();
    slots[1].syncFromProcessor();
    pushFiltersToWave();
    updateSnapshotButtons();
    refreshPresets();                              // drop any external slot + select Default
}

void OrbitCabAudioProcessorEditor::saveCurrentPreset()
{
    // Save = write back to the current user preset. The button is disabled for a factory
    // preset (Default) or an external one (no backing file) — Save As is the path there — but
    // guard anyway so a stray call is a no-op rather than an overwrite.
    const auto file = currentPresetFile();
    if (processorRef.isPresetFactory() || file == juce::File())
        return;
    if (presets.writeTo (file))        // re-serialise the (portable) preset over its file
    {
        processorRef.captureBaseline();    // saved → clean again
        updatePresetDisplay();
    }
}

void OrbitCabAudioProcessorEditor::deleteCurrentPreset()
{
    // Only a user preset with a backing file can be deleted; factory Default + external presets
    // are read-only (the button is disabled for them anyway). Confirm, then move it to the Trash.
    const auto file = currentPresetFile();
    if (processorRef.isPresetFactory() || file == juce::File())
        return;

    const auto name = processorRef.presetMeta().name;
    saveDialog = std::make_unique<juce::AlertWindow> ("Delete preset",
                     "Delete \"" + name + "\"?  It will be moved to the Trash.",
                     juce::MessageBoxIconType::WarningIcon);
    saveDialog->addButton ("Delete", 1, juce::KeyPress (juce::KeyPress::returnKey));
    saveDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    saveDialog->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, file, safe = juce::Component::SafePointer<OrbitCabAudioProcessorEditor> (this)] (int result)
        {
            if (safe == nullptr) return;          // editor gone before the dialog closed
            if (result == 1 && presets.deleteFile (file))
                applyDefaultPreset();             // current preset gone → fall back to Default (refreshes the list)
            saveDialog.reset();
        }), false);
}

juce::String OrbitCabAudioProcessorEditor::nextCopyName (const juce::String& base) const
{
    // "<root> (copy N)" with the first N (from 1) whose file doesn't already exist — so the
    // Save As field is prefilled with a name that won't trip the duplicate-name guard. Strips an
    // existing " (copy N)" first, so copying a copy gives "X (copy 2)", not "X (copy 1) (copy 1)".
    juce::String root = base.trim();
    const int open = root.lastIndexOf (" (copy ");
    if (open >= 0 && root.endsWithChar (')'))
    {
        const auto inner = root.substring (open + 7, root.length() - 1);   // digits between "(copy " and ")"
        if (inner.isNotEmpty() && inner.containsOnly ("0123456789"))
            root = root.substring (0, open).trim();
    }
    if (root.isEmpty())
        root = "My Preset";

    for (int n = 1; ; ++n)
    {
        const auto cand = root + " (copy " + juce::String (n) + ")";
        const auto f = orbitcab::PresetManager::directory()
                           .getChildFile (juce::File::createLegalFileName (cand) + ".orbitcab");
        if (! f.existsAsFile() && ! cand.equalsIgnoreCase ("Default"))
            return cand;
    }
}

juce::File OrbitCabAudioProcessorEditor::currentPresetFile() const
{
    // The library file backing the current preset — the Save / Delete target. A factory preset
    // (Default) or an imported/external one has none → Save As territory.
    if (processorRef.isPresetFactory())
        return {};

    // We know exactly which file this preset came from this session: trust it (collision-safe).
    if (loadedPresetFile != juce::File())
        return (loadedPresetFile.existsAsFile()
                && loadedPresetFile.isAChildOf (orbitcab::PresetManager::directory()))
                   ? loadedPresetFile : juce::File();   // imported/external → no library backing

    // Unknown backing (e.g. a host session reload recreated the editor) → best-effort name match.
    const auto name = processorRef.presetMeta().name;
    for (const auto& f : presetFiles)
        if (f.getFileNameWithoutExtension() == name)
            return f;
    return {};
}

void OrbitCabAudioProcessorEditor::updatePresetDisplay()
{
    // Reflect the current preset in the combo: select the matching item (Default / a library
    // file / "(Custom)" for an external one) and append " *" when the live state is dirty.
    // Cached so the 30 Hz timer only touches the combo when something actually changed.
    const auto name  = processorRef.presetMeta().name;
    const bool dirty = processorRef.isPresetDirty();

    // Save (write-back) + Delete are for a user preset only — a factory preset (Default) and an
    // external one have no backing library file. Save As is the path to persist those (fork).
    const bool canWriteBack = ! processorRef.isPresetFactory() && currentPresetFile() != juce::File();
    saveBtn.setEnabled  (canWriteBack);
    trashBtn.setEnabled (canWriteBack);

    int          id   = 1;                                   // 1 = current external / "(Custom)"
    juce::String base = name.isNotEmpty() ? name : "(Custom)";
    if (processorRef.isPresetFactory())                      // a bundled factory preset
    {
        if (name == orbitcab::kDefaultPresetName)            // the default shows as "Default" (id 2)
        {
            id   = 2;
            base = "Default";
        }
        else                                                 // the rest live in the OrbitCab bank (ids 100+)
            for (int i = 0; i < (int) factoryList.size(); ++i)
                if (factoryList[(size_t) i].name == name)
                {
                    id = 100 + i;
                    break;
                }
    }
    else                                                     // a user library file (ids 1000+)
    {
        for (int i = 0; i < presetFiles.size(); ++i)
            if (presetFiles[i].getFileNameWithoutExtension() == name)
            {
                id = 1000 + i;
                break;
            }
    }

    const juce::String label = base + (dirty ? " *" : "");
    if (id == presetShownId && label == presetShownLabel)
        return;
    presetShownId    = id;
    presetShownLabel = label;

    presetBox.changeItemText (id, label);
    presetBox.setSelectedId  (id, juce::dontSendNotification);
}

//==============================================================================
void OrbitCabAudioProcessorEditor::paint (juce::Graphics& g)
{
    // main background — subtle top-lit gradient
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff141418), 0.0f, 0.0f,
                                             juce::Colour (0xff0b0b0d), 0.0f, (float) getHeight(), false));
    g.fillAll();

    auto vpanel = [&g] (juce::Rectangle<float> rect, juce::uint32 top, juce::uint32 bot, float corner)
    {
        g.setGradientFill (juce::ColourGradient (juce::Colour (top), rect.getX(), rect.getY(),
                                                 juce::Colour (bot), rect.getX(), rect.getBottom(), false));
        if (corner > 0.0f) g.fillRoundedRectangle (rect, corner);
        else               g.fillRect (rect);
    };

    // header + bottom MIX strip
    vpanel (getLocalBounds().removeFromTop (50).toFloat(),    0xff24242b, 0xff17171c, 0.0f);
    vpanel ((mixStripBounds.isEmpty() ? getLocalBounds().removeFromBottom (46) : mixStripBounds).toFloat(),
            0xff24242b, 0xff17171c, 0.0f);
    if (! ampRowBounds.isEmpty())                                  // revealed poweramp row (darker shade)
        vpanel (ampRowBounds.toFloat(), 0xff1f1f26, 0xff141419, 0.0f);

    // INPUT / OUTPUT side panels (rounded, gradient, faint border)
    for (auto rect : { inBlockBounds, outBlockBounds })
        if (! rect.isEmpty())
        {
            vpanel (rect.toFloat().reduced (2.0f), 0xff1e1e24, 0xff131317, 6.0f);
            g.setColour (juce::Colour (0x18ffffff));
            g.drawRoundedRectangle (rect.toFloat().reduced (2.0f), 6.0f, 1.0f);
        }

    // (the brand logo + title are drawn by the HeaderBrand component now)

    // MIX track: an A→B (violet→orange) gradient when both slots play, else a neutral
    // rail (the slider's own track is transparent; its thumb draws on top).
    if (! mixABSlider.getBounds().isEmpty())
    {
        // track area excludes the ±dB textbox on the right
        auto trackArea = mixABSlider.getBounds().toFloat().withTrimmedRight (62.0f);
        const auto track = trackArea.withSizeKeepingCentre (trackArea.getWidth(), 5.0f);
        if (mixABSlider.isEnabled())
            g.setGradientFill (juce::ColourGradient (juce::Colour (OrbitCabLookAndFeel::kAccent),  track.getX(), track.getY(),
                                                     juce::Colour (OrbitCabLookAndFeel::kAccentB), track.getRight(), track.getY(), false));
        else
            g.setColour (juce::Colour (0x40c0c0c8));
        g.fillRoundedRectangle (track, 2.5f);

        // centre (0) marker
        const float cx = track.getCentreX();
        g.setColour (juce::Colour (0x88ffffff));
        g.fillRect (cx - 0.5f, track.getY() - 4.0f, 1.0f, track.getHeight() + 8.0f);
        g.setFont (juce::FontOptions (8.0f, juce::Font::bold));
        g.drawText ("0", juce::Rectangle<float> (cx - 6.0f, track.getY() - 14.0f, 12.0f, 9.0f),
                    juce::Justification::centred, false);
    }
}

void OrbitCabAudioProcessorEditor::resized()
{
    auto r = getLocalBounds();

    // ---- header ----
    // Taller header so the brand logo can be big — but the small controls keep their
    // original height by living in a 44-tall band centred vertically in the header (else
    // they stretch with the taller strip). Only the brand uses the full header height.
    auto header = r.removeFromTop (50);
    constexpr int kCtlBand = 44;

    // Right cluster narrower now that Save/Save As are icons (not text) — that frees the gap
    // on the left so undo/redo are visible again in their original spot (see leftBar below).
    auto headerRight = header.removeFromRight (580);   // save+saveAs+trash+export+import+gear + A/B/C/D + combo
    auto rightBar    = headerRight.withSizeKeepingCentre (headerRight.getWidth(), kCtlBand);
    settingsBtn.setBounds (rightBar.removeFromRight (40).reduced (7));
    importBtn.setBounds   (rightBar.removeFromRight (34).reduced (5, 7));
    exportBtn.setBounds   (rightBar.removeFromRight (34).reduced (5, 7));
    trashBtn.setBounds    (rightBar.removeFromRight (40).reduced (4, 7));   // framed icons, grouped
    saveAsBtn.setBounds   (rightBar.removeFromRight (40).reduced (4, 7));
    saveBtn.setBounds     (rightBar.removeFromRight (40).reduced (4, 7));
    // A/B/C/D compare cluster sits just left of the preset combo (between it and the title)
    auto snapArea = rightBar.removeFromLeft (124).reduced (6, 8);
    const int sw = snapArea.getWidth() / OrbitCabAudioProcessor::kNumSnapshots;
    for (int i = 0; i < OrbitCabAudioProcessor::kNumSnapshots; ++i)
        snapBtn[i].setBounds ((i < OrbitCabAudioProcessor::kNumSnapshots - 1
                                   ? snapArea.removeFromLeft (sw) : snapArea).reduced (2, 0));
    presetBox.setBounds   (rightBar.reduced (7));
    // brand on the left, sized to its content so the click area hugs the text (full height)
    header.removeFromLeft (4);
    brand.setBounds (header.removeFromLeft (juce::jmin (header.getWidth(),
                                                        brand.preferredWidth (header.getHeight()))));
    // undo / redo in the gap between the brand and the A/B/C/D cluster (centred band)
    header.removeFromLeft (14);
    auto leftBar = header.withSizeKeepingCentre (header.getWidth(), kCtlBand);
    undoBtn.setBounds (leftBar.removeFromLeft (34).reduced (3, 8));   // ~ same box as A/B/C/D
    redoBtn.setBounds (leftBar.removeFromLeft (34).reduced (3, 8));

    // ---- MIX strip (bottom centre) ----
    // Revealed POWERAMP row at the very bottom — present only when AMP power is on.
    if (hasPoweramps && ampPowerBtn.getToggleState())
    {
        ampRowBounds = r.removeFromBottom (ampRowH());
        auto row = ampRowBounds.reduced (16, 10);
        // amp icon always; +tube area when Show-tubes is on (wider, taller row)
        tubeDisplay.setBounds (row.removeFromLeft (showTubesPref ? 152 : 52));
        row.removeFromLeft (16);

        // Laid out from the RIGHT: singletons combo, then contextual hours segments, then PP/SE.
        // The grouped NAME buttons fill whatever's left in the middle.
        if (ampSingleBox.isVisible())
        {
            ampSingleBox.setBounds (row.removeFromRight (160).withSizeKeepingCentre (160, 28));
            row.removeFromRight (14);
        }
        // hours segments (visible ones)
        {
            int nh = 0; for (auto& b : ampHourBtns) if (b->isVisible()) ++nh;
            if (nh > 0)
            {
                constexpr int hw = 42, hg = 3;
                auto ha = row.removeFromRight (nh * hw + (nh - 1) * hg).withSizeKeepingCentre (nh * hw + (nh - 1) * hg, 26);
                for (auto& b : ampHourBtns) if (b->isVisible()) { b->setBounds (ha.removeFromLeft (hw)); ha.removeFromLeft (hg); }
                row.removeFromRight (12);
            }
        }
        // PP / SE toggle (visible ones)
        {
            int nm = 0; for (auto& b : ampModeBtn) if (b.isVisible()) ++nm;
            if (nm > 0)
            {
                constexpr int mw = 46, mg = 3;
                auto ma = row.removeFromRight (nm * mw + (nm - 1) * mg).withSizeKeepingCentre (nm * mw + (nm - 1) * mg, 26);
                for (auto& b : ampModeBtn) if (b.isVisible()) { b.setBounds (ma.removeFromLeft (mw)); ma.removeFromLeft (mg); }
                row.removeFromRight (14);
            }
        }

        // Grouped NAME buttons fill the middle, vertically centred, sharing the remaining width.
        auto ctl = row.withSizeKeepingCentre (row.getWidth(), 30);
        const int n = (int) ampNameBtns.size();
        if (n > 0)
        {
            const int gap = 6;
            const int bw  = juce::jlimit (44, 132, (ctl.getWidth() - gap * (n - 1)) / n);
            for (auto& b : ampNameBtns)
            {
                b->setBounds (ctl.removeFromLeft (bw).reduced (1, 0));
                ctl.removeFromLeft (gap);
            }
        }
    }
    else
        ampRowBounds = {};

    auto strip = r.removeFromBottom (46);
    mixStripBounds = strip;                       // repaint region for the A→B gradient
    versionBadge.setBounds (strip.removeFromRight (96).reduced (12, 15));   // version always bottom-right
    if (hasPoweramps)
        ampPowerBtn.setBounds (strip.removeFromLeft (108).reduced (12, 12));   // bottom-left POWERAMP checkbox
    mixABLabel.setBounds   (strip.removeFromLeft (146).reduced (10, 0));
    mixABSlider.setBounds  (strip.reduced (40, 12));
    // With snap-to-mouse off the drag is relative; default sensitivity (250 px = full
    // range) makes the thumb outrun the cursor on this wide strip. Match the sensitivity
    // to the visible track width (= bounds minus the 62 px text box) so dragging is ~1:1.
    mixABSlider.setMouseDragSensitivity (juce::jmax (1, mixABSlider.getWidth() - 62));

    // ---- INPUT / OUTPUT side blocks ----
    auto inCol  = r.removeFromLeft (80);   inBlockBounds  = inCol;
    auto outCol = r.removeFromRight (80);  outBlockBounds = outCol;
    auto inBlock  = inCol.reduced (4, 6);
    auto outBlock = outCol.reduced (4, 6);

    bypassBtn.setBounds (inBlock.removeFromTop (22));
    inLabel.setBounds   (inBlock.removeFromBottom (14));
    inMeter.setBounds   (inBlock.removeFromRight (26));
    inGainFader.setBounds (inBlock);

    autoBtn.setBounds   (outBlock.removeFromTop (22));
    outLabel.setBounds  (outBlock.removeFromBottom (14));
    outMeter.setBounds  (outBlock.removeFromLeft (26));
    masterFader.setBounds (outBlock);

    // ---- slots A | B ---- (each SlotComponent lays out its own internals)
    auto left = r.removeFromLeft (r.getWidth() / 2);
    slots[0].setBounds (left);
    slots[1].setBounds (r);
}
