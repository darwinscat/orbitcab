// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "PluginEditor.h"
#include "IRLibrary.h"    // bundled-IR enumeration (shared with the processor)
#include "BinaryData.h"
#include "core/AmpEq.h"   // cab::EqParams + cab::AmpEq::describe — feed the live EQ curve

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

    // Preamp names treated as "clean"/near-transparent front-ends (the ISA studio pre). Single source of
    // truth for both the selector (curated to the bottom next to BYPASS) and the default pick (preferred
    // over the voiced amp models). Prefix-matched so any "ISA*" label counts.
    inline bool preampNameIsClean (const juce::String& nm) { return nm.startsWith ("ISA"); }
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

    versionBadge.setBrandTypeface (brand.wordmarkTypeface);   // Michroma for the (i) popover title mark
    addAndMakeVisible (versionBadge);   // bottom version + opt-in update check
    addAndMakeVisible (perfBadge);      // latency + DSP load, left of the version

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
        b.getProperties().set ("orbitStockToggle", true);           // neutral two-fill toggle — keep the stock look, not the mode-button border
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

    // ---- POWERAMP CAPTURES (NAM): power checkbox (bottom strip) + revealed selector row ----
    // CAPTURES + SIMULATOR are a RADIO over the ONE poweramp slot (ampOn + ampMode) — no APVTS attachment;
    // the toggle is set by syncPowerAmpTabs(), and clicking it powers the slot in CAPTURE mode (SIMULATOR off).
    ampPowerBtn.setTooltip (juce::String::fromUTF8 ("NAM poweramp captures in front of the cab \xe2\x80\x94 the model selector below. Shares the poweramp slot with POWER AMP SIMULATOR (only one runs)."));
    ampPowerBtn.setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
    addAndMakeVisible (ampPowerBtn);
    ampPowerBtn.onClick = [this]
    {
        auto set = [this] (const char* id, float v) { if (auto* p = processorRef.apvts.getParameter (id)) p->setValueNotifyingHost (v); };
        if (ampPowerBtn.getToggleState()) { set ("ampMode", 0.0f); set ("ampOn", 1.0f); }   // → CAPTURE, poweramp on
        else                                set ("ampOn", 0.0f);                              // → poweramp off
        syncPowerAmpTabs();
    };

    addAndMakeVisible (tubeDisplay);   // symbolic amp + glowing tube(s) in the revealed row

    // Contextual PP / SE toggle — shown only when the selected NAME has both modes. Param-free,
    // driven by syncAmpSelector (no self-toggle). [0] = push-pull, [1] = single-ended.
    static const char* const kModeNames[2] = { "PP", "SE" };
    for (int m = 0; m < 2; ++m)
    {
        ampModeBtn[m].setButtonText (kModeNames[m]);
        ampModeBtn[m].setClickingTogglesState (false);
        ampModeBtn[m].setColour (juce::TextButton::buttonOnColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
        ampModeBtn[m].setTooltip (juce::String::fromUTF8 (
                                      m == 0 ? "Push-pull: tighter, punchy \xe2\x80\x94 better for rhythm"
                                             : "Single-ended: sweeter, singing \xe2\x80\x94 better for melodic solos"));
        ampModeBtn[m].onClick = [this, m] { selectAmpMode (m == 0 ? orbitcab::PowerampCat::pushPull
                                                                   : orbitcab::PowerampCat::singleEnded); };
        addChildComponent (ampModeBtn[m]);
    }

    // Contextual hours: a horizontal discrete slider snapping to the available "<N>h" positions of the
    // current name+mode. The slider value is the INDEX into ampHourVals (so non-uniform clock stops
    // like 9/12/15/16 snap cleanly); the text box shows the hour. Rebuilt by configureHourSlider.
    ampHourSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    ampHourSlider.setTextBoxStyle (juce::Slider::TextBoxRight, true, 38, 22);   // read-only, shows "12h"
    ampHourSlider.setColour (juce::Slider::trackColourId,        juce::Colour (OrbitCabLookAndFeel::kAccent));
    ampHourSlider.setColour (juce::Slider::thumbColourId,        juce::Colour (OrbitCabLookAndFeel::kAccent));
    ampHourSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0x00000000));
    ampHourSlider.setTooltip ("Knob position the capture was taken at (o'clock).");
    ampHourSlider.textFromValueFunction = [this] (double v)
    {
        const int i = (int) std::lround (v);
        return juce::isPositiveAndBelow (i, (int) ampHourVals.size()) ? juce::String (ampHourVals[(size_t) i]) + "h" : juce::String();
    };
    ampHourSlider.valueFromTextFunction = [] (const juce::String& t) { return (double) t.getIntValue(); };   // unused (read-only)
    ampHourSlider.onValueChange = [this]
    {
        const int i = (int) std::lround (ampHourSlider.getValue());
        if (juce::isPositiveAndBelow (i, (int) ampHourVals.size()))
            selectAmpHours (ampHourVals[(size_t) i]);
    };
    addChildComponent (ampHourSlider);

    // Singletons combo (one-off amps by full filename). Selecting an item picks that capture.
    ampSingleBox.setTextWhenNothingSelected (juce::String::fromUTF8 ("Amps\xe2\x80\xa6"));
    ampSingleBox.setTooltip ("One-off poweramp captures (not part of a tube family).");
    ampSingleBox.onChange = [this]
    {
        const int idx = ampSingleBox.getSelectedId() - 1;            // ids are 1-based → entry index
        if (juce::isPositiveAndBelow (idx, (int) ampLib.size()))
        { processorRef.selectPoweramp (ampLib[(size_t) idx].id); syncAmpSelector(); }
    };
    addChildComponent (ampSingleBox);

    rebuildAmpSelector();              // scan the merged library → name buttons + combo + hasPoweramps + power-btn visibility

    // ---- PREAMP (NAM): power checkbox (bottom strip) + revealed selector row (above the poweramp) ----
    preampPowerBtn.setTooltip (juce::String::fromUTF8 ("Power the NAM preamp stage \xe2\x80\x94 the first neural stage, in front of the poweramp. Reveals the model selector below."));
    preampPowerBtn.setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
    addAndMakeVisible (preampPowerBtn);
    preampPowerAtt = std::make_unique<BAtt> (processorRef.apvts, "preampOn", preampPowerBtn);
    preampPowerBtn.onClick = [this] { updatePreampRow(); };       // reveal/hide the row + resize on toggle

    addChildComponent (preampTubeDisplay);   // device glyph removed from the preamp strip — kept as a hidden child

    // Contextual channel switch — shown only for the channels the selected NAME has (and only when ≥2
    // exist). Each slot's caption, colour tint and channel value are data-driven in syncPreampSelector
    // (a slot may be "CH 2" or a colour like "RED" that glows in the unit's channel colour). Param-free,
    // no self-toggle; clicking a slot selects whatever channel it currently maps to.
    for (int m = 0; m < 4; ++m)
    {
        preampChannelBtn[m].setClickingTogglesState (false);
        preampChannelBtn[m].setColour (juce::TextButton::buttonOnColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
        preampChannelBtn[m].onClick = [this, m] { selectPreampChannel (preampChannelBtnCh[m]); };
        addChildComponent (preampChannelBtn[m]);
    }

    // Contextual gain: a horizontal discrete slider snapping to the available "<N>h" positions of the
    // current name+channel. One-for-one with the poweramp's hour slider, just labelled "gain".
    // GAIN — an ORANGE discrete rotary (the brand's Slot-B orange, distinct from the violet EQ knobs),
    // its stops the captured clock positions (7h…17h); the read-out shows the hour. "By the clock".
    preampGainSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    preampGainSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);   // value ("12h") drawn in the dial centre
    preampGainSlider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (OrbitCabLookAndFeel::kAccentB));
    preampGainSlider.setColour (juce::Slider::thumbColourId,            juce::Colour (OrbitCabLookAndFeel::kAccentB));
    preampGainSlider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3a3a40));
    preampGainSlider.getProperties().set ("clockTicks", true);   // draw clock marks around the dial (one per gain stop)
    preampGainSlider.setTooltip ("Preamp gain — the knob position the capture was taken at (o'clock).");
    preampGainSlider.textFromValueFunction = [this] (double v)
    {
        const int i = (int) std::lround (v);
        return juce::isPositiveAndBelow (i, (int) preampGainVals.size()) ? juce::String (preampGainVals[(size_t) i]) + "h" : juce::String();
    };
    preampGainSlider.valueFromTextFunction = [] (const juce::String& t) { return (double) t.getIntValue(); };   // unused (read-only)
    preampGainSlider.onValueChange = [this]
    {
        const int i = (int) std::lround (preampGainSlider.getValue());
        if (juce::isPositiveAndBelow (i, (int) preampGainVals.size()))
            selectPreampGain (preampGainVals[(size_t) i]);
    };
    addChildComponent (preampGainSlider);

    // Contextual boost toggle — shown only when the current name+channel+gain has BOTH an on- and an
    // off-capture. Param-free (it switches between two captures, like the channel switch).
    preampBoostBtn.setClickingTogglesState (false);
    // Boost reads as an aggressive control: hot red — faint when idle, vivid when engaged.
    {
        const juce::Colour boostRed (0xffe23b3b);
        preampBoostBtn.setColour (juce::TextButton::buttonColourId,   boostRed.withMultipliedSaturation (0.6f).withMultipliedBrightness (0.4f));
        preampBoostBtn.setColour (juce::TextButton::buttonOnColourId, boostRed);
    }
    preampBoostBtn.setTooltip ("Boost — switch to the boosted (or clean) capture of this preamp.");
    preampBoostBtn.onClick = [this]
    {
        if (auto* cur = preampSel.entryById (processorRef.selectedPreampId()))
            selectPreampBoost (! cur->boost);
    };
    addChildComponent (preampBoostBtn);

    // NAME picker — ONE combo listing model families and one-off singletons, grouped Factory / User.
    // Picking a family resolves to its current channel/gain/boost variant (the contextual controls to the
    // right then refine it); picking a singleton selects that capture directly. preampBoxTargets maps each
    // 1-based item id to {isFamily, name|id}.
    preampBox.setTextWhenNothingSelected (juce::String::fromUTF8 ("Preamp\xe2\x80\xa6"));
    preampBox.setTooltip ("Preamp model — Factory (embedded) and your User captures.");
    preampBox.onChange = [this]
    {
        const int idx = preampBox.getSelectedId() - 1;            // item id (1-based) → preampBoxTargets index
        if (! juce::isPositiveAndBelow (idx, (int) preampBoxTargets.size())) return;
        const auto& t = preampBoxTargets[(size_t) idx];
        if (t.first) selectPreampName (t.second);                 // a family → keep channel/gain/boost if they exist there
        else { processorRef.selectPreamp (t.second); syncPreampSelector(); }   // a singleton's entry id
    };
    addChildComponent (preampBox);

    // Gear caption ABOVE the combo + device glyph strip BELOW it — both driven by the model metadata.
    preampGearLabel.setJustificationType (juce::Justification::centred);
    preampGearLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    preampGearLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc4bbaa));
    preampGearLabel.setInterceptsMouseClicks (false, false);
    preampGearLabel.setMinimumHorizontalScale (0.7f);
    addChildComponent (preampGearLabel);
    addChildComponent (preampDeviceStrip);

    // Device glyphs inside the combo dropdown — the popup L&F looks each item up in preampItemDevice.
    preampMenuLnf.deviceFor = [this] (const juce::String& itemText) -> orbitcab::ui::DeviceSpec
    {
        const auto it = preampItemDevice.find (itemText);
        return it != preampItemDevice.end() ? it->second : orbitcab::ui::DeviceSpec {};
    };
    preampBox.setLookAndFeel (&preampMenuLnf);

    rebuildPreampSelector();           // scan the merged preamp library → name buttons + combo + hasPreamps + power-btn visibility

    // ---- AMP EQ: tone stack (Bass/Mid/Treble) + Presence + HPF/LPF, revealed row + bottom toggle ----
    eqPowerBtn.setTooltip (juce::String::fromUTF8 ("Amp tone EQ between the preamp and poweramp \xe2\x80\x94 reveals the knobs below."));
    eqPowerBtn.setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
    addChildComponent (eqPowerBtn);   // HIDDEN — the tone EQ is now part of the preamp; this just mirrors preampOn → eqOn
    eqPowerAtt = std::make_unique<BAtt> (processorRef.apvts, "eqOn", eqPowerBtn);
    eqPowerBtn.onClick = [this] { updateEqRow(); };          // show/hide the tone controls + resize

    // tone = the Bass/Mid/Treble/Presence knobs (violet, ±dB, discrete 1 dB steps when the pref is on).
    // The value box INSIDE the knob shows the UNIT (dB / Hz / kHz) — NOT a number; double-clicking to
    // edit shows a bare number (no unit). The model name sits in the caption above (HPF/LPF: the toggle).
    auto setupKnob = [this] (CentreUnitSlider& k, const juce::String& paramId, std::unique_ptr<SAtt>& att,
                             const juce::String& tip, bool tone, const juce::String& unit)
    {
        k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 52, 14);   // NUMBER below; double-click it to type a value
        k.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0x00000000));
        k.getProperties().set ("unit", unit);   // UNIT drawn in the dial centre by the look-and-feel
        const auto fill = juce::Colour (tone ? OrbitCabLookAndFeel::kAccent     // violet tone knobs (match the curve)
                                             : OrbitCabLookAndFeel::kNeutral);  // neutral grey filter knobs
        k.setColour (juce::Slider::rotarySliderFillColourId, fill);
        k.setColour (juce::Slider::thumbColourId,            fill);
        k.setTooltip (tip);
        addAndMakeVisible (k);
        att = std::make_unique<SAtt> (processorRef.apvts, paramId, k);
        if (tone && eqDiscretePref) k.setRange (k.getMinimum(), k.getMaximum(), 1.0);   // whole-dB steps
        k.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v)); };   // bare number below
        k.valueFromTextFunction = [] (const juce::String& t) { return parseLooseNumber (t); };
        k.updateText();
    };
    setupKnob (eqBassKnob,     "eqBass",     eqBassAtt,     "Bass \xe2\x80\x94 low shelf ~100 Hz",      true,  "dB");
    setupKnob (eqMidKnob,      "eqMid",      eqMidAtt,      "Mid \xe2\x80\x94 bell ~600 Hz",            true,  "dB");
    setupKnob (eqTrebleKnob,   "eqTreble",   eqTrebleAtt,   "Treble \xe2\x80\x94 high shelf ~2.8 kHz",  true,  "dB");
    setupKnob (eqPresenceKnob, "eqPresence", eqPresenceAtt, "Presence \xe2\x80\x94 high shelf ~5 kHz",  true,  "dB");
    // (EQ HPF/LPF freq knobs removed — the response curve's own left/right edges are the draggable corners.)

    // Caption above each tone knob = the model NAME (the unit lives inside the knob now).
    styleLabel (eqBassLabel,     "BASS");
    styleLabel (eqMidLabel,      "MID");
    styleLabel (eqTrebleLabel,   "TREBLE");
    styleLabel (eqPresenceLabel, "PRESENCE");

    // HPF/LPF: the toggle is the caption + enable (click to engage the cut); the unit sits in the knob.
    for (auto* b : { &eqHpfBtn, &eqLpfBtn })
        b->setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kNeutral));
    addAndMakeVisible (eqHpfBtn);
    eqHpfOnAtt = std::make_unique<BAtt> (processorRef.apvts, "eqHpfOn", eqHpfBtn);
    addAndMakeVisible (eqLpfBtn);
    eqLpfOnAtt = std::make_unique<BAtt> (processorRef.apvts, "eqLpfOn", eqLpfBtn);

    // Live response curve — reads the EQ params and reuses cab::AmpEq::describe so the drawn shape
    // is exactly what the audio path applies. Repainted from the 30 Hz timer while the row is open.
    eqCurve.getBands = [this] (teq::BandParams* out) -> int
    {
        auto& a = processorRef.apvts;
        cab::EqParams e;
        e.bassDb     = a.getRawParameterValue ("eqBass")->load();
        e.midDb      = a.getRawParameterValue ("eqMid")->load();
        e.trebleDb   = a.getRawParameterValue ("eqTreble")->load();
        e.presenceDb = 0.0f;   // Presence removed — keep the curve flat there (no leftover-state shelf)
        e.hpfOn      = a.getRawParameterValue ("eqHpfOn")->load() > 0.5f;
        e.hpfHz      = a.getRawParameterValue ("eqHpfFreq")->load();
        e.lpfOn      = a.getRawParameterValue ("eqLpfOn")->load() > 0.5f;
        e.lpfHz      = a.getRawParameterValue ("eqLpfFreq")->load();
        cab::AmpEq::describe (e, out);
        return cab::AmpEq::kNumBands;
    };
    addAndMakeVisible (eqCurve);
    // Drag the curve's left/right edges → the HPF/LPF freq params (replacing the deleted knobs).
    eqCurve.onHpfDragged = [this] (float hz) {
        auto& ap = processorRef.apvts;
        if (auto* q = ap.getParameter ("eqHpfFreq")) q->setValueNotifyingHost (ap.getParameterRange ("eqHpfFreq").convertTo0to1 (hz));
    };
    eqCurve.onLpfDragged = [this] (float hz) {
        auto& ap = processorRef.apvts;
        if (auto* q = ap.getParameter ("eqLpfFreq")) q->setValueNotifyingHost (ap.getParameterRange ("eqLpfFreq").convertTo0to1 (hz));
    };
    eqHpfBtn.toFront (false);   // HPF/LPF enable checkboxes sit on the curve's top corners → keep them clickable
    eqLpfBtn.toFront (false);

    // ---- REVERB: in-amp Tube spring (after EQ, before poweramp). ONE spring now — no selector knob.
    // The reverb knob (captioned REV) is the amount + on/off (0 = off). SCALE/TRIM are TEMP calibration;
    // VOL is the preamp volume (orange). ----
    styleLabel (reverbMixLabel, "REV");   // caption above the reverb amount knob
    setupKnob (reverbMixKnob, "reverbMix", reverbMixAtt,
               juce::String::fromUTF8 ("Reverb \xe2\x80\x94 Tube spring return amount (0 = off)"), false, "MIX");
    reverbMixKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colours::white);   // white (per Oleh)
    reverbMixKnob.setColour (juce::Slider::thumbColourId,            juce::Colours::white);
    // (Reverb Scale / Trim calibration knobs removed — Tube is calibrated in code now.)
    // Preamp VOLUME — the rightmost knob (a preamp param): post-preamp output level. Orange (like GAIN), ±12 dB.
    styleLabel (preampVolLabel, "VOL");
    setupKnob (preampVolKnob, "preampVol", preampVolAtt,
               juce::String::fromUTF8 ("Preamp Volume \xe2\x80\x94 post-preamp output level, drives the EQ / poweramp / cab harder"), false, "dB");
    preampVolKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (OrbitCabLookAndFeel::kAccentB));   // orange (like GAIN)
    preampVolKnob.setColour (juce::Slider::thumbColourId,            juce::Colour (OrbitCabLookAndFeel::kAccentB));
    preampOutMeter.setRange (-24.0f, 6.0f);   // hot internal tap — zoom in so it reads as movement, not a pinned bar
    addAndMakeVisible (preampOutMeter);   // preamp-OUT meter (right of VOL) — shown/hidden with the front strip

    // ---- NOISE GATE: in-amp gate — DETECTOR on the clean input, VCA after the EQ (the reverb tail rings out).
    // A GATE caption + a compact HORIZONTAL threshold slider sit BOTTOM-LEFT under the device glyphs (no toggle —
    // OFF = drag it left); a GateLed shows the state (grey off → green open → yellow → red closed). ----
    styleLabel (gateLabel, "GATE");
    gateLabel.setJustificationType (juce::Justification::centredLeft);
    gateThreshSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gateThreshSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 14);   // value readout: "OFF" / "−48 dB" (param getText)
    gateThreshSlider.setColour (juce::Slider::backgroundColourId,     juce::Colour (0x33ffffff));
    gateThreshSlider.setColour (juce::Slider::trackColourId,          juce::Colour (OrbitCabLookAndFeel::kAccent));
    gateThreshSlider.setColour (juce::Slider::thumbColourId,          juce::Colour (OrbitCabLookAndFeel::kAccent));
    gateThreshSlider.setColour (juce::Slider::textBoxTextColourId,    juce::Colour (0xffb8b8c0));
    gateThreshSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0x00000000));
    gateThreshSlider.setTooltip (juce::String::fromUTF8 ("Noise Gate \xe2\x80\x94 drag LEFT to OFF; otherwise the OPEN threshold (dBFS) vs the input. Attenuates after the EQ so the reverb tail rings out."));
    addAndMakeVisible (gateThreshSlider);
    gateThreshAtt = std::make_unique<SAtt> (processorRef.apvts, "gateThreshold", gateThreshSlider);
    addAndMakeVisible (gateLed);
    addAndMakeVisible (gateLabel);

    // ---- POWERAMP SIMULATOR (white-box tube, blocks 2+3): the third stage tab (radio with CAPTURES) ----
    tubeSimBtn.setTooltip (juce::String::fromUTF8 ("White-box tube poweramp simulator \xe2\x80\x94 no capture needed. Shares the poweramp slot with POWER AMP CAPTURES (only one runs)."));
    tubeSimBtn.setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
    addAndMakeVisible (tubeSimBtn);   // ALWAYS available — the sim needs no .nam library
    tubeSimBtn.onClick = [this]
    {
        auto set = [this] (const char* id, float v) { if (auto* p = processorRef.apvts.getParameter (id)) p->setValueNotifyingHost (v); };
        if (tubeSimBtn.getToggleState()) { set ("ampMode", 1.0f); set ("ampOn", 1.0f); }   // → TUBE, poweramp on
        else                               set ("ampOn", 0.0f);                             // → poweramp off
        syncPowerAmpTabs();
    };

    addChildComponent (tubeSimDisplay);   // the selected tube(s), warm heater glow (count = PP 2 / SE 1)

    // Tube TYPE — 6L6 / EL34 / EL84 / KT88 radio → the tubeType choice param.
    static const char* const kTubeNames[4] = { "6L6", "EL34", "EL84", "KT88" };
    for (int t = 0; t < 4; ++t)
    {
        tubeTypeBtn[t].setButtonText (kTubeNames[t]);
        tubeTypeBtn[t].setClickingTogglesState (false);
        tubeTypeBtn[t].setColour (juce::TextButton::buttonOnColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
        tubeTypeBtn[t].setTooltip (juce::String ("Tube voicing: ") + kTubeNames[t]);
        tubeTypeBtn[t].onClick = [this, t] { selectTubeType (t); };
        addChildComponent (tubeTypeBtn[t]);
    }
    // Tube TOPOLOGY — PP = push-pull (class AB, 2 tubes), SE = single-ended (class A, 1 tube). Labelled
    // PP/SE (not x2/x1) so it doesn't read as an oversampling "×N" series next to the Quality combo.
    static const char* const kTopoNames[2] = { "PP", "SE" };   // [0] = PP push-pull, [1] = SE single-ended
    for (int m = 0; m < 2; ++m)
    {
        tubeTopoBtn[m].setButtonText (kTopoNames[m]);
        tubeTopoBtn[m].setClickingTogglesState (false);
        tubeTopoBtn[m].setColour (juce::TextButton::buttonOnColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
        tubeTopoBtn[m].setTooltip (m == 0 ? juce::String::fromUTF8 ("PP \xe2\x80\x94 push-pull (class AB): two tubes, tighter, even harmonics cancel")
                                          : juce::String::fromUTF8 ("SE \xe2\x80\x94 single-ended (class A): one tube, sweeter, even-harmonic rich"));
        tubeTopoBtn[m].onClick = [this, m] { selectTubeTopo (m == 1); };   // [1] = SE = single-ended
        addChildComponent (tubeTopoBtn[m]);
    }
    // Feel knobs — Drive / Sag / Presence / Depth / Load / Output, warm amber to match the tube glow.
    setupKnob (tubeDriveKnob, "tubeDrive",    tubeDriveAtt, juce::String::fromUTF8 ("Drive into the tube \xe2\x80\x94 how hard it's pushed"),             false, "dB");
    setupKnob (tubeSagKnob,   "tubeSag",      tubeSagAtt,   juce::String::fromUTF8 ("Sag \xe2\x80\x94 power-supply bloom / touch / compression under load"), false, "%");
    setupKnob (tubePresKnob,  "tubePresence", tubePresAtt,  juce::String::fromUTF8 ("Presence \xe2\x80\x94 NFB-style HF that opens up when pushed"),         false, "%");
    setupKnob (tubeDepthKnob, "tubeDepth",    tubeDepthAtt, juce::String::fromUTF8 ("Depth \xe2\x80\x94 NFB-style LF that loosens when pushed"),            false, "%");
    setupKnob (tubeLoadKnob,  "tubeLoad",     tubeLoadAtt,  juce::String::fromUTF8 ("Load \xe2\x80\x94 reactive-speaker impedance: bass punch, pick attack, 'amp in a room'"), false, "%");
    setupKnob (tubeIronKnob,  "tubeIron",     tubeIronAtt,  juce::String::fromUTF8 ("Iron \xe2\x80\x94 output transformer: low-note grind/compression + softer top"),        false, "%");
    setupKnob (tubeBloomKnob, "tubeBias",     tubeBloomAtt, juce::String::fromUTF8 ("Bloom \xe2\x80\x94 dynamic bias-shift under sag: crossover 'chew' as you dig in (needs Sag)"), false, "%");
    setupKnob (tubeOutKnob,   "tubeOutput",   tubeOutAtt,   juce::String::fromUTF8 ("Output trim after the tube stage"),                                    false, "dB");
    for (auto* k : { &tubeDriveKnob, &tubeSagKnob, &tubePresKnob, &tubeDepthKnob, &tubeLoadKnob, &tubeIronKnob, &tubeBloomKnob, &tubeOutKnob })
    {
        k->setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (OrbitCabLookAndFeel::kAccentB));   // warm amber
        k->setColour (juce::Slider::thumbColourId,            juce::Colour (OrbitCabLookAndFeel::kAccentB));
        k->setVisible (false);   // shown only when SIMULATOR is the active poweramp tab
    }
    styleLabel (tubeDriveLbl, "DRIVE");  styleLabel (tubeSagLbl, "SAG");  styleLabel (tubePresLbl, "PRESENCE");
    styleLabel (tubeDepthLbl, "DEPTH");  styleLabel (tubeLoadLbl, "LOAD");  styleLabel (tubeIronLbl, "IRON");
    styleLabel (tubeBloomLbl, "BLOOM");  styleLabel (tubeOutLbl, "OUTPUT");
    for (auto* l : { &tubeDriveLbl, &tubeSagLbl, &tubePresLbl, &tubeDepthLbl, &tubeLoadLbl, &tubeIronLbl, &tubeBloomLbl, &tubeOutLbl }) l->setVisible (false);
    // OS-quality picker (live) — items MUST match the "tubeOS" choice-param order (1-based ids).
    tubeOsBox.addItem ("2x",  1);
    tubeOsBox.addItem ("4x",  2);
    tubeOsBox.addItem ("8x",  3);
    tubeOsBox.addItem ("16x", 4);
    tubeOsBox.addItem ("32x", 5);
    tubeOsBox.setTooltip (juce::String::fromUTF8 ("Oversampling \xe2\x80\x94 higher = softer top (less aliasing), more CPU (16x/32x heavy). Switches live."));
    tubeOsBox.setColour (juce::ComboBox::textColourId, juce::Colour (OrbitCabLookAndFeel::kAccentB));
    addChildComponent (tubeOsBox);   // shown only when SIMULATOR is the active poweramp tab
    tubeOsAtt = std::make_unique<CAtt> (processorRef.apvts, "tubeOS", tubeOsBox);

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
    updateEqRow();       // initial reveal state for the amp-EQ row
    updatePreampRow();   // initial reveal state for the preamp row
    syncPowerAmpTabs();  // poweramp tabs (CAPTURES ⊕ SIMULATOR): set toggles from ampOn/ampMode, reveal, size
}

