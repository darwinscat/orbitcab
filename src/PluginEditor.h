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
#include "ui/PowerampManager.h"
#include "ui/PreampManager.h"
#include "PreampSelector.h"   // pure resolve/view-model behind the PREAMP row (GUI-free, unit-tested)
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

    // POWERAMP (NAM): the "POWERAMP" power checkbox in the bottom strip reveals a row below (window
    // grows by ampRowH) with the symbolic amp + glowing tubes and a NAME-FIRST selector:
    //   • captures that share a display name with ≥2 variants form a GROUP — a name button, plus a
    //     contextual PP/SE toggle (only when both modes exist) and contextual hours segments (only
    //     when the name+mode has several "<N>h" clock positions);
    //   • a one-off capture (a name with a single variant) is a SINGLETON — listed in a combo by its
    //     full filename.
    // ampOn gates + reveals; the chosen model is library state ("ampSel"), not a host param.
    juce::ToggleButton    ampPowerBtn { "POWERAMP" };   // it's a poweramp, not a full amp (preamp+poweramp)
    std::unique_ptr<BAtt> ampPowerAtt;
    TubeDisplay           tubeDisplay;          // symbolic amp + glowing tubes (count by mode: PP 2 / SE 1 / Other 0)
    std::vector<orbitcab::PowerampEntry>           ampLib;          // cached merged library (rebuilt on add/remove)

    std::vector<juce::String>                      ampGroupNames;   // distinct names with ≥2 variants (ordered)
    std::vector<std::unique_ptr<juce::TextButton>> ampNameBtns;     // one per group name
    juce::TextButton      ampModeBtn[2];         // contextual PP / SE toggle (shown when the name has both)
    juce::Slider          ampHourSlider;         // contextual horizontal discrete slider over the available <N>h
    std::vector<int>      ampHourVals;           // hour value at each slider index (sorted; snaps to these stops)
    juce::ComboBox        ampSingleBox;          // SINGLETONS (one-off amps) listed by full filename

    void rebuildAmpSelector();                   // rescan library → groups/singletons → (re)create controls
    void selectAmpName   (const juce::String& name);          // pick a group (keep mode/hours if they exist there)
    void selectAmpMode   (orbitcab::PowerampCat c);           // switch PP/SE within the current name
    void selectAmpHours  (int hours);                         // switch position within the current name+mode
    void configureHourSlider();                  // set the slider's stops to the current name+mode positions
    void updateAmpRow();                         // show/hide the revealed row + resize the editor
    void syncAmpSelector();                      // reflect "ampSel" → highlight + contextual controls + tubes

    // Library queries, computed from ampLib (small vector — no caching needed):
    std::vector<orbitcab::PowerampCat> catsForName    (const juce::String& name) const;          // distinct modes present
    std::vector<int>                   hoursForNameCat (const juce::String& name, orbitcab::PowerampCat c) const;
    juce::String findAmpId   (const juce::String& name, orbitcab::PowerampCat c, int hours) const;  // matching entry id ("" = none)
    bool         isGroupName (const juce::String& name) const;     // ≥2 entries share this display name
    const orbitcab::PowerampEntry* ampEntryById (const juce::String& id) const;

    juce::Rectangle<int>  ampRowBounds;          // painted panel region of the revealed row
    juce::String ampSyncedId;                    // last selection reflected (timer re-syncs only on change)
    bool ampOnCache = false;                     // detect ampOn change on the timer (host automation)
    bool showTubesPref = true;                   // gear "Show tubes" view pref (default on) — shared by both rows
    bool hasPoweramps = false;                   // library non-empty (factory or user) → show the POWERAMP UI

    // PREAMP (NAM): the SECOND stage's selector — an exact sibling of the poweramp block above,
    // against the preamp library (channel / gain / boost dimensions instead of PP-SE / hours). Its
    // revealed row stacks ABOVE the poweramp row (signal order: input → preamp → poweramp → cab).
    juce::ToggleButton    preampPowerBtn { "PREAMP" };
    std::unique_ptr<BAtt> preampPowerAtt;
    TubeDisplay           preampTubeDisplay;
    orbitcab::PreampSelector                       preampSel;          // pure resolve/view-model (owns the merged library snapshot)
    std::vector<juce::String>                      preampGroupNames;   // group names, ordered (mirrors preampSel.groupNames(); maps name buttons)
    std::vector<std::unique_ptr<juce::TextButton>> preampNameBtns;     // one per group name
    juce::TextButton      preampChannelBtn[3];   // contextual 3-way channel switch (ch1/ch2/ch3; shown when ≥2 exist)
    juce::Slider          preampGainSlider;      // contextual horizontal discrete slider over the available <N>h (gain)
    std::vector<int>      preampGainVals;        // gain hour at each slider index (sorted; snaps to these stops)
    juce::TextButton      preampBoostBtn { "BOOST" };   // contextual boost toggle (shown when both on+off variants exist)
    juce::ComboBox        preampSingleBox;       // SINGLETONS (one-off preamps) listed by full filename

    void rebuildPreampSelector();                // rescan library → groups/singletons → (re)create controls
    void selectPreampName    (const juce::String& name);   // pick a group (keep channel/gain/boost if they exist there)
    void selectPreampChannel (int channel);                // switch channel within the current name
    void selectPreampGain    (int hours);                  // switch gain position within the current name+channel
    void selectPreampBoost   (bool boost);                 // switch boost within the current name+channel+gain
    void updatePreampRow();                      // show/hide the revealed row + resize the editor
    void syncPreampSelector();                   // reflect "preampSel" → highlight + contextual controls + tubes
    // (the resolution + visibility policy lives in orbitcab::PreampSelector; these are thin bindings.)

    juce::Rectangle<int>  preampRowBounds;       // painted panel region of the revealed preamp row
    juce::String preampSyncedId;                 // last selection reflected (timer re-syncs only on change)
    bool preampOnCache = false;                   // detect preampOn change on the timer (host automation)
    bool hasPreamps = false;                      // library non-empty (factory or user) → show the PREAMP UI

    static constexpr int  kBaseHeight = 620;
    int ampRowH()    const { return showTubesPref ? 90 : 54; }   // tall row with tubes, slim strip (amp icon stays) without
    int preampRowH() const { return ampRowH(); }                 // same geometry as the poweramp row

    // Window height = base + whichever NAM rows are revealed (each stage independent). One helper so
    // updateAmpRow()/updatePreampRow() can't disagree on the total. setSize is a no-op when unchanged.
    void resizeForAmpRows()
    {
        setSize (1040, kBaseHeight
                       + (hasPreamps   && preampPowerBtn.getToggleState() ? preampRowH() : 0)
                       + (hasPoweramps && ampPowerBtn.getToggleState()    ? ampRowH()    : 0));
    }

    void updateEnablement();    // dim a muted/empty slot's WF+controls; disable MIX when not A&B

    void openSettings();         // gear → CallOutBox: HEAD / Dry-Wet / spectrum toggles
    void openPowerampManager();  // settings "Manage library…" → PowerampManager pop-over (Add/Remove .nam)
    void openPreampManager();    // settings "Manage library…" → PreampManager pop-over (Add/Remove .nam)
    juce::Component::SafePointer<juce::CallOutBox> settingsCallout;   // dismiss it when opening the manager

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
