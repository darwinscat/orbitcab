// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "core/CabEngine.h"
#include "UpdateChecker.h"

#include <map>
#include <vector>

//==============================================================================
// OrbitCab — the AudioProcessor (audio thread + state owner).
//
// APVTS parameters + the live chain
//   per slot (A/B):  in → HPF → LPF → Convolution → Phase → Dry/Wet(blend raw input)
//   then:            MIX (mixAB) crossfades the two slot outputs → Auto-level → Output Gain → out
// HPF/LPF/Mix/Gain/Phase are live & smooth (process the signal, not the IR).
// Only the IR itself (TRIM / IR-switch) reloads. See docs/IR-LOADER-DESIGN.md.
//
// 🔴 Real-time rule: processBlock and anything it calls must not allocate,
// lock, do IO, or throw. Preallocate in prepareToPlay.
//==============================================================================
class OrbitCabAudioProcessor final : public juce::AudioProcessor,
                                 private juce::AudioProcessorValueTreeState::Listener,
                                 private juce::AsyncUpdater
{
public:
    OrbitCabAudioProcessor();
    ~OrbitCabAudioProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    // Report the loaded IR length so hosts don't clip the cab tail on bounce.
    double getTailLengthSeconds() const override { return irLengthSeconds; }

    //==========================================================================
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    // Versioned state from day one (ARCHITECTURE). Full migration + IR
    // reference comes later; here we persist the APVTS tree stamped with a version.
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Portable preset export: like getStateInformation, but external-IR file paths are
    // stripped (content-hash keys + a display name, no recent-files list) so a shared
    // .orbitcab doesn't leak the sharer's folders. The session state above keeps the paths.
    void getStateForPreset (juce::MemoryBlock& destData);

    //==========================================================================
    // Parameter tree — public so the editor can attach controls. The layout is
    // built by orbitcab::createParameterLayout() in Parameters.cpp (one source of truth).
    juce::AudioProcessorValueTreeState apvts;

    // Per-block input/output peaks (linear) for the editor's level meters — owned by
    // the engine (written on the audio thread, read on the message thread, atomic).
    float getInputLevel()  const { return engine.inputLevel();  }
    float getOutputLevel() const { return engine.outputLevel(); }

    // Spectrum capture (for the editor's pre/post analyser). The audio thread fills a
    // fixed window in the engine; the editor pulls the latest ready frame on its timer
    // and runs the FFT itself. The FFT-size contract lives in the core (cab::SpectrumTap);
    // these aliases keep the editor's references stable. `pre` = dry input, else = output.
    static constexpr int fftOrder = cab::CabEngine::fftOrder;   // 2048-point
    static constexpr int fftSize  = cab::CabEngine::fftSize;
    bool pullSpectrumFrame (bool pre, float* destFftSize) { return engine.pullSpectrum (pre, destFftSize); }

    // The editor tells the engine when it actually wants the analyser fed (editor open
    // AND the "≈" toggle on); processBlock also skips capture during an offline render.
    void setSpectrumActive (bool shouldFeed) { engine.setSpectrumActive (shouldFeed); }

    // Load an IR into a slot. Decoded on the calling (message) thread;
    // the convolver swaps it on JUCE's loader thread — RT-safe. Two sources per
    // slot: a user-chosen file, and a bundled IR (embedded bytes via BinaryData). The
    // `ref` of a bundled load is its filename, so the slot can be persisted.
    void loadIRFromFile   (const juce::File& file, bool slotA = true);
    void loadIRFromMemory (const void* data, size_t sizeInBytes, bool slotA = true,
                           const juce::String& bundledName = {});
    void clearSlotB();                     // unload B (back to single-IR / full A)
    bool isSlotBLoaded() const { return slotBLoaded.load (std::memory_order_relaxed); }

    // TRIM: keep the slot IR's first `fraction` and reload the truncated copy.
    // Called from the editor's waveform drag (message thread); rebuilds + swaps off the
    // audio thread. `fraction` in [0,1]; 1 = full. The only IR-touching control.
    void setTrim (float fraction01, bool slotA = true);

    // Slot IR reference for persistence + editor sync. `bundled` => `ref` is a
    // BinaryData filename; otherwise `ref` is an absolute file path. Empty ref = none.
    juce::String getSlotRef    (bool slotA) const { return slotA ? slotRefA : slotRefB; }
    juce::String getSlotName   (bool slotA) const { return slotA ? slotNameA : slotNameB; }   // display override (a portable preset sets it; else empty)
    bool         isSlotBundled (bool slotA) const { return slotA ? slotBundledA : slotBundledB; }
    float        getTrim       (bool slotA) const { return slotA ? trimFractionA : trimFractionB; }

    // Accumulating history of user-opened IR files. Shared by both slots,
    // deduped + capped, persisted in the state so it survives editor close / reload.
    void addUserIR (const juce::File& file);
    void clearUserIRs() { userIRPaths.clear(); }
    const juce::StringArray& getUserIRPaths() const { return userIRPaths; }

    // A/B/C/D compare — 4 snapshot registers, each a full settings snapshot (both IR
    // slots + every parameter). Live-register model: the active register always mirrors
    // the live state, so switching captures the current settings into the active
    // register and recalls the target; a never-used register starts as a copy of the
    // current state. Lives here (not the editor) so it survives the window closing and
    // is saved in the DAW session. Called on the message thread (button click).
    static constexpr int kNumSnapshots = 4;
    void switchToSnapshot (int index);
    int  getActiveSnapshot() const { return activeSnapshot; }

    // Undo / redo — a stack of full ref-only state snapshots (reuses capture/applyStateTree).
    // The editor pumps undoTick() on its timer; a burst of edits coalesces into one step
    // once the state settles. undo()/redo() apply a step (the editor then re-syncs its UI).
    void undoTick();
    bool undo();
    bool redo();
    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }

    // Embedded IR bytes for a path, if this session carries them — lets the editor
    // draw the waveform from the embedded copy when the original file is gone.
    const juce::MemoryBlock* embeddedIRBytes (const juce::String& path) const;

    // Opt-in update check. Owned here so it survives the editor window closing and
    // is one per plugin instance; the editor's version badge drives + reads it.
    orbitcab::UpdateChecker& updateChecker() { return updateCheckerInstance; }

