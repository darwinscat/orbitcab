// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

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
#include "ui/SnapshotButton.h"
#include "ui/TubeDisplay.h"
#include "ui/DeviceGlyph.h"   // schematic tube / PNP / FET glyphs for the preamp device strip
#include "ui/PreampMenuLNF.h" // draws those glyphs inside the preamp combo's dropdown items
#include "ui/PowerampManager.h"
#include "ui/PreampManager.h"
#include "ui/EqCurve.h"
#include "ui/PerfBadge.h"
#include "PreampSelector.h"   // pure resolve/view-model behind the PREAMP row (GUI-free, unit-tested)
#include "PresetManager.h"
#include "FactoryPresets.h"   // bundled read-only factory presets (combo "Factory" section)

#include <array>
#include <vector>

//==============================================================================
// CentreUnitSlider — the EQ knobs. A plain rotary whose NUMBER shows in the text box BELOW (double-click
// it to type a value); the UNIT (dB / Hz / kHz, set via the "unit" property) is drawn in the dial CENTRE
// by OrbitCabLookAndFeel, with the NAME in the caption above. Named only so the members read clearly.
class CentreUnitSlider : public juce::Slider {};

//==============================================================================
// GateLed — the noise-gate state indicator. Its COLOUR tracks the gate gain like a hardware gate LED:
// GREEN when open (signal passing) → YELLOW mid-transition → RED when closed (muting). Not a level bar.
class GateLed final : public juce::Component
{
public:
    // `active` = the gate is armed (not at the OFF position); `g` = the effective gate gain (0..1).
    void setState (bool active, float g)
    {
        g = juce::jlimit (0.0f, 1.0f, g);
        if (active != on || std::abs (g - gain) > 1.0e-4f) { on = active; gain = g; repaint(); }
    }
    void paint (juce::Graphics& gr) override
    {
        auto r = getLocalBounds().toFloat();
        gr.setColour (juce::Colour (0xff0f0f12));
        gr.fillRoundedRectangle (r, 3.0f);
        juce::Colour c;
        if (! on)
            c = juce::Colour (0xff3a3a42);   // OFF → dim grey (gate inactive)
        else
        {
            const float db = gain > 0.0f ? juce::Decibels::gainToDecibels (gain) : -120.0f;
            // −1 dB (open) → green … −30 dB (closed) → red, through yellow in the middle.
            const float t = juce::jlimit (0.0f, 1.0f, juce::jmap (db, -30.0f, -1.0f, 0.0f, 1.0f));
            const juce::Colour red (0xffe5544e), yellow (0xfff2c94c), green (0xff53c07a);
            c = t < 0.5f ? red.interpolatedWith (yellow, t * 2.0f)
                         : yellow.interpolatedWith (green, (t - 0.5f) * 2.0f);
        }
        auto led = r.reduced (1.5f);
        gr.setColour (c);
        gr.fillRoundedRectangle (led, 2.0f);
        gr.setColour (c.brighter (0.5f).withAlpha (on ? 0.55f : 0.25f));   // subtle glow rim
        gr.drawRoundedRectangle (led, 2.0f, 1.0f);
    }
private:
    float gain = 1.0f;
    bool  on = false;
};

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
                                       public  juce::DragAndDropContainer,   // A/B/C/D drag-copy (button onto button)
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
    using CAtt  = APVTS::ComboBoxAttachment;

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
    orbitcab::ui::SnapshotButton snapBtn[OrbitCabAudioProcessor::kNumSnapshots];   // A/B/C/D compare registers
    void updateSnapshotButtons();                                       // reflect the active register
    void switchSnapshot (int i);                                        // recall register i + re-sync (click or 1-4 key)
    void afterUndoRedo();                                               // re-sync UI after undo/redo
    // Register copy — the engine's copyRegister/applyEdit behind the UI gestures (right-click
    // menu, button drag-n-drop, system clipboard). Clipboard payload = the <Sound> XML.
    void showSnapshotMenu (int i);                                      // right-click on button i
    void applySnapshotCopy (int from, int to);                          // copyRegister + UI re-sync
    void copySnapshotToClipboard (int i);                               // register i's <Sound> → clipboard
    bool pasteSnapshotFromClipboard (int toReg);                        // clipboard → register (validated)
    juce::ComboBox        presetBox;
    std::unique_ptr<juce::Drawable> logo;  // Darwin's Cat mark (drawn inside HeaderBrand)
    HeaderBrand           brand;           // logo + OrbitCab brand -> /orbitcab
    VersionBadge          versionBadge { processorRef.updateChecker(), processorRef.pluginFormat() };   // bottom version + format + update check
    PerfBadge             perfBadge;                                        // latency + DSP load (click → per-stage breakdown)

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
    juce::ToggleButton    ampPowerBtn { "POWER AMP CAPTURES" };   // NAM poweramp captures — one of the two poweramp tabs (radio with SIMULATOR).
    // No APVTS attachment: CAPTURES + SIMULATOR share the ONE poweramp slot (ampOn + ampMode) as a radio,
    // so the toggle is driven manually by syncPowerAmpTabs() and its onClick sets ampOn/ampMode.
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

    // POWERAMP SIMULATOR (white-box tube, blocks 2+3) — the THIRD stage "tab". It shares the ONE poweramp
    // slot with the NAM captures (ampOn + ampMode): SIMULATOR ⊕ CAPTURES is a RADIO — turning one on turns
    // the other off (ampMode = Tube vs Capture). Its reveal row occupies the same poweramp row area (only one
    // ever shows). Unlike captures it needs no .nam library, so its toggle is ALWAYS available.
    juce::ToggleButton    tubeSimBtn { "POWER AMP SIMULATOR" };
    TubeDisplay           tubeSimDisplay;                 // the selected tube ×(PP 2 / SE 1), warm glow
    juce::TextButton      tubeTypeBtn[4];                 // 6L6 / EL34 / EL84 / KT88 → tubeType (radio highlight)
    juce::TextButton      tubeTopoBtn[2];                 // x1 (push-pull) / x2 (single-ended) → tubeTopo
    CentreUnitSlider      tubeDriveKnob, tubeSagKnob, tubePresKnob, tubeDepthKnob, tubeLoadKnob, tubeIronKnob, tubeBloomKnob, tubeOutKnob;
    juce::Label           tubeDriveLbl, tubeSagLbl, tubePresLbl, tubeDepthLbl, tubeLoadLbl, tubeIronLbl, tubeBloomLbl, tubeOutLbl;
    std::unique_ptr<SAtt> tubeDriveAtt, tubeSagAtt, tubePresAtt, tubeDepthAtt, tubeLoadAtt, tubeIronAtt, tubeBloomAtt, tubeOutAtt;
    juce::ComboBox        tubeOsBox;              // OS quality: 4x / 8x HQ (live switch)
    std::unique_ptr<CAtt> tubeOsAtt;
    juce::Rectangle<int>  tubeSimRowBounds;
    int  ampModeCache = 0;                                // detect ampMode change on the timer (host automation)
    void syncPowerAmpTabs();                              // (ampOn, ampMode) → CAPTURES/SIMULATOR toggles + reveal the right row
    void updateTubeSimRow();                              // sim controls visibility + resize (mutually exclusive w/ captures)
    void selectTubeType (int t);                          // set the tubeType choice param + reflect the radio
    void selectTubeTopo (bool singleEnded);              // set the tubeTopo choice param + reflect
    int  tubeSimRowH() const { return showTubesPref ? 118 : 92; }   // taller than captures: display + type/topo + labelled knobs

    // PREAMP (NAM): the SECOND stage's selector — an exact sibling of the poweramp block above,
    // against the preamp library (channel / gain / boost dimensions instead of PP-SE / hours). Its
    // revealed row stacks ABOVE the poweramp row (signal order: input → preamp → poweramp → cab).
    juce::ToggleButton    preampPowerBtn { "PREAMP" };
    std::unique_ptr<BAtt> preampPowerAtt;
    TubeDisplay           preampTubeDisplay;
    orbitcab::PreampSelector                       preampSel;          // pure resolve/view-model (owns the merged library snapshot)
    orbitcab::ui::PreampMenuLNF preampMenuLnf;   // draws device glyphs in the combo popup (declared BEFORE preampBox → outlives it)
    juce::Label           preampGearLabel;       // ABOVE the combo — "what gear is this" (from metadata gear_make/model)
    juce::ComboBox        preampBox;             // unified NAME picker: families + singletons, grouped Factory / User
    orbitcab::ui::DeviceStrip preampDeviceStrip; // BELOW the combo — N tube / PNP / FET schematic glyphs (from metadata)
    std::map<juce::String, orbitcab::ui::DeviceSpec> preampItemDevice;   // combo item text → device spec (may be hybrid)
    // What each preampBox item id selects: {true,name} = a model family (→ resolveName); {false,id} = a singleton entry.
    std::vector<std::pair<bool, juce::String>> preampBoxTargets;   // index = item id - 1
    juce::TextButton      preampChannelBtn[4];   // contextual channel switch (up to 4; chN or colour-tinted; shown when ≥2 exist) — stacked vertically as radios
    int                   preampChannelBtnCh[4] {};   // channel value each switch slot currently maps to (set in syncPreampSelector)
    juce::Slider          preampGainSlider;      // contextual gain — orange discrete rotary over the available <N>h (clock positions)
    std::vector<int>      preampGainVals;        // gain hour at each stop (sorted; snaps to these)
    juce::TextButton      preampBoostBtn { "BOOST" };   // contextual boost toggle (shown when both on+off variants exist)

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

    // AMP EQ (teq): a fixed-frequency tone stack (Bass/Mid/Treble) + Presence + an HPF/LPF
    // "tightening" pair. A revealed row BETWEEN the preamp and poweramp rows (signal order:
    // input → preamp → EQ → poweramp → cab). eqOn gates the DSP and reveals the row. No library,
    // so — unlike the NAM rows — the toggle is ALWAYS shown (even on a public build with no .nam).
    juce::ToggleButton    eqPowerBtn { "AMP EQ" };
    std::unique_ptr<BAtt> eqPowerAtt;
    CentreUnitSlider      eqBassKnob, eqMidKnob, eqTrebleKnob, eqPresenceKnob;   // (HPF/LPF freq knobs removed — curve edges drag)
    juce::Label           eqBassLabel, eqMidLabel, eqTrebleLabel, eqPresenceLabel;        // static tone captions
    juce::ToggleButton    eqHpfBtn { "HPF" }, eqLpfBtn { "LPF" };                         // enable toggles — now overlaid on the curve corners
    std::unique_ptr<SAtt> eqBassAtt, eqMidAtt, eqTrebleAtt, eqPresenceAtt;
    std::unique_ptr<BAtt> eqHpfOnAtt, eqLpfOnAtt;
    EqCurve               eqCurve;                                                        // live frequency-response curve (teq::EqEngine::magnitudeDbFor)
    juce::Rectangle<int>  eqRowBounds;                                                    // painted panel region of the revealed EQ row
    bool eqOnCache = false;                                                               // detect eqOn change on the timer (host automation)
    bool eqDiscretePref = true;                                                           // config: tone knobs snap to whole-dB steps (HPF/LPF stay smooth)
    void updateEqRow();                                                                   // reveal/hide the row + resize
    int  eqRowH()    const { return 104; }   // (legacy, unused since the merge)
    int  frontRowH() const { return 128; }   // merged PREAMP+TONE strip: combo/gain/channel/boost + GATE row (left) + tone knobs/curve/HPF-LPF (right)

    // REVERB — in-amp spring tank (after EQ, before poweramp). Lives in the merged PREAMP+TONE strip
    // (far right), so it shows whenever that strip is open (Preamp or EQ on). A guitar-style DISCRETE
    // TYPE rotary (Off + the 4 springs; the "reverbType" choice attachment snaps it to whole steps and
    // drives the text box BELOW from the param's getText → the active spring NAME is the caption under
    // the knob), plus a MIX return knob. Off is a knob position (zero CPU) — no separate reveal toggle.
    CentreUnitSlider reverbMixKnob;        // the reverb amount / on-off (0 = off), captioned "REV"
    juce::Label      reverbMixLabel;       // "REV" caption (above the reverb knob)
    std::unique_ptr<SAtt> reverbMixAtt;
    // Preamp VOLUME — the rightmost knob in this row (a preamp param, placed here per Oleh's layout).
    CentreUnitSlider preampVolKnob;
    juce::Label      preampVolLabel;
    std::unique_ptr<SAtt> preampVolAtt;
    LevelMeter       preampOutMeter;   // thin vertical meter at the strip's right edge — preamp OUT (feeds the poweramp)

    // NOISE GATE — in-amp gate (detector on the clean input, VCA after the EQ; the spring tail rings out).
    // A GATE caption + a compact HORIZONTAL threshold slider sit at the BOTTOM-LEFT under the preamp device
    // glyphs (no on/off toggle — OFF = drag the slider left); a GateLed shows the state: grey (off) → green
    // (open) → yellow → red (closed).
    juce::Label           gateLabel;   // "GATE" caption for the row (no on/off toggle — OFF = drag the slider left)
    juce::Slider          gateThreshSlider;
    std::unique_ptr<SAtt> gateThreshAtt;
    GateLed               gateLed;

    static constexpr int  kBaseHeight = 620;
    int ampRowH()    const { return showTubesPref ? 90 : 54; }   // tall row with tubes, slim strip (amp icon stays) without
    int preampRowH() const { return ampRowH(); }                 // (legacy)

    // Window height = base + the merged front strip (revealed when the preamp and/or the EQ is on) + the
    // poweramp row when revealed. One helper so the toggles can't disagree on the total.
    void resizeForAmpRows()
    {
        const bool front = (hasPreamps && preampPowerBtn.getToggleState()) || eqPowerBtn.getToggleState();
        // CAPTURES and SIMULATOR are mutually exclusive (radio) and share the poweramp row slot — at most
        // one is on, so the poweramp band's height is the sim row's when SIMULATOR is on, else the captures row's.
        const int paH = tubeSimBtn.getToggleState() ? tubeSimRowH()
                      : (hasPoweramps && ampPowerBtn.getToggleState() ? ampRowH() : 0);
        setSize (1040, kBaseHeight + (front ? frontRowH() : 0) + paH);
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

    // Blend phase tint (gear panel, global view pref, default ON): a log-f strip under the
    // MIX rail tinting where the A/B blend cancels (red) / reinforces (green). The curve is
    // computed processor-side on the prepared IRs (debounced, message thread); the editor
    // polls blendTintRevision() on its timer and repaints the strip on change. It only ever
    // shows for a real two-sided blend (both slots + MIX between the ends), so the default-on
    // costs nothing in the common single-IR workflow.
    bool blendTintPref = true;                   // persisted under "blendTint"
    juce::uint32 blendTintSeenRev = 0;           // last revision painted

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
    juce::uint32 lastHistoryRev = 0xffffffff;   // ≠ 0 so the first tick always syncs the history UI
                                                // (undo/redo enablement + A/B/C/D dots) from the engine

    // Open slider gestures (normally 0 or 1; >1 = nested). Counted so the destructor can drain a
    // gesture left open by a window destroyed mid-drag (the engine outlives the editor — a leaked
    // refcount would stop future drags from committing undo steps), and so history-navigation
    // shortcuts (1-4 switch, ⌘V paste) go inert while the mouse is mid-drag.
    int openParamGestures = 0;

    // The actual file the current preset was loaded from / saved to THIS session — the Save /
    // Delete target. Tracking the file (not matching by name) stops Save overwriting a different
    // library preset that happens to share the imported preset's name (doc#7). Empty after a
    // host reload recreated the editor → currentPresetFile() falls back to a name match.
    juce::File loadedPresetFile;

    // Hosts the hover hints for every control (without this, setTooltip does nothing).
    juce::TooltipWindow tooltipWindow { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrbitCabAudioProcessorEditor)
};