OrbitCabAudioProcessorEditor::~OrbitCabAudioProcessorEditor()
{
    stopTimer();
    processorRef.setSpectrumActive (false);   // editor gone → stop feeding the analyser
    preampBox.setLookAndFeel (nullptr);        // detach the custom popup L&F before it is destroyed
    setLookAndFeel (nullptr);
}

void OrbitCabAudioProcessorEditor::timerCallback()
{
    inMeter.setLevel  (processorRef.getInputLevel());
    outMeter.setLevel (processorRef.getOutputLevel());
    preampOutMeter.setLevel (processorRef.getPreampOutLevel());   // preamp OUT (pre-poweramp)
    gateLed.setState (processorRef.isGateArmed(),                 // gate state LED: grey (off) → green (open) → yellow → red (closed)
                      processorRef.getGateGain());
    {
        PerfBadge::Stats ps;
        const double sr = processorRef.getSampleRate();
        ps.latencySamples = processorRef.getLatencySamples();
        ps.latencyMs = sr > 0.0 ? (float) (ps.latencySamples * 1000.0 / sr) : 0.0f;
        ps.total    = processorRef.getCpuTotal();
        ps.preamp   = processorRef.getCpuPreamp();
        ps.eq       = processorRef.getCpuEq();
        ps.reverb   = processorRef.getCpuReverb();
        ps.poweramp = processorRef.getCpuPoweramp();
        ps.cab      = processorRef.getCpuCab();
        perfBadge.setStats (ps);
    }
    pushFiltersToWave();
    updateEnablement();
    refreshDryWetVisibility();                     // catch a blend (mix ≠ 100%) loaded by the host
    updateSpectrum();

    // Reflect poweramp params onto the controls (host automation / state restore don't fire the
    // button onClicks). A change in ampOn re-runs the reveal + resize; otherwise just keep the
    // tube radio buttons in sync with the choice param.
    // Poweramp tabs (CAPTURES ⊕ SIMULATOR): reflect ampOn/ampMode from host automation / state restore
    // (the button onClicks don't fire for those). The sim needs no library, so this runs unconditionally.
    {
        const bool ampOn = processorRef.apvts.getRawParameterValue ("ampOn")->load()   > 0.5f;
        const int  mode  = processorRef.apvts.getRawParameterValue ("ampMode")->load() > 0.5f ? 1 : 0;
        if (ampOn != ampOnCache || mode != ampModeCache)
            syncPowerAmpTabs();                    // ampOn/ampMode changed → re-set toggles + reveal + resize
        else if (hasPoweramps && ampPowerBtn.getToggleState() && processorRef.selectedPowerampId() != ampSyncedId)
            syncAmpSelector();                     // capture selection changed externally (automation / restore)
        if (ampPowerBtn.getToggleState()) tubeDisplay.tick();       // captures warm-heater flicker (~30 Hz)
        if (tubeSimBtn.getToggleState())  tubeSimDisplay.tick();    // simulator warm-heater flicker
    }
    if (hasPreamps)                                // same, for the PREAMP stage
    {
        if ((processorRef.apvts.getRawParameterValue ("preampOn")->load() > 0.5f) != preampOnCache)
            updatePreampRow();                     // preampOn flipped → reveal/hide + resize
        else if (processorRef.selectedPreampId() != preampSyncedId)
            syncPreampSelector();                  // selection changed externally (automation / restore) → re-sync
        if (preampPowerBtn.getToggleState())
        {
            preampTubeDisplay.tick();              // advance the warm heater flicker (~30 Hz)
            preampDeviceStrip.tick();              // device glyph glow flicker (tube / BJT / FET)
        }
    }
    // AMP EQ has no library — always available; just mirror eqOn (host automation / state restore,
    // which don't fire the button's onClick) to reveal/hide + resize the row.
    if ((processorRef.apvts.getRawParameterValue ("eqOn")->load() > 0.5f) != eqOnCache)
        updateEqRow();
    if (eqPowerBtn.getToggleState())                       // row open → keep the response curve live
    {
        const double sr = processorRef.getSampleRate();
        eqCurve.sampleRate = sr > 0.0 ? sr : 48000.0;
        // Push the live HPF/LPF on-state + freq + ranges so the curve's draggable corners track the params.
        auto& ap = processorRef.apvts;
        const auto hR = ap.getParameterRange ("eqHpfFreq");
        const auto lR = ap.getParameterRange ("eqLpfFreq");
        eqCurve.setHpf (ap.getRawParameterValue ("eqHpfOn")->load() > 0.5f, ap.getRawParameterValue ("eqHpfFreq")->load(), hR.start, hR.end);
        eqCurve.setLpf (ap.getRawParameterValue ("eqLpfOn")->load() > 0.5f, ap.getRawParameterValue ("eqLpfFreq")->load(), lR.start, lR.end);
        eqCurve.repaint();
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
        updatePreampRow();   // a restored / snapshot / undo selection may flip hasPreamps/hasPoweramps —
        updateAmpRow();      // re-evaluate visibility so an audible NAM stage is never left hidden
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
    {
        eqCurve.setSpectrum ({});
        return;
    }

    spectrum.update (processorRef);
    slots[0].setSpectrum (spectrum.pre(), spectrum.post());
    slots[1].setSpectrum (spectrum.pre(), spectrum.post());
    eqCurve.setSpectrum (spectrum.post());           // faint output spectrum behind the EQ curve
}

void OrbitCabAudioProcessorEditor::applyWaveformScale()
{
    slots[0].setWaveformScale (waveLogPref, (float) waveFloorPref);
    slots[1].setWaveformScale (waveLogPref, (float) waveFloorPref);
}

void OrbitCabAudioProcessorEditor::openSettings()
{
    auto panel = std::make_unique<SettingsPanel> (
        processorRef.getHeadTrim(), processorRef.getInputSource(), dryWetPref, spectrumEnabled, waveLogPref, waveFloorPref,
        showTubesPref,
        [this] (bool on)                                   // HEAD: persisted session setting
        {
            processorRef.setHeadTrim (on);
            pushFiltersToWave();                           // refresh the waveform head overlay
        },
        [this] (int mode)                                  // INPUT source: persisted session setting (Stereo/Left/Right)
        {
            processorRef.setInputSource (mode);
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
            updatePreampRow();                             // re-flow + resize the NAM rows (tall ↔ slim strip)
            updateAmpRow();                                // — each sets its own tube display's show-tubes flag
            updateTubeSimRow();                            // + the tube simulator row (its display + height)
        },
        [this]                                             // Poweramp library…: open the poweramp manager
        {
            if (settingsCallout != nullptr)
                settingsCallout->dismiss();                // close the settings pop-over first (no nested call-outs)
            openPowerampManager();
        },
        [this]                                             // Preamp library…: open the preamp manager
        {
            if (settingsCallout != nullptr)
                settingsCallout->dismiss();
            openPreampManager();
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
    hasPoweramps = ! ampLib.empty() || processorRef.selectedPowerampId().isNotEmpty();   // restored/pooled model shows even with an empty local library

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

// Point the hours slider at the current name+mode's positions (its stops = ampHourVals indices).
// Leaves ampHourVals empty when there are <2 positions (caller hides the slider then).
void OrbitCabAudioProcessorEditor::configureHourSlider()
{
    ampHourVals.clear();
    auto* cur = ampEntryById (processorRef.selectedPowerampId());
    if (cur == nullptr) return;
    const auto hrs = hoursForNameCat (cur->name, cur->cat);
    if (hrs.size() < 2) return;                       // 0/1 position → no slider
    ampHourVals = hrs;                                // sorted; slider index i → ampHourVals[i] o'clock

    ampHourSlider.setRange (0.0, (double) (ampHourVals.size() - 1), 1.0);   // discrete: snaps to each stop
    const int idx = (int) (std::find (ampHourVals.begin(), ampHourVals.end(), cur->hours) - ampHourVals.begin());
    ampHourSlider.setValue (juce::isPositiveAndBelow (idx, (int) ampHourVals.size()) ? idx : 0,
                            juce::dontSendNotification);
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
    const bool hasPP = std::find (cats.begin(), cats.end(), orbitcab::PowerampCat::pushPull)    != cats.end();
    const bool hasSE = std::find (cats.begin(), cats.end(), orbitcab::PowerampCat::singleEnded) != cats.end();
    const bool showMode = on && group && hasPP && hasSE;   // a real PP↔SE choice, not e.g. PP + Other
    for (int m = 0; m < 2; ++m)
    {
        const auto mc = (m == 0 ? orbitcab::PowerampCat::pushPull : orbitcab::PowerampCat::singleEnded);
        ampModeBtn[m].setVisible (showMode);
        ampModeBtn[m].setToggleState (cur != nullptr && cur->cat == mc, juce::dontSendNotification);
    }

    // contextual hours slider — pointed at this name+mode's positions (hidden when <2 exist)
    configureHourSlider();
    ampHourSlider.setVisible (on && group && ampHourVals.size() >= 2);

    // tubes: silhouette from the name, count from the mode (PP 2 / SE 1 / Other 0)
    tubeDisplay.setSelection (cur != nullptr ? tubeTypeFromName (cur->name) : 0,
                              cur != nullptr ? orbitcab::tubeCountForCat (cur->cat) : 0, on);

    resized();   // contextual controls appeared/disappeared → re-flow the row
}

void OrbitCabAudioProcessorEditor::updateAmpRow()
{
    // A restored/embedded model (from <PowerampPool>) can be audible even when this build's local
    // library is empty (a moved project on a public build). Show the UI when EITHER the library has
    // models OR a selection is restored, so an audible stage is never invisible/untoggleable. Recompute
    // here (not just on rebuild) so a host state-restore / snapshot / undo reveals it.
    hasPoweramps = ! ampLib.empty() || processorRef.selectedPowerampId().isNotEmpty();
    ampPowerBtn.setVisible (hasPoweramps);

    const bool on = hasPoweramps && ampPowerBtn.getToggleState();
    // (ampOnCache / ampModeCache are owned by syncPowerAmpTabs — the CAPTURES⊕SIMULATOR radio.)

    // Powering on with no resolvable selection → default to the first library entry (so the stage
    // is never "on but silent"). A restored session that already carries a valid "ampSel" keeps it.
    // Default a selection only when there is NONE — don't overwrite a restored ampSel that resolves
    // from the embedded pool but isn't in this machine's local library (else a moved session resets).
    if (on && ! ampLib.empty() && processorRef.selectedPowerampId().isEmpty())
        processorRef.selectPoweramp (ampLib.front().id);

    tubeDisplay.setShowTubes (showTubesPref);         // "Show tubes" hides the tubes but keeps the amp icon
    tubeDisplay.setVisible (on);                      // amp icon stays whenever the poweramp is on

    for (auto& b : ampNameBtns) b->setVisible (on);                       // name buttons always visible when on
    ampSingleBox.setVisible (on && ampSingleBox.getNumItems() > 0);       // combo only if there are singletons

    resizeForAmpRows();                                  // grow/shrink for both NAM rows (triggers resized on change)
    syncAmpSelector();                                   // contextual controls + tubes + final re-flow
}

//==============================================================================
// POWERAMP TABS — CAPTURES (NAM) ⊕ SIMULATOR (white-box tube) share the ONE poweramp slot (ampOn +
// ampMode) as a radio. syncPowerAmpTabs is the single point that reflects the two params onto the two
// strip toggles + the tube type/topology radios + the glow display, then reveals the active row.
//==============================================================================
void OrbitCabAudioProcessorEditor::syncPowerAmpTabs()
{
    auto& a = processorRef.apvts;
    const bool ampOn = a.getRawParameterValue ("ampOn")->load()   > 0.5f;
    const bool tube  = a.getRawParameterValue ("ampMode")->load() > 0.5f;
    ampOnCache   = ampOn;
    ampModeCache = tube ? 1 : 0;

    // Radio: at most one poweramp tab is active (CAPTURES needs a library; SIMULATOR is always available).
    ampPowerBtn.setToggleState (ampOn && ! tube, juce::dontSendNotification);
    tubeSimBtn .setToggleState (ampOn &&   tube, juce::dontSendNotification);

    // reflect the tube TYPE + TOPOLOGY radios + the glow display
    const int  type = juce::jlimit (0, 3, (int) std::lround (a.getRawParameterValue ("tubeType")->load()));
    const bool se   = a.getRawParameterValue ("tubeTopo")->load() > 0.5f;
    for (int t = 0; t < 4; ++t) tubeTypeBtn[t].setToggleState (t == type, juce::dontSendNotification);
    tubeTopoBtn[0].setToggleState (! se, juce::dontSendNotification);   // PP push-pull
    tubeTopoBtn[1].setToggleState (  se, juce::dontSendNotification);   // SE single-ended
    tubeSimDisplay.setShowTubes (showTubesPref);
    tubeSimDisplay.setSelection (type, se ? 1 : 2, tubeSimBtn.getToggleState());   // silhouette + count (PP 2 / SE 1) + glow

    updateAmpRow();       // captures row visibility (keys off ampPowerBtn) + resize + syncAmpSelector
    updateTubeSimRow();   // sim row visibility (keys off tubeSimBtn) + resize
}

void OrbitCabAudioProcessorEditor::updateTubeSimRow()
{
    const bool on = tubeSimBtn.getToggleState();
    tubeSimDisplay.setShowTubes (showTubesPref);
    tubeSimDisplay.setVisible (on);
    for (auto& b : tubeTypeBtn) b.setVisible (on);
    for (auto& b : tubeTopoBtn) b.setVisible (on);
    for (auto* k : { &tubeDriveKnob, &tubeSagKnob, &tubePresKnob, &tubeDepthKnob, &tubeLoadKnob, &tubeIronKnob, &tubeBloomKnob, &tubeOutKnob }) k->setVisible (on);
    for (auto* l : { &tubeDriveLbl, &tubeSagLbl, &tubePresLbl, &tubeDepthLbl, &tubeLoadLbl, &tubeIronLbl, &tubeBloomLbl, &tubeOutLbl })         l->setVisible (on);
    tubeOsBox.setVisible (on);
    resizeForAmpRows();   // size the poweramp band (sim row height when on; triggers resized on change)
    resized();            // re-flow whichever poweramp tab is active into the shared band
}

void OrbitCabAudioProcessorEditor::selectTubeType (int t)
{
    if (auto* p = processorRef.apvts.getParameter ("tubeType"))
        p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, (float) juce::jlimit (0, 3, t) / 3.0f));   // 4 choices → 0, 1/3, 2/3, 1
    syncPowerAmpTabs();
}

void OrbitCabAudioProcessorEditor::selectTubeTopo (bool singleEnded)
{
    if (auto* p = processorRef.apvts.getParameter ("tubeTopo"))
        p->setValueNotifyingHost (singleEnded ? 1.0f : 0.0f);   // 0 = push-pull, 1 = single-ended
    syncPowerAmpTabs();
}

//==============================================================================
// PREAMP selector — thin GUI bindings over orbitcab::PreampSelector (the pure resolve/view-model in
// PreampSelector.h, unit-tested headless). select* asks the model to resolve a dimension change into
// a new id; syncPreampSelector asks it which contextual controls to show + their values and pushes
// that onto the widgets. The "≥2 values → show the control" rule + the keep-other-dimensions-else-
// default policy live in the model, not here.
//==============================================================================

void OrbitCabAudioProcessorEditor::rebuildPreampSelector()
{
    preampSel.lib = processorRef.preampLibrary();   // factory (PreampBinaryData) + user (preampDir), merged
    hasPreamps    = ! preampSel.lib.empty() || processorRef.selectedPreampId().isNotEmpty();   // restored/pooled model shows even with an empty local library

    // Unified NAME combo: model families (a name shared by ≥2 captures) + one-off singletons, split into
    // Factory and User sections. preampBoxTargets[itemId-1] records what each row selects:
    //   {true,  name} → a family  (selecting it resolves to its current channel/gain/boost variant)
    //   {false, id}   → a singleton entry (selected directly)
    preampBox.clear (juce::dontSendNotification);
    preampBoxTargets.clear();
    preampItemDevice.clear();
    // Per-item device glyphs for the popup: read a representative capture's metadata (device + stages).
    auto storeDevice = [this] (const juce::String& itemText, const juce::String& id)
    {
        auto spec = orbitcab::ui::parseDeviceSpec (processorRef.preampMetaFor (id).getValue ("device", {}));
        if (! spec.empty())
            preampItemDevice[itemText] = std::move (spec);
    };
    auto repIdForName = [this] (const juce::String& nm) -> juce::String
    {
        for (const auto& e : preampSel.lib) if (e.name == nm) return e.id;
        return {};
    };
    const auto groups = preampSel.groupNames();   // family names, first-seen order

    auto familyIsFactory = [this] (const juce::String& nm)
    {
        for (const auto& e : preampSel.lib) if (e.name == nm) return e.factory;   // a family's source = its first variant's
        return true;
    };
    // Names treated as "clean"/near-transparent front-ends: curated to the BOTTOM next to BYPASS rather
    // than listed among the voiced amp models (they colour almost nothing). "ISA Studio Pre" = Focusrite
    // ISA Two studio pre — essentially flat. Prefix-matched so any ISA* label lands here. Singletons.
    auto isCleanName = [] (const juce::String& nm) { return preampNameIsClean (nm); };

    auto addSection = [this, &groups, &familyIsFactory, &isCleanName, &storeDevice, &repIdForName] (bool factory, const char* header)
    {
        bool wroteHeader = false;
        auto ensureHeader = [&] { if (! wroteHeader) { preampBox.addSectionHeading (header); wroteHeader = true; } };
        for (const auto& nm : groups)
            if (familyIsFactory (nm) == factory)
            {
                ensureHeader();
                preampBoxTargets.push_back ({ true, nm });
                preampBox.addItem (nm, (int) preampBoxTargets.size());
                storeDevice (nm, repIdForName (nm));
            }
        for (const auto& e : preampSel.lib)
            if (e.factory == factory && ! preampSel.isGroupName (e.name) && ! isCleanName (e.name))
            {
                ensureHeader();
                preampBoxTargets.push_back ({ false, e.id });
                preampBox.addItem (e.name, (int) preampBoxTargets.size());
                storeDevice (e.name, e.id);
            }
    };
    addSection (true,  "Factory");
    addSection (false, "User");

    // Bottom cluster — the "no voicing" end of the list: clean/near-transparent front-ends (ISA) then
    // BYPASS. A clean pre colours almost nothing, so it sits with "no amp" rather than among the models.
    // BYPASS itself: no neural preamp (the stage passes through), but the tone EQ still runs, so this is
    // a standalone EQ (IR cab + tone). Sentinel id "bypass" (handled in applyPreamp).
    if (preampBox.getNumItems() > 0)
        preampBox.addSeparator();
    for (const auto& e : preampSel.lib)
        if (isCleanName (e.name))
        {
            preampBoxTargets.push_back ({ false, e.id });
            preampBox.addItem (e.name, (int) preampBoxTargets.size());
            storeDevice (e.name, e.id);
        }
    preampBoxTargets.push_back ({ false, "bypass" });
    preampBox.addItem (juce::String::fromUTF8 ("BYPASS \xe2\x80\x94 EQ only"), (int) preampBoxTargets.size());

    preampPowerBtn.setVisible (hasPreamps);
}

// Each select* resolves a single-dimension change to a new id via the model, then commits + re-syncs.
void OrbitCabAudioProcessorEditor::selectPreampName (const juce::String& name)
{
    const auto id = preampSel.resolveName (processorRef.selectedPreampId(), name);
    if (id.isNotEmpty()) { processorRef.selectPreamp (id); syncPreampSelector(); }
}

void OrbitCabAudioProcessorEditor::selectPreampChannel (int channel)
{
    const auto id = preampSel.resolveChannel (processorRef.selectedPreampId(), channel);
    if (id.isNotEmpty()) { processorRef.selectPreamp (id); syncPreampSelector(); }
}

void OrbitCabAudioProcessorEditor::selectPreampGain (int hours)
{
    const auto id = preampSel.resolveGain (processorRef.selectedPreampId(), hours);
    if (id.isNotEmpty()) { processorRef.selectPreamp (id); syncPreampSelector(); }
}

void OrbitCabAudioProcessorEditor::selectPreampBoost (bool boost)
{
    const auto id = preampSel.resolveBoost (processorRef.selectedPreampId(), boost);
    if (id.isNotEmpty()) { processorRef.selectPreamp (id); syncPreampSelector(); }
}

// Reflect the "preampSel" selection onto the whole selector: highlight the name button (or the combo),
// show the contextual channel switch / gain slider / boost toggle (each only when ≥2 values exist for
// that dimension), plus the tube. The model decides WHAT to show; this binds it to the widgets.
void OrbitCabAudioProcessorEditor::syncPreampSelector()
{
    preampSyncedId = processorRef.selectedPreampId();
    const bool on  = preampPowerBtn.getToggleState();
    auto* cur = preampSel.entryById (preampSyncedId);
    const auto v = preampSel.viewFor (preampSyncedId);

    // unified NAME combo — reflect (or clear) the selection: a family selection matches its {true,name}
    // target row, a singleton its {false,id} row, BYPASS its {false,"bypass"} sentinel row.
    int selItem = 0;
    for (int i = 0; i < (int) preampBoxTargets.size(); ++i)
    {
        const auto& t = preampBoxTargets[(size_t) i];
        const bool hit = preampSyncedId == "bypass" ? (! t.first && t.second == "bypass")
                       : cur != nullptr ? (v.group ? (t.first && t.second == cur->name)
                                                    : (! t.first && t.second == cur->id))
                       : false;
        if (hit) { selItem = i + 1; break; }
    }
    preampBox.setSelectedId (selItem, juce::dontSendNotification);

    // Rich tooltip from the model's metadata (gear · tone · boost · author), read cheaply from the
    // .namz header — no weight inflate. Falls back to the plain hint when a model carries no metadata.
    {
        juce::String tip = "Preamp — pick a neural capture (or Bypass).";
        juce::String gear;
        orbitcab::ui::DeviceSpec spec;
        if (cur != nullptr)
        {
            const auto meta = processorRef.preampMetaFor (preampSyncedId);
            gear = (meta.getValue ("gear_make", {}) + " " + meta.getValue ("gear_model", {})).trim();
            spec = orbitcab::ui::parseDeviceSpec (meta.getValue ("device", {}));
            juce::StringArray bits;
            if (gear.isNotEmpty())                              bits.add (gear);
            if (meta.getValue ("tone_type", {}).isNotEmpty())   bits.add (meta["tone_type"]);
            if (meta.getValue ("boost", "false") == "true")     bits.add ("+boost");
            if (meta.getValue ("modeled_by", {}).isNotEmpty())  bits.add ("modeled by " + meta["modeled_by"]);
            if (! bits.isEmpty()) tip = cur->name + "  —  " + bits.joinIntoString ("  ·  ");
        }
        preampBox.setTooltip (tip);

        // Gear caption above + device glyphs below the combo (amber tube / steel transistor·FET).
        preampGearLabel.setText (gear, juce::dontSendNotification);
        preampGearLabel.setVisible (on && gear.isNotEmpty());
        preampDeviceStrip.set (spec);            // colours + flicker come from the device family (per glyph)
        preampDeviceStrip.setVisible (on && ! spec.empty());
    }

    // contextual channel switch — one button per REAL channel the name has (channel 0 = "none" gets no
    // button), shown only when ≥2 exist. Each slot is captioned + tinted from that channel's entry, so a
    // colour channel (e.g. a red/green amp) reads and glows in its own colour; chN stays accent.
    std::vector<int> chans;
    for (int c : v.channels) if (c > 0) chans.push_back (c);
    const bool showChannels = on && v.showChannels;
    for (int m = 0; m < 4; ++m)
    {
        auto& btn = preampChannelBtn[m];
        const bool used = m < (int) chans.size();
        if (used)
        {
            const int channel = chans[(size_t) m];
            preampChannelBtnCh[m] = channel;
            const auto* e = cur != nullptr ? preampSel.anyEntryFor (cur->name, channel) : nullptr;

            // Caption = the channel's TONE from the model metadata (CLEAN / CRUNCH / HI-GAIN), read
            // cheaply from the .namz header; fall back to the colour word, then "Ch N".
            const juce::String tone = (e != nullptr) ? processorRef.preampMetaFor (e->id).getValue ("tone_type", {})
                                                     : juce::String();
            const juce::String label = tone.isNotEmpty()                             ? tone
                                     : (e != nullptr && e->channelLabel.isNotEmpty()) ? e->channelLabel
                                     :                                                  ("Ch " + juce::String (channel));
            btn.setButtonText (label.toUpperCase());

            // Tint by the channel colour: faint when idle, vivid when active (green / orange / blue / red).
            const juce::Colour chCol = (e != nullptr && e->channelColour != 0) ? juce::Colour (e->channelColour)
                                                                               : juce::Colour (OrbitCabLookAndFeel::kAccent);
            btn.setColour (juce::TextButton::buttonColourId,   chCol.withMultipliedSaturation (0.78f).withMultipliedBrightness (0.62f));
            btn.setColour (juce::TextButton::buttonOnColourId, chCol);
            btn.setTooltip ((e != nullptr && e->channelLabel.isNotEmpty() ? e->channelLabel + " channel" : juce::String ("Channel"))
                            + (tone.isNotEmpty() ? juce::String (" — ") + tone : juce::String()));
            btn.setToggleState (v.currentChannel == channel, juce::dontSendNotification);
        }
        btn.setVisible (showChannels && used);
    }

    // contextual gain slider — its discrete stops are this name+channel's positions (hidden when <2)
    preampGainVals = v.showGain ? v.gains : std::vector<int>{};
    if (preampGainVals.size() >= 2)
    {
        preampGainSlider.setRange (0.0, (double) (preampGainVals.size() - 1), 1.0);   // snaps to each stop
        const int idx = (int) (std::find (preampGainVals.begin(), preampGainVals.end(), v.currentGain) - preampGainVals.begin());
        preampGainSlider.setValue (juce::isPositiveAndBelow (idx, (int) preampGainVals.size()) ? idx : 0,
                                   juce::dontSendNotification);
    }
    preampGainSlider.setVisible (on && v.showGain);

    // contextual boost toggle — only when both an on- and an off-capture exist for name+channel+gain
    preampBoostBtn.setVisible (on && v.showBoost);
    preampBoostBtn.setToggleState (v.currentBoost, juce::dontSendNotification);

    // tube: a single slim preamp tube (12AX7-ish), glowing when on
    preampTubeDisplay.setSelection (2 /*slim silhouette*/, cur != nullptr ? 1 : 0, on);

    resized();   // contextual controls appeared/disappeared → re-flow the row
}

void OrbitCabAudioProcessorEditor::updatePreampRow()
{
    // Same as updateAmpRow: a restored/embedded preamp can be audible with an empty local library, so
    // reveal the UI when EITHER the library has models OR a selection is restored (re-evaluated here so
    // a host state-restore / snapshot / undo reveals it, not only on a library rebuild).
    hasPreamps = ! preampSel.lib.empty() || processorRef.selectedPreampId().isNotEmpty();
    preampPowerBtn.setVisible (hasPreamps);

    const bool on = hasPreamps && preampPowerBtn.getToggleState();
    preampOnCache = preampPowerBtn.getToggleState();

    // The tone EQ is part of the preamp now: its DSP + controls follow the preamp's power. Driving the
    // (hidden) eqPowerBtn keeps the eqOn parameter and the tone-control visibility in lock-step.
    eqPowerBtn.setToggleState (on, juce::sendNotificationSync);

    // Powering on with no resolvable selection → default to the clean front-end (ISA) if present, else
    // the first library entry (never "on but silent"). Explicit ISA preference so the default doesn't
    // ride on library sort order. A restored session that already carries a valid "preampSel" keeps it;
    // "bypass" counts as a real (non-empty) selection, so it is preserved (standalone EQ).
    if (on && ! preampSel.lib.empty() && processorRef.selectedPreampId().isEmpty())
    {
        const auto clean = std::find_if (preampSel.lib.begin(), preampSel.lib.end(),
                                         [] (const auto& e) { return preampNameIsClean (e.name); });
        processorRef.selectPreamp (clean != preampSel.lib.end() ? clean->id : preampSel.lib.front().id);
    }

    preampTubeDisplay.setShowTubes (showTubesPref);   // "Show tubes" hides the tube but keeps the amp icon
    preampTubeDisplay.setVisible (false);             // device glyph removed from the preamp strip (Oleh) — freed for the reverb

    preampBox.setVisible (on && preampBox.getNumItems() > 0);                // the unified NAME combo

    resizeForAmpRows();                               // grow/shrink for both NAM rows (triggers resized on change)
    syncPreampSelector();                             // contextual controls + tube + final re-flow
}

void OrbitCabAudioProcessorEditor::updateEqRow()
{
    const bool on = eqPowerBtn.getToggleState();
    eqOnCache = on;

    eqCurve.setVisible (on);
    for (auto* k : { &eqBassKnob, &eqMidKnob, &eqTrebleKnob })   // HPF/LPF freq knobs removed (curve edges drag)
        k->setVisible (on);
    for (auto* l : { &eqBassLabel, &eqMidLabel, &eqTrebleLabel })
        l->setVisible (on);
    eqPresenceKnob.setVisible (false);    // Presence removed from the UI — redundant ~5 kHz HF shelf (near Treble),
    eqPresenceLabel.setVisible (false);   // and NOT the real NFB amp presence (that's the Tube poweramp's own Presence)
    eqHpfBtn.setVisible (on);
    eqLpfBtn.setVisible (on);

    resizeForAmpRows();   // grow/shrink for all three front-stage rows (triggers resized on change)
    repaint();            // refresh the revealed row's panel background
}

void OrbitCabAudioProcessorEditor::openPreampManager()
{
    // The preamp library manager: Add / Remove the user .nam captures (preampDir). On any change it
    // rebuilds the bottom-strip selector + re-flows the row.
    auto mgr = std::make_unique<PreampManager> (processorRef, [this]
    {
        rebuildPreampSelector();   // rescan merged library → model buttons + hasPreamps + power-btn visibility
        updatePreampRow();         // reveal/hide + resize to match the new library / selection
    });
    juce::CallOutBox::launchAsynchronously (std::move (mgr), settingsBtn.getBounds(), this);
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

            // Light, factual heads-up — only when the preset ACTUALLY carries embedded audio: an
            // external (user) IR and/or the preamp/poweramp .nam (v5/v6). A preset of only bundled
            // refs with no amp embeds nothing → no popup. Wording reflects what's really in there.
            const bool embIR  = processorRef.exportEmbedsIR();
            const bool embAmp = processorRef.exportEmbedsAmp();
            const bool embPre = processorRef.exportEmbedsPreamp();
            if (embIR || embAmp || embPre)
            {
                juce::StringArray bits;
                if (embIR)  bits.add ("its IR audio");
                if (embPre) bits.add ("the preamp capture");
                if (embAmp) bits.add ("the poweramp capture");
                juce::String what = bits.size() == 1 ? bits[0]
                                  : bits.joinIntoString (", ", 0, bits.size() - 1) + " and " + bits[bits.size() - 1];
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
    if (! tubeSimRowBounds.isEmpty())                              // revealed tube-simulator row (same shade)
        vpanel (tubeSimRowBounds.toFloat(), 0xff1f1f26, 0xff141419, 0.0f);
    if (! preampRowBounds.isEmpty())                               // revealed preamp row (same shade)
        vpanel (preampRowBounds.toFloat(), 0xff1f1f26, 0xff141419, 0.0f);
    if (! eqRowBounds.isEmpty())                                   // revealed amp-EQ row (same shade)
        vpanel (eqRowBounds.toFloat(), 0xff1f1f26, 0xff141419, 0.0f);

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
    // Revealed NAM rows at the very bottom: POWERAMP lowest, PREAMP just above it, so the window
    // reads top→bottom in signal order (… MIX strip, PREAMP, POWERAMP). removeFromBottom peels the
    // lowest band first, so the poweramp row is removed before the preamp row. Each present only
    // when its stage is powered.
    if (hasPoweramps && ampPowerBtn.getToggleState())
    {
        ampRowBounds = r.removeFromBottom (ampRowH());
        tubeSimRowBounds = {};                                       // captures active → sim row not laid out
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
        // hours slider (discrete, horizontal) — track + a "12h" read-out box on its right
        if (ampHourSlider.isVisible())
        {
            ampHourSlider.setBounds (row.removeFromRight (132).withSizeKeepingCentre (132, 26));
            row.removeFromRight (12);
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
    else if (tubeSimBtn.getToggleState())    // SIMULATOR is the active poweramp tab — same bottom slot as captures
    {
        ampRowBounds = {};
        tubeSimRowBounds = r.removeFromBottom (tubeSimRowH());
        auto row = tubeSimRowBounds.reduced (16, 8);
        tubeSimDisplay.setBounds (row.removeFromLeft (showTubesPref ? 150 : 52));
        row.removeFromLeft (14);

        // TYPE (a row of 4) over TOPOLOGY (a row of 2), a two-row block on the left
        {
            auto blk   = row.removeFromLeft (206);
            auto typeR = blk.removeFromTop (blk.getHeight() / 2).withSizeKeepingCentre (206, 26);
            constexpr int tw = 48, tg = 3;
            for (auto& b : tubeTypeBtn) { b.setBounds (typeR.removeFromLeft (tw)); typeR.removeFromLeft (tg); }
            auto topoR = blk.withSizeKeepingCentre (206, 24);   // bottom row: TOPOLOGY (2) on the left, OS-quality combo on the right
            constexpr int pw = 46, pg = 3;
            for (auto& b : tubeTopoBtn) { b.setBounds (topoR.removeFromLeft (pw)); topoR.removeFromLeft (pg); }
            topoR.removeFromLeft (8);
            tubeOsBox.setBounds (topoR.removeFromLeft (72));
        }
        row.removeFromLeft (14);

        // 8 feel knobs — a caption over a dial, sharing the remaining width
        CentreUnitSlider* knobs[8] = { &tubeDriveKnob, &tubeSagKnob, &tubePresKnob, &tubeDepthKnob, &tubeLoadKnob, &tubeIronKnob, &tubeBloomKnob, &tubeOutKnob };
        juce::Label*      labs [8] = { &tubeDriveLbl, &tubeSagLbl, &tubePresLbl, &tubeDepthLbl, &tubeLoadLbl, &tubeIronLbl, &tubeBloomLbl, &tubeOutLbl };
        const int kw = juce::jmax (44, row.getWidth() / 8);
        for (int i = 0; i < 8; ++i)
        {
            auto cell = (i < 7) ? row.removeFromLeft (kw) : row;
            labs[i]->setBounds  (cell.removeFromTop (13));
            knobs[i]->setBounds (cell.reduced (2, 0));
        }
    }
    else
    {
        ampRowBounds = {};
        tubeSimRowBounds = {};
    }

    // Revealed PREAMP + TONE row — ONE merged strip above the poweramp row (signal order: preamp → EQ →
    // poweramp). Preamp picks sit on the LEFT, laid out in signal order: tube · NAME combo · orange GAIN
    // dial · vertical channel radios · BOOST. The tone EQ fills the rest on the RIGHT: violet tone knobs ·
    // response curve · HPF/LPF (after the curve). Each half shows by its own toggle; eqRowBounds stays
    // empty (this single panel is painted via preampRowBounds).
    const bool preShown = hasPreamps && preampPowerBtn.getToggleState();
    const bool eqShown  = eqPowerBtn.getToggleState();
    // Gate controls live in the preamp column (under the device glyphs) → visible with the preamp half.
    gateLabel.setVisible (preShown);
    gateThreshSlider.setVisible (preShown);
    gateLed.setVisible (preShown);
    eqRowBounds = {};
    if (preShown || eqShown)
    {
        preampRowBounds = r.removeFromBottom (frontRowH());
        auto inner = preampRowBounds.reduced (16, 9);

        // ---- REVERB (always shown while the strip is open): TYPE (spring name in its box below) + MIX,
        // carved from the far RIGHT first so it's present whether the preamp half, the tone half, or both. ----
        {
            // preamp-OUT meter — a thin vertical bar at the far right (where the signal leaves the preamp
            // section for the poweramp), i.e. right AFTER the VOL knob.
            inner.removeFromRight (3);
            preampOutMeter.setBounds (inner.removeFromRight (8).reduced (0, 2));
            inner.removeFromRight (6);

            constexpr int rgap = 4, wKnob = 56;   // both knobs one size (matches the EQ tone knobs)
            auto rev = inner.removeFromRight (wKnob * 2 + rgap);
            auto place = [&] (juce::Label& lab, juce::Component& knob, int w)
            {
                auto cell = rev.removeFromLeft (w);
                lab.setBounds (cell.removeFromTop (13));
                knob.setBounds (cell);
                rev.removeFromLeft (rgap);
            };
            place (reverbMixLabel, reverbMixKnob, wKnob);   // REV caption + "MIX" dial — reverb amount (0 = off)
            place (preampVolLabel, preampVolKnob, wKnob);   // rightmost — preamp VOLUME
            inner.removeFromRight (10);
        }

        // ---- preamp picks (left, signal order) — device glyph removed; BOOST stacked under GAIN ----
        if (preShown)
        {
            {   // gear caption · NAME combo · device glyph strip · GATE row (threshold slider + state LED)
                auto col = inner.removeFromLeft (150);
                const int labH = 14, boxH = 28, stripH = 34, gateH = 14, gap = 4;
                auto stk = col.withSizeKeepingCentre (150, labH + boxH + stripH + gateH + 3 * gap);
                preampGearLabel.setBounds   (stk.removeFromTop (labH));
                stk.removeFromTop (gap);
                preampBox.setBounds         (stk.removeFromTop (boxH));
                stk.removeFromTop (gap);
                preampDeviceStrip.setBounds (stk.removeFromTop (stripH));
                stk.removeFromTop (gap);
                // NOISE GATE — the GATE caption + a compact HORIZONTAL threshold slider UNDER the device glyphs,
                // with a grey→green→yellow→red state LED at the far right.
                auto grow = stk.removeFromTop (gateH);
                gateLabel.setBounds (grow.removeFromLeft (34));
                gateLed.setBounds (grow.removeFromRight (14).reduced (1, 1));
                grow.removeFromRight (4);
                gateThreshSlider.setBounds (grow);   // track + TextBoxRight (value / "OFF")
            }
            inner.removeFromLeft (12);

            // GAIN dial with BOOST stacked directly UNDER it (one column) — orange rotary; square cell keeps it round
            if (preampGainSlider.isVisible())
            {
                auto gcol = inner.removeFromLeft (84);
                if (preampBoostBtn.isVisible())
                    preampBoostBtn.setBounds (gcol.removeFromBottom (24).withSizeKeepingCentre (74, 22));
                preampGainSlider.setBounds (gcol.withSizeKeepingCentre (84, juce::jmin (gcol.getHeight(), 84)));
                inner.removeFromLeft (10);
            }
            else if (preampBoostBtn.isVisible())   // no gain dial for this preamp → boost stands on its own
            {
                preampBoostBtn.setBounds (inner.removeFromLeft (74).withSizeKeepingCentre (74, 24));
                inner.removeFromLeft (10);
            }

            // CHANNEL — vertical radio stack of the visible channel buttons (red/green glow)
            int nc = 0; for (auto& b : preampChannelBtn) if (b.isVisible()) ++nc;
            if (nc > 0)
            {
                auto colArea = inner.removeFromLeft (66);   // fits the tone captions (CLEAN / CRUNCH / HI-GAIN)
                const int bh = juce::jlimit (18, 28, (colArea.getHeight() - (nc - 1) * 4) / nc);
                auto stack = colArea.withSizeKeepingCentre (66, nc * bh + (nc - 1) * 4);
                for (auto& b : preampChannelBtn) if (b.isVisible()) { b.setBounds (stack.removeFromTop (bh)); stack.removeFromTop (4); }
                inner.removeFromLeft (10);
            }
        }

        // ---- tone EQ (right) ----
        if (eqShown)
        {
            inner.removeFromLeft (preShown ? 8 : 0);   // small gap after the preamp half (panel paints a divider)

            struct Cell { juce::Slider* knob; juce::Label* label; juce::ToggleButton* toggle; };
            // Presence removed — it was a ~5 kHz high shelf (near Treble's ~2.8 kHz shelf), redundant with
            // Treble and NOT the real NFB amp presence (that lives in the Tube poweramp's own Presence).
            const Cell tone[3] = {
                { &eqBassKnob,   &eqBassLabel,   nullptr },
                { &eqMidKnob,    &eqMidLabel,    nullptr },
                { &eqTrebleKnob, &eqTrebleLabel, nullptr },
            };
            constexpr int kKnobW = 56, kKnobGap = 2;   // tighter spacing than before
            auto place = [&] (const Cell& c, juce::Rectangle<int> cell)
            {
                auto cap = cell.removeFromTop (14);
                if (c.label)  c.label->setBounds (cap);
                if (c.toggle) c.toggle->setBounds (cap.withSizeKeepingCentre (kKnobW, 14));
                c.knob->setBounds (cell);
            };

            for (auto& c : tone) { place (c, inner.removeFromLeft (kKnobW)); inner.removeFromLeft (kKnobGap); }

            // The HPF/LPF freq knobs are gone — the curve fills the rest, and its own left/right edges are the
            // draggable HPF/LPF corners; the enable checkboxes overlay the curve's top-left / top-right corners.
            inner.removeFromLeft (8);
            eqCurve.setBounds (inner.reduced (0, 4));
            {
                auto cb = eqCurve.getBounds();
                eqHpfBtn.setBounds (cb.getX() + 4,      cb.getY() + 3, 54, 18);   // HPF — top-left
                eqLpfBtn.setBounds (cb.getRight() - 58, cb.getY() + 3, 54, 18);   // LPF — top-right
            }
        }

        for (juce::Component* c : { (juce::Component*) &reverbMixLabel, (juce::Component*) &reverbMixKnob,
                                    (juce::Component*) &preampVolLabel, (juce::Component*) &preampVolKnob,
                                    (juce::Component*) &preampOutMeter })
            c->setVisible (true);
    }
    else
    {
        preampRowBounds = {};
        for (juce::Component* c : { (juce::Component*) &reverbMixLabel, (juce::Component*) &reverbMixKnob,
                                    (juce::Component*) &preampVolLabel, (juce::Component*) &preampVolKnob,
                                    (juce::Component*) &preampOutMeter })
            c->setVisible (false);   // reverb rides the strip — hidden when neither Preamp nor EQ is open
    }

    auto strip = r.removeFromBottom (46);
    mixStripBounds = strip;                       // repaint region for the A→B gradient
    versionBadge.setBounds (strip.removeFromRight (120).reduced (12, 6));   // version + 14-digit build no. (two lines), bottom-right
    perfBadge.setBounds    (strip.removeFromRight (140).reduced (6, 15));   // latency + DSP load, just left of it
    // Bottom-left power checkboxes, in signal order: PREAMP, POWERAMP. (The tone EQ is now part of the
    // PREAMP stage — no separate toggle.) Each appears only when its library is non-empty.
    {
        std::array<juce::ToggleButton*, 3> col {};
        int n = 0;
        if (hasPreamps)   col[(size_t) n++] = &preampPowerBtn;
        if (hasPoweramps) col[(size_t) n++] = &ampPowerBtn;
        col[(size_t) n++] = &tubeSimBtn;   // SIMULATOR — always available (needs no .nam library)

        auto amps = strip.removeFromLeft (190).reduced (12, 2);   // wide enough for "POWER AMP SIMULATOR"
                                                                   // at the stacked-toggle font; MIX absorbs the loss
        if (n == 1)
            col[0]->setBounds (amps.withSizeKeepingCentre (amps.getWidth(), 22));
        else
        {
            const int h = amps.getHeight() / n;
            for (int i = 0; i < n; ++i)
                col[(size_t) i]->setBounds (i < n - 1 ? amps.removeFromTop (h) : amps);
        }
    }
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