private:
    //==========================================================================
    // The headless DSP core (cab::CabEngine) owns the whole real-time signal path —
    // per-slot HPF/LPF/Convolution/Phase/Dry-Wet, the A<->B crossfade, auto-level,
    // master gain, metering and the spectrum taps. The adapter just packs a cab::Params
    // each block (from the APVTS atomics below) and calls engine.process().
    cab::CabEngine engine;
    cab::Params    packParams() const;            // APVTS atomics -> plain values (RT-safe)

    orbitcab::UpdateChecker updateCheckerInstance;   // version + opt-in update check

    double currentSampleRate = 44100.0;           // IR-load sample-rate fallback (message thread)
    std::atomic<double> irLengthSeconds { 1.0 }; // longest loaded IR tail; getTailLengthSeconds() reads it from the host thread

    // Input trim + bypass parameter pointers (packed into cab::Params each block).
    std::atomic<float>* inputGainParam = nullptr;
    std::atomic<float>* bypassParam    = nullptr;

    // Cached raw parameter pointers — RT-safe atomic reads in processBlock. Per-slot
    // params are [0]=A, [1]=B.
    std::atomic<float>* hpfOnP[2]   { nullptr, nullptr };
    std::atomic<float>* hpfFreqP[2] { nullptr, nullptr };
    std::atomic<float>* lpfOnP[2]   { nullptr, nullptr };
    std::atomic<float>* lpfFreqP[2] { nullptr, nullptr };
    std::atomic<float>* phaseP[2]   { nullptr, nullptr };
    std::atomic<float>* mixP[2]     { nullptr, nullptr };
    std::atomic<float>* trimOnP[2]  { nullptr, nullptr };
    std::atomic<float>* headOnP[2]  { nullptr, nullptr };   // trim leading silence
    std::atomic<float>* muteP[2]    { nullptr, nullptr };
    std::atomic<float>* mixABParam     = nullptr;
    std::atomic<float>* gainParam      = nullptr;
    std::atomic<float>* autoLevelParam = nullptr;

    juce::AudioFormatManager formatManager;
    void loadBundledIR();
    bool loadIRFromReader (std::unique_ptr<juce::AudioFormatReader> reader, bool slotA);   // true on a successful decode

    // The decoded IR + TRIM/HEAD math now live in the core's cab::IRSlot (one per slot);
    // the adapter keeps only the persisted drag position + the loaded flag. Message-thread.
    float  trimFractionA = 1.0f, trimFractionB = 1.0f;
    std::atomic<bool> slotBLoaded { false };
    void applyTrimAndLoad (bool slotA);

    // TRIM enable is a param that changes the IR length, so toggling it must re-trim —
    // but the change can arrive on the audio thread (host automation). Defer the reload
    // to the message thread via AsyncUpdater (RT-safe).
    void parameterChanged (const juce::String& id, float newValue) override;
    void handleAsyncUpdate() override;
    std::atomic<bool> pendingTrimReloadA { false };
    std::atomic<bool> pendingTrimReloadB { false };

    // IR reference per slot (for state/preset save). Message-thread only.
    juce::String slotRefA, slotRefB;
    juce::String slotNameA, slotNameB;          // display name (a portable preset carries one; empty = derive from ref)
    bool slotBundledA = false, slotBundledB = false;
    juce::StringArray userIRPaths;             // recent user-opened IRs
    static constexpr int kMaxUserIRs = 50;
    void loadBundledByName (const juce::String& filename, bool slotA);
    void restoreIRsFromTree (const juce::ValueTree& ir);

    // Load a slot from a stored reference: bundled name → embedded bytes (this session's
    // pool) → file on disk. Used by state/snapshot recall so external IRs survive a moved
    // file / another machine when their bytes were embedded.
    void loadIRRef (const juce::String& ref, bool bundled, bool slotA);

    // Self-contained sessions: external (user) IRs are embedded in the saved state
    // so a project/preset restores even if the original file is gone. Bytes are kept here
    // (path → WAV blob, capped to kMaxIRSeconds) for every external IR loaded this session
    // and rehydrated from the saved IRPool on restore. Bundled IRs are never embedded.
    std::map<juce::String, juce::MemoryBlock> embeddedIRs;
    void cacheEmbeddedIR (const juce::String& path, bool slotA);

    // A/B/C/D snapshots (message-thread only). Each register is a full settings tree
    // (params + IR refs, no user-IR history). captureStateTree snapshots the live state;
    // applyStateTree recalls one. Persisted in get/setStateInformation.
    juce::ValueTree snapshots[kNumSnapshots];
    int activeSnapshot = 0;
    juce::ValueTree captureStateTree();
    void applyStateTree (const juce::ValueTree& tree);

    // Builds the full state tree (params + IR refs + snapshots + embedded IR pool) shared by
    // getStateInformation (session, keeps paths) and getStateForPreset (portable, paths stripped).
    juce::ValueTree buildStateTree();

    // Undo/redo state (message-thread only). Snapshots are ref-only (no embedded bytes —
    // those live in embeddedIRs for the instance), so the stacks stay tiny.
    std::vector<juce::ValueTree> undoStack, redoStack;
    juce::ValueTree undoBaseline, undoPrev;
    int undoSettle = 0;
    static constexpr int kMaxUndo        = 64;
    static constexpr int kUndoSettleTicks = 12;   // ~0.4 s of no change @ 30 Hz → commit

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrbitCabAudioProcessor)
};
