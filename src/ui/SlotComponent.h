// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "WaveformDisplay.h"

#include <functional>
#include <memory>
#include <vector>

//==============================================================================
// SlotComponent — one IR slot (A or B): a mute badge + the IR browser (name popup,
// Open file/folder, ‹ ›) + a WaveformDisplay hosting the direct-manipulation controls
// (TRIM drag, HPF/LPF EQ points) + the per-slot enable-checkbox row and an optional
// Dry/Wet slider (revealed from the gear panel).
// Owns its APVTS attachments and its IR list. Two instances replace the editor's
// `slots[2]` struct + the duplicated per-slot methods (editor decomposition).
//==============================================================================
class SlotComponent final : public juce::Component
{
public:
    SlotComponent (OrbitCabAudioProcessor& processor, int slotIndex);

    bool isA() const { return index == 0; }

    void rebuildList();            // bundled packs + accumulated user history (shared)
    void syncFromProcessor();      // reflect the loaded IR / trim / refs after a recall
    void pushFiltersToWave();      // push HPF/LPF/trim params + global HEAD onto the wave overlay
    void setActive (bool on);      // mute/empty → dim the wave + the checkbox row
    void selectBundledStartingWith (const juce::String& namePrefix);   // factory-preset helper
    void setSpectrum (const std::vector<float>& pre, const std::vector<float>& post) { wave.setSpectrum (pre, post); }

    // Editor hooks (cross-slot / window-level):
    std::function<void()> onUserIRsChanged;   // chooseIR / clear → editor rebuilds BOTH lists
    std::function<void()> onFirstBLoad;       // B's first IR → editor snaps MIX to centre

    void resized() override;

    // Show/hide this slot's horizontal Dry/Wet slider (driven by the global gear-panel
    // toggle, default off). Re-lays-out so the waveform reclaims the row when hidden.
    void setDryWetVisible (bool shouldShow);

private:
    using BAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SAtt = juce::AudioProcessorValueTreeState::SliderAttachment;

    struct IREntry
    {
        juce::String name, pack;
        bool         bundled  = false;
        const char*  data     = nullptr;
        int          dataSize = 0;
        juce::File   file;
    };

    juce::String sfx() const { return isA() ? "A" : "B"; }   // param-ID suffix

    void selectIR       (int i, bool sendToProcessor = true);
    void selectIRByFile (const juce::File&);
    void stepIR         (int delta);
    void showIRMenu();
    void chooseIR();
    void setHpfFromCurve (bool on, float hz);
    void setLpfFromCurve (bool on, float hz);

    OrbitCabAudioProcessor& proc;
    const int index;             // 0 = A, 1 = B

    juce::TextButton   badge, name { "No IR" }, folder { "Open" }, prev { "<" }, next { ">" };
    WaveformDisplay    wave;
    juce::ToggleButton hpfOn { "HPF" }, lpfOn { "LPF" }, trimOn { "TRIM" },
                       phase { juce::String::fromUTF8 ("\xc3\x98") };
    std::unique_ptr<BAtt> muteAtt, hpfOnAtt, lpfOnAtt, trimOnAtt, phaseAtt;

    // Dry/Wet — a horizontal slider under the checkbox row (hidden unless the gear-panel
    // "Dry/Wet" toggle is on). Bound to the per-slot "mix" param via SliderAttachment.
    juce::Label           dwLabel { {}, "DRY/WET" };
    juce::Slider          dwSlider;
    std::unique_ptr<SAtt> dwAtt;

    std::vector<IREntry> list;
    int                  listIndex = -1;
    juce::File           lastFolder;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotComponent)
};
