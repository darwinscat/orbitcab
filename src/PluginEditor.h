// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "ui/OrbitCabLookAndFeel.h"
#include "ui/WaveformDisplay.h"
#include "ui/LevelMeter.h"
#include "ui/HeaderBrand.h"
#include "ui/IconButton.h"
#include "ui/VersionBadge.h"
#include "ui/SpectrumAnalyser.h"
#include "ui/SettingsPanel.h"
#include "ui/SlotComponent.h"
#include "ui/TubeDisplay.h"
#include "PresetManager.h"
#include "FactoryPresets.h"   // bundled read-only factory presets (combo "Factory" section)

#include <array>
#include <vector>

//==============================================================================
// OrbitCab editor — direct-manipulation layout. Two IR slots A|B side by
// side, each a full channel: browser (Open file/folder, ‹ ›, click-name popup) + a
// waveform hosting TRIM (drag) and the HPF/LPF EQ curve, plus per-slot control rows
// (HPF/LPF toggle+freq, TRIM/PHASE toggles, DRY/WET). Left = INPUT block (bypass +
// input fader + IN meter), right = OUTPUT block (AUTO + master/mix-volume fader + OUT
// meter), bottom-centre = MIX (A↔B crossfade).
//==============================================================================
class OrbitCabAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                       public  juce::FileDragAndDropTarget,
                                       private juce::Timer
{
public:
    explicit OrbitCabAudioProcessorEditor (OrbitCabAudioProcessor&);
    ~OrbitCabAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Drop a .orbitcab preset anywhere on the editor to load it.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    using APVTS = juce::AudioProcessorValueTreeState;
    using SAtt  = APVTS::SliderAttachment;
    using BAtt  = APVTS::ButtonAttachment;

    void timerCallback() override;
    bool keyPressed (const juce::KeyPress& key) override;   // 1/2/3/4 → A/B/C/D snapshot switch

    void pushFiltersToWave();                 // push both SlotComponents' wave overlays
    void snapMixToCentre();                   // MIX param -> centre (B's first IR load hook)
    void styleLabel (juce::Label&, const juce::String&);

    // presets (preset-centric: the live state IS the current preset; the combo shows its
    // name with a " *" dirty marker, Save writes back to a user preset / forks a factory one)
    void refreshPresets();
    void promptSavePreset (const juce::String& initialName = {});   // Save As: prompt a name → fork (re-prompts on a name clash)
    void saveCurrentPreset();           // Save: write back to the current user preset, else Save As
    void loadPresetFile (const juce::File&);
    void loadFactoryPreset (const orbitcab::FactoryPreset&);   // load a bundled read-only factory preset
    void applyDefaultPreset();          // first-start / reset → the bundled default (Roche Limit)
    void exportPreset();                // → .orbitcab at a chosen location (IR embedded)
    void importPreset();                // ← .orbitcab from disk
    void deleteCurrentPreset();         // move the current user preset to the Trash (factory/none: no-op)
    void updatePresetDisplay();         // reflect the current preset's name + dirty in the combo
    juce::File currentPresetFile() const;   // the library file backing the current preset, or {} (factory/external)
    juce::String nextCopyName (const juce::String& base) const;   // "<base> (copy N)" — first name not already taken

    OrbitCabAudioProcessor& processorRef;
    OrbitCabLookAndFeel     lnf;

    SlotComponent slots[2] { { processorRef, 0 }, { processorRef, 1 } };

    // header
    IconButton            saveBtn   { IconButton::Kind::save };          // write back to the current user preset
    IconButton            saveAsBtn { IconButton::Kind::saveAs };        // fork a new named preset
    IconButton            exportBtn { IconButton::Kind::exportFile };   // ↑ export .orbitcab (IR inside)
    IconButton            importBtn { IconButton::Kind::importFile };   // ↓ import .orbitcab
    IconButton            trashBtn  { IconButton::Kind::trash };        // delete current user preset (factory: disabled)
    IconButton            undoBtn   { IconButton::Kind::undo };
    IconButton            redoBtn   { IconButton::Kind::redo };
    IconButton            settingsBtn { IconButton::Kind::settings };       // gear → settings pop-over
    juce::TextButton      snapBtn[OrbitCabAudioProcessor::kNumSnapshots];   // A/B/C/D compare registers
    void updateSnapshotButtons();                                       // reflect the active register
    void switchSnapshot (int i);                                        // recall register i + re-sync (click or 1-4 key)
    void afterUndoRedo();                                               // re-sync UI after undo/redo
    juce::ComboBox        presetBox;
    std::unique_ptr<juce::Drawable> logo;  // Darwin's Cat mark (drawn inside HeaderBrand)
    HeaderBrand           brand;           // logo + OrbitCab brand -> /orbitcab
    VersionBadge          versionBadge { processorRef.updateChecker() };   // bottom-left version + update check

    // INPUT block (left): bypass + input fader + IN meter
    juce::ToggleButton bypassBtn { "BYP" };
    juce::Slider       inGainFader;
    LevelMeter         inMeter;
    juce::Label        inLabel;
    std::unique_ptr<SAtt> inGainAtt;
    std::unique_ptr<BAtt> bypassAtt;

    // OUTPUT block (right): AUTO + master (mix-volume) fader + OUT meter
    juce::ToggleButton autoBtn { "AUTO" };
    juce::Slider       masterFader;
    LevelMeter         outMeter;
    juce::Label        outLabel;
    std::unique_ptr<SAtt> masterAtt;
    std::unique_ptr<BAtt> autoAtt;

    // MIX (A<->B) — bottom centre
    juce::Slider mixABSlider;
    juce::Label  mixABLabel;
    std::unique_ptr<SAtt> mixABAtt;

    // POWERAMP (NAM): a "POWERAMP" power checkbox sits in the bottom strip by the version; toggling
    // it reveals a row below (window grows by ampRowH) carrying the symbolic amp + glowing tubes, a
    // row of model buttons, and a 3-position mode switch (PP / SE / Other) where the old 2x checkbox
    // sat. The mode switch FILTERS the model row: PP shows only push-pull models, SE only single-
    // ended, Other the rest. ampOn drives the gate + reveal; the chosen model is library state
    // ("ampSel"), not a host param; the active mode is derived from the selected model's category.
    juce::ToggleButton    ampPowerBtn { "POWERAMP" };   // it's a poweramp, not a full amp (preamp+poweramp)
    std::unique_ptr<BAtt> ampPowerAtt;
    TubeDisplay           tubeDisplay;          // symbolic amp + glowing tubes (count by mode: PP 2 / SE 1 / Other 0)
    std::vector<orbitcab::PowerampEntry>           ampLib;       // cached merged library (rebuilt on add/remove)
    std::vector<std::unique_ptr<juce::TextButton>> ampSelBtns;   // one button per library entry (shown if cat == ampCat)
    juce::TextButton      ampCatBtn[3];          // mode switch (push-pull / single / other); labels set in the ctor
    int  ampCat = 0;                             // active mode index: 0=PP, 1=SE, 2=Other (view filter)
    void rebuildAmpSelector();                   // rescan the library + (re)create the model buttons
    void setAmpCategory (int cat);               // switch the visible mode + select its first model
    void updateAmpRow();                         // show/hide the revealed row + resize the editor
    void syncAmpTubes();                         // reflect the "ampSel" selection onto mode/buttons/tubes
    int  ampCatCount (int cat) const;            // how many library models fall in a mode (0 → disable that switch)
    juce::Rectangle<int>  ampRowBounds;          // painted panel region of the revealed row
    bool ampOnCache = false;                     // detect ampOn change on the timer (host automation)
    bool showTubesPref = true;                   // gear "Show tubes" view pref (default on)
    bool hasPoweramps = false;                   // library non-empty (factory or user) → show the POWERAMP UI

    static int         ampCatIndex (orbitcab::PowerampCat c)   // category → switch index (0/1/2)
    { return c == orbitcab::PowerampCat::pushPull ? 0 : c == orbitcab::PowerampCat::singleEnded ? 1 : 2; }
    static constexpr int  kBaseHeight = 620;
    int ampRowH() const { return showTubesPref ? 90 : 54; }   // tall row with tubes, slim strip (amp icon stays) without

    void updateEnablement();    // dim a muted/empty slot's WF+controls; disable MIX when not A&B

    void openSettings();         // gear → CallOutBox: HEAD / Dry-Wet / spectrum toggles

    // pre/post spectrum analyser (drawn faint inside the waveforms). `spectrumEnabled` mirrors
    // the gear-panel toggle; it's a global view preference (default on) persisted via the
    // UpdateChecker's PropertiesFile under "spectrumOn", seeded in the ctor.
    void updateSpectrum();
    SpectrumAnalyser spectrum;   // display-side pre/post analyser (extracted)
    bool spectrumEnabled = true;

    // Global view pref (gear panel, default off): reveal the per-slot Dry/Wet sliders.
    // Persisted via the UpdateChecker's PropertiesFile under "dryWetShown", seeded in the ctor.
    // The sliders are ALSO force-shown whenever a slot's Dry/Wet mix ≠ 100% (a non-default
    // blend arriving from saved state, a preset, or host automation) so an active blend is
    // never an invisible "ghost knob". refreshDryWetVisibility() applies pref || blend-active.
    bool dryWetPref       = false;   // user's gear-panel preference (what the toggle reflects)
    bool dryWetShownCache = false;   // last applied effective visibility (so we re-lay-out only on change)
    void refreshDryWetVisibility();

    // Waveform amplitude scale (gear panel, global view prefs): log (dB) vs linear + dB floor.
    // Persisted under "waveLog" / "waveFloor"; default log on, −48 dB. Seeded in the ctor.
    bool waveLogPref   = true;
    int  waveFloorPref = -48;
    void applyWaveformScale();   // push the pref to both slots' waveforms

    // enablement caches (so we only re-dim on change) + MIX strip bounds for its gradient
    bool slotOnCache[2] { true, true };   // per-slot active-state cache (drives SlotComponent::setActive)
    bool mixOnCache = true;
    juce::Rectangle<int> mixStripBounds, inBlockBounds, outBlockBounds;

    orbitcab::PresetManager presets { processorRef };   // preset file/state I/O
    juce::Array<juce::File> presetFiles;                 // combo id -> user file (1000+); UI mapping
    std::vector<orbitcab::FactoryPreset> factoryList;    // combo id -> bundled factory preset (100+)
    std::unique_ptr<juce::AlertWindow> saveDialog;
    std::unique_ptr<juce::FileChooser> chooser;
    // Cache so the 30 Hz timer only re-touches the combo when the shown preset / dirty changes.
    int          presetShownId    = -1;
    juce::String presetShownLabel;

    // Last-seen processor revision counters — polled on the timer to re-sync the slot display /
    // A/B/C/D buttons / recents after any processor-side state change (incl. host setStateInformation).
    juce::uint32 lastSoundRev = 0, lastWorkspaceRev = 0, lastUserIRRev = 0;

    // The actual file the current preset was loaded from / saved to THIS session — the Save /
    // Delete target. Tracking the file (not matching by name) stops Save overwriting a different
    // library preset that happens to share the imported preset's name (doc#7). Empty after a
    // host reload recreated the editor → currentPresetFile() falls back to a name match.
    juce::File loadedPresetFile;

    // Hosts the hover hints for every control (without this, setTooltip does nothing).
    juce::TooltipWindow tooltipWindow { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrbitCabAudioProcessorEditor)
};
