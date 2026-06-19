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
#include "ui/SlotComponent.h"
#include "PresetManager.h"

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

    void pushFiltersToWave();                 // push both SlotComponents' wave overlays
    void snapMixToCentre();                   // MIX param -> centre (B's first IR load hook)
    void styleLabel (juce::Label&, const juce::String&);

    // presets
    void refreshPresets();
    void promptSavePreset();
    void loadPresetFile (const juce::File&);
    void applyDefaultPreset();          // factory "Default": IR 16 + HPF, single box
    void exportPreset();                // → .orbitcab at a chosen location (IR embedded)
    void importPreset();                // ← .orbitcab from disk

    OrbitCabAudioProcessor& processorRef;
    OrbitCabLookAndFeel     lnf;

    SlotComponent slots[2] { { processorRef, 0 }, { processorRef, 1 } };

    // header
    juce::TextButton      saveBtn { "Save" };
    IconButton            exportBtn { IconButton::Kind::exportFile };   // ↑ export .orbitcab (IR inside)
    IconButton            importBtn { IconButton::Kind::importFile };   // ↓ import .orbitcab
    IconButton            undoBtn   { IconButton::Kind::undo };
    IconButton            redoBtn   { IconButton::Kind::redo };
    juce::TextButton      spectrumBtn { juce::String::fromUTF8 ("\xe2\x89\x88") };  // toggle the analyser
    juce::TextButton      snapBtn[OrbitCabAudioProcessor::kNumSnapshots];   // A/B/C/D compare registers
    void updateSnapshotButtons();                                       // reflect the active register
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

    void updateEnablement();    // dim a muted/empty slot's WF+controls; disable MIX when not A&B

    // pre/post spectrum analyser (drawn faint inside the waveforms)
    void updateSpectrum();
    SpectrumAnalyser spectrum;   // display-side pre/post analyser (extracted)

    // enablement caches (so we only re-dim on change) + MIX strip bounds for its gradient
    bool slotOnCache[2] { true, true };   // per-slot active-state cache (drives SlotComponent::setActive)
    bool mixOnCache = true;
    juce::Rectangle<int> mixStripBounds, inBlockBounds, outBlockBounds;

    orbitcab::PresetManager presets { processorRef };   // preset file/state I/O
    juce::Array<juce::File> presetFiles;                 // combo id -> file (3+); UI mapping
    std::unique_ptr<juce::AlertWindow> saveDialog;
    std::unique_ptr<juce::FileChooser> chooser;

    // Hosts the hover hints for every control (without this, setTooltip does nothing).
    juce::TooltipWindow tooltipWindow { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrbitCabAudioProcessorEditor)
};
