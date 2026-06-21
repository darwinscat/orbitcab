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
        if      (id == 2)                                  applyDefaultPreset();      // factory
        else if (id >= 3 && id - 3 < presetFiles.size())   loadPresetFile (presetFiles[id - 3]);
    };
    saveBtn.onClick = [this] { promptSavePreset(); };
    saveBtn.setTooltip ("Save preset to the library");
    addAndMakeVisible (saveBtn);

    // Export / Import a .orbitcab preset file (IR embedded). Icon-only buttons, right of Save.
    exportBtn.setTooltip ("Export preset to a .orbitcab file (IR included)");
    exportBtn.onClick = [this] { exportPreset(); };
    addAndMakeVisible (exportBtn);
    importBtn.setTooltip ("Import a .orbitcab preset");
    importBtn.onClick = [this] { importPreset(); };
    addAndMakeVisible (importBtn);

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
        b.setTooltip ("Snapshot " + juce::String (snapNames[i]));
        b.onClick = [this, i]
        {
            processorRef.switchToSnapshot (i);
            slots[0].syncFromProcessor();          // recall may have swapped the IRs / refs
            slots[1].syncFromProcessor();
            pushFiltersToWave();
            updateSnapshotButtons();
        };
        addAndMakeVisible (b);
    }
    updateSnapshotButtons();

    // ---- slots ----  (two self-contained SlotComponents; the editor only wires the
    // two cross-slot hooks: rebuild BOTH lists when the shared user-IR history changes,
    // and snap MIX to centre when B's first IR loads.)
    for (auto& slot : slots)
    {
        slot.onUserIRsChanged = [this] { slots[0].rebuildList(); slots[1].rebuildList(); };
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

    // ---- IR library + restore display ----
    slots[0].rebuildList();
    slots[1].rebuildList();
    slots[0].syncFromProcessor();
    slots[1].syncFromProcessor();
    dryWetPref = processorRef.appPreferences().getFlag ("dryWetShown", false);   // global view pref
    refreshDryWetVisibility();   // applies the pref, or force-shows if a loaded blend has mix ≠ 100%
    pushFiltersToWave();
    refreshPresets();

    startTimerHz (30);
    setSize (1040, 620);
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

    processorRef.undoTick();                       // coalesce edits into undo steps
    undoBtn.setEnabled (processorRef.canUndo());
    redoBtn.setEnabled (processorRef.canRedo());
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

void OrbitCabAudioProcessorEditor::openSettings()
{
    auto panel = std::make_unique<SettingsPanel> (
        processorRef.getHeadTrim(), dryWetPref, spectrumEnabled,
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
        });

    // Parent the pop-over to the editor (not the desktop) so it can't outlive the window
    // (the version call-out orphan bug, #52). areaToPointTo is in the editor's coords.
    juce::CallOutBox::launchAsynchronously (std::move (panel), settingsBtn.getBounds(), this);
}

//==============================================================================
void OrbitCabAudioProcessorEditor::styleLabel (juce::Label& l, const juce::String& t)
{
    l.setText (t, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    addAndMakeVisible (l);
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
    presetBox.addItem ("(Custom)", 1);
    presetBox.addItem ("Default",  2);                 // factory starting point (IR 16 + HPF)

    int id = 3;                                         // 1 = (Custom), 2 = Default, 3+ = user files
    for (const auto& f : presets.list())
    {
        presetFiles.add (f);
        presetBox.addItem (f.getFileNameWithoutExtension(), id++);
    }
    presetBox.setSelectedId (1, juce::dontSendNotification);
}

void OrbitCabAudioProcessorEditor::promptSavePreset()
{
    saveDialog = std::make_unique<juce::AlertWindow> ("Save preset", "Preset name:",
                                                      juce::MessageBoxIconType::NoIcon);
    saveDialog->addTextEditor ("name", "My Preset");
    saveDialog->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
    saveDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    saveDialog->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, safe = juce::Component::SafePointer<OrbitCabAudioProcessorEditor> (this)] (int result)
        {
            if (safe == nullptr) return;   // editor gone before the dialog closed
            if (result == 1)
            {
                const auto name = saveDialog->getTextEditorContents ("name").trim();
                if (name.isNotEmpty())
                {
                    const auto file = presets.saveAs (name);
                    refreshPresets();
                    for (int i = 0; i < presetFiles.size(); ++i)
                        if (presetFiles[i] == file)
                            presetBox.setSelectedId (i + 3, juce::dontSendNotification);
                }
            }
            saveDialog.reset();
        }), false);
}

void OrbitCabAudioProcessorEditor::loadPresetFile (const juce::File& file)
{
    if (! presets.loadFrom (file))
        return;
    slots[0].rebuildList();                        // restored user-IR history may differ
    slots[1].rebuildList();
    slots[0].syncFromProcessor();
    slots[1].syncFromProcessor();
    pushFiltersToWave();
    updateSnapshotButtons();
}

void OrbitCabAudioProcessorEditor::exportPreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Export preset",
                                                   orbitcab::PresetManager::directory().getChildFile ("My Preset.orbitcab"), "*.orbitcab");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
        [this] (const juce::FileChooser& fc) { presets.writeTo (fc.getResult()); });
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
            loadPresetFile (file);
            presetBox.setSelectedId (1, juce::dontSendNotification);   // external → "(Custom)"
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
            loadPresetFile (juce::File (f));
            presetBox.setSelectedId (1, juce::dontSendNotification);   // external → "(Custom)"
            break;
        }
}

void OrbitCabAudioProcessorEditor::applyDefaultPreset()
{
    // Factory starting point: a clean single-box sound — Emerald IR #16 in box I with
    // the HPF engaged. Reset every parameter to its default first so the result is
    // reproducible regardless of the current state, then load the IR + arm the HPF.
    auto& ap = processorRef.apvts;
    for (auto* p : processorRef.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            rp->setValueNotifyingHost (rp->getDefaultValue());

    processorRef.clearSlotB();                          // box II empty (single IR)

    slots[0].selectBundledStartingWith ("16");          // box I → 16-*.wav (factory default)

    if (auto* q = ap.getParameter ("hpfOnA"))
        q->setValueNotifyingHost (1.0f);                // HPF on (default 80 Hz)

    slots[0].syncFromProcessor();
    slots[1].syncFromProcessor();
    pushFiltersToWave();
    updateSnapshotButtons();
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
    vpanel (getLocalBounds().removeFromBottom (46).toFloat(), 0xff24242b, 0xff17171c, 0.0f);

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

    auto headerRight = header.removeFromRight (410 + 124);      // save+export+import+≈ + A/B/C/D cluster
    auto rightBar    = headerRight.withSizeKeepingCentre (headerRight.getWidth(), kCtlBand);
    settingsBtn.setBounds (rightBar.removeFromRight (40).reduced (7));
    importBtn.setBounds   (rightBar.removeFromRight (34).reduced (5, 7));
    exportBtn.setBounds   (rightBar.removeFromRight (34).reduced (5, 7));
    saveBtn.setBounds     (rightBar.removeFromRight (60).reduced (7));
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
    auto strip = r.removeFromBottom (46);
    mixStripBounds = strip;                       // repaint region for the A→B gradient
    versionBadge.setBounds (strip.removeFromLeft (84).reduced (12, 15));    // bottom-left version
    mixABLabel.setBounds (strip.removeFromLeft (146).reduced (10, 0));
    mixABSlider.setBounds (strip.reduced (40, 12));
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
