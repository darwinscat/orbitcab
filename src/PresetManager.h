// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "PluginProcessor.h"
#include <juce_core/juce_core.h>

//==============================================================================
// orbitcab::PresetManager — preset file I/O. A preset is the processor's
// state blob (params + IR references + embedded external IR audio) written to
// ~/…/Darwin's Cat/OrbitCab/Presets/*.orbitcab. Pure file + state logic; the dialogs
// (name prompt, file chooser, drag-drop) and the UI re-sync stay in the editor. Adapter
// layer (host glue) — the cab:: DSP core is untouched.
//==============================================================================
namespace orbitcab
{

class PresetManager
{
public:
    explicit PresetManager (OrbitCabAudioProcessor& processor) : proc (processor) {}

    static juce::File directory();              // …/Darwin's Cat/OrbitCab/Presets
    juce::Array<juce::File> list() const;       // *.orbitcab in the preset dir, natural-sorted

    // A preset file paired with its <meta> (name / author / description / tags / irRefs),
    // read WITHOUT applying state — i.e. without touching the DSP or decoding the embedded
    // IR audio. The cheap source a preset browser renders from. Files with no <meta>
    // (pre-v3 presets) fall back to the filename for the name.
    struct PresetEntry { juce::File file; orbitcab::PresetMeta meta; };
    juce::Array<PresetEntry> listWithMeta() const;

    juce::File saveAs   (const juce::String& name);   // current state → dir/<name>.orbitcab; returns the file
    bool       loadFrom (const juce::File& file);     // file → setStateInformation; false if unreadable
    bool       writeTo  (juce::File file);            // current state → file (forces .orbitcab); embeds IRs
    bool       deleteFile (const juce::File& file);   // move a user preset to the Trash; guarded to the preset dir

private:
    OrbitCabAudioProcessor& proc;
};

} // namespace orbitcab
