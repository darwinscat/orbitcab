// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "core/CabEngine.h"
#include "AppPreferences.h"
#include "PowerampLibrary.h"
#include "PreampLibrary.h"
#include "UpdateChecker.h"
#include "Metadata.h"
#include "StateModel.h"

#include <map>
#include <optional>
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
                                 private juce::Timer
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

    // Preset descriptive identity (preset-centric model). Embedded as a <meta> child of the
    // saved state (v3) and restored on load; a preset browser reads ONLY <meta> for a cheap
    // listing (no DSP, no IR decode). The functional IR load still goes through the <IR>
    // node + the embedded-bytes pool — <meta> is a descriptive *reference*, not the source
    // of truth. currentIrRefs() builds the per-slot refs (id reserved empty until hashing).
    void setPresetName (const juce::String& n)         { currentMeta.name = n; }
    const orbitcab::PresetMeta& presetMeta() const     { return currentMeta; }
    void setPresetMeta (const orbitcab::PresetMeta& m) { currentMeta = m; }

    // Stamp the preset's save timestamps. Called ONLY on an explicit Save / Save As — NOT in
    // buildStateTree — so session serialisation stays byte-deterministic (a host must not see
    // "changed" state on every getStateInformation poll just from a fresh timestamp).
    void markPresetModified();

    // Preset-centric model (no ad-hoc state): the live state IS the current preset, and
    // editing it dirties the preset until Save writes it back. The factory "Default" is the
    // read-only first-start preset — editing it + Save forks (Save As) instead of overwriting.
    //
    // Dirty is a cheap fingerprint of the preset-defining state (params incl. headTrim + IR
    // refs — the same tree snapshots/undo use), so only sound-affecting edits flip it; the
    // baseline is stamped on load/save and persisted in <meta> so a dirty "Default *" survives
    // a DAW session reload. All message-thread (editor / state restore).
    void applyFactoryDefault();                            // first-start / reset: load the bundled default (Roche Limit)
    void loadFactoryPresetState (const void* data, int size);   // load a bundled read-only factory preset (editor Factory section)
    void captureBaseline()        { presetBaselineFingerprint = stateFingerprint(); }
    bool isPresetDirty()          { return presetBaselineFingerprint.isNotEmpty() && stateFingerprint() != presetBaselineFingerprint; }
    void ensureBaselineCaptured() { if (presetBaselineFingerprint.isEmpty()) captureBaseline(); }
    bool isPresetFactory() const  { return presetIsFactory; }
    void setPresetFactory (bool f) { presetIsFactory = f; }

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
    void clearSlotA();                     // empty A (no cab on A → dry passthrough on that slot)

    // POWERAMP (NAM): a neural poweramp stage in front of the cab (input → AMP → cab), gated by
    // the `ampOn` host param. WHICH model runs is a LIBRARY SELECTION (not a host param): the
    // merged factory (BinaryData) + user (powerampDir) library, with the chosen entry's stable id
    // persisted as the "ampSel" state property and loaded off the audio thread (applyPoweramp).
    //
    // The editor hides the whole POWERAMP UI when the library is empty (a public build ships no
    // factory captures and an untouched machine has no user folder yet).
    std::vector<orbitcab::PowerampEntry> powerampLibrary() const;          // factory + user, merged + sorted
    bool         hasAnyPoweramps() const { return ! powerampLibrary().empty(); }
    juce::String selectedPowerampId() const { return apvts.state.getProperty ("ampSel", juce::String()).toString(); }
    void         selectPoweramp (const juce::String& id);                  // set "ampSel" + reload + bump (message thread)
    juce::File   importPoweramp (const juce::File& src);                   // copy a .nam into powerampDir; {} on failure
    bool         removePoweramp (const juce::String& id);                  // delete a USER model (factory: no-op → false)

    // PREAMP (NAM): the SECOND neural stage, run BEFORE the poweramp (input → PREAMP → POWERAMP →
    // cab), gated by `preampOn`. Mirrors the poweramp exactly — a separate merged factory
    // (PreampBinaryData) + user (preampDir) library, the chosen entry's stable id persisted as
    // "preampSel", loaded off the audio thread (applyPreamp). Editor hides the UI when empty.
    std::vector<orbitcab::PreampEntry> preampLibrary() const;              // factory + user, merged + sorted
    bool         hasAnyPreamps() const { return ! preampLibrary().empty(); }
    juce::String selectedPreampId() const { return apvts.state.getProperty ("preampSel", juce::String()).toString(); }
    void         selectPreamp (const juce::String& id);                    // set "preampSel" + reload + bump (message thread)
    juce::File   importPreamp (const juce::File& src);                     // copy a .nam into preampDir; {} on failure
    bool         removePreamp (const juce::String& id);                    // delete a USER model (factory: no-op → false)

    // Does an exported preset ACTUALLY carry embedded audio (vs only bundled refs)? Drives the
    // export heads-up. Keyed on the same pools buildStateTree embeds from, so they never disagree.
    bool         exportEmbedsIR()  const;                                  // live uses an external IR whose bytes are embedded
    bool         exportEmbedsAmp() const;                                  // live uses a poweramp whose .nam is embedded
    bool         exportEmbedsPreamp() const;                               // live uses a preamp whose .nam is embedded

    bool isSlotBLoaded() const { return slotAudioLoaded[1].load (std::memory_order_relaxed); }
    bool isSlotALoaded() const { return slotAudioLoaded[0].load (std::memory_order_relaxed); }

    // TRIM: keep the slot IR's first `fraction` and reload the truncated copy.
    // Called from the editor's waveform drag (message thread); rebuilds + swaps off the
    // audio thread. `fraction` in [0,1]; 1 = full. The only IR-touching control.
    void setTrim (float fraction01, bool slotA = true);

    // HEAD trim — one global, on-by-default session setting (NOT a per-slot host param):
    // snap each IR to its onset, dropping the baked-in cabinet pre-delay so dry/wet and
    // A/B blends stay phase-aligned. Stored as the "headTrim" property on the APVTS state
    // tree (rides session save/load, snapshots, undo). Toggled from the gear settings panel
    // on the message thread; both slots rebuild off the audio thread via the reload poll.
    bool getHeadTrim() const { return (bool) apvts.state.getProperty ("headTrim", true); }
    void setHeadTrim (bool on);

    // Slot IR reference for persistence + editor sync. `bundled` => `ref` is a
    // BinaryData filename; otherwise `ref` is an absolute file path. Empty ref = none.
    // The slot's full IR identity (v4 single source of truth) + thin back-compat wrappers.
    orbitcab::state::SlotIR getSlotIR (bool slotA) const { return slotState[slotA ? 0 : 1]; }   // by value (no dangling ref if state changes)
    juce::String getSlotRef    (bool slotA) const { return getSlotIR (slotA).ref; }
    juce::String getSlotName   (bool slotA) const { return getSlotIR (slotA).displayName; }
    bool         isSlotBundled (bool slotA) const { return getSlotIR (slotA).bundled; }
    float        getTrim       (bool slotA) const { return getSlotIR (slotA).trim; }
    bool         isSlotMissing (bool slotA) const { return getSlotIR (slotA).status == orbitcab::state::SlotIR::Status::missing; }

    // Editor sync — monotonic counters the editor polls on its 30 Hz timer to catch
    // processor-side changes (incl. host-driven setStateInformation) without a push:
    //   soundRev     — any slot IR identity change / load / clear / resolve
    //   workspaceRev — A/B/C/D switch, undo/redo, full-state restore
    //   userIRRev    — the shared recent-IR list changed
    juce::uint32 soundRevision()     const { return soundRev.load     (std::memory_order_relaxed); }
    juce::uint32 workspaceRevision() const { return workspaceRev.load (std::memory_order_relaxed); }
    juce::uint32 userIRRevision()    const { return userIRRev.load    (std::memory_order_relaxed); }

    // Accumulating history of user-opened IR files. Shared by both slots,
    // deduped + capped, persisted in the state so it survives editor close / reload.
    void addUserIR (const juce::File& file);
    void clearUserIRs() { userIRPaths.clear(); persistRecents(); userIRRev.fetch_add (1, std::memory_order_relaxed); }
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

    // The single global PropertiesFile owner (per-machine view-prefs: gear-panel toggles).
    orbitcab::AppPreferences& appPreferences() { return appPreferencesInstance; }

private:
    //==========================================================================
    // The headless DSP core (cab::CabEngine) owns the whole real-time signal path —
    // per-slot HPF/LPF/Convolution/Phase/Dry-Wet, the A<->B crossfade, auto-level,
    // master gain, metering and the spectrum taps. The adapter just packs a cab::Params
    // each block (from the APVTS atomics below) and calls engine.process().
    cab::CabEngine engine;
    cab::Params    packParams() const;            // APVTS atomics -> plain values (RT-safe)

    // appPreferences MUST precede updateCheckerInstance: the checker takes it by reference,
    // so it has to be constructed first (member init follows declaration order).
    orbitcab::AppPreferences appPreferencesInstance;   // single global PropertiesFile owner
    orbitcab::UpdateChecker  updateCheckerInstance;    // version + opt-in update check

    double currentSampleRate = 44100.0;           // IR-load sample-rate fallback (message thread)
    std::atomic<double> irLengthSeconds { 1.0 }; // longest loaded IR tail; getTailLengthSeconds() reads it from the host thread

    // Input trim + bypass parameter pointers (packed into cab::Params each block).
    std::atomic<float>* inputGainParam = nullptr;
    std::atomic<float>* bypassParam    = nullptr;
    std::atomic<float>* ampOnParam     = nullptr;   // NAM poweramp stage master gate / bypass
    std::atomic<float>* preampOnParam  = nullptr;   // NAM preamp stage master gate / bypass

    // Cached raw parameter pointers — RT-safe atomic reads in processBlock. Per-slot
    // params are [0]=A, [1]=B.
    std::atomic<float>* hpfOnP[2]   { nullptr, nullptr };
    std::atomic<float>* hpfFreqP[2] { nullptr, nullptr };
    std::atomic<float>* lpfOnP[2]   { nullptr, nullptr };
    std::atomic<float>* lpfFreqP[2] { nullptr, nullptr };
    std::atomic<float>* phaseP[2]   { nullptr, nullptr };
    std::atomic<float>* mixP[2]     { nullptr, nullptr };
    std::atomic<float>* trimOnP[2]  { nullptr, nullptr };
    std::atomic<float>* muteP[2]    { nullptr, nullptr };
    std::atomic<float>* mixABParam     = nullptr;
    std::atomic<float>* gainParam      = nullptr;
    std::atomic<float>* autoLevelParam = nullptr;

    juce::AudioFormatManager formatManager;
    void loadBundledIR();

    // v4 single source of truth: each slot's full IR identity (status / bundled / ref /
    // displayName / localPath / trim) lives in one struct on the message thread. The audio
    // thread NEVER reads it — it reads the derived `slotAudioLoaded` mirrors (true == ready)
    // in packParams. setSlotAudioMirror keeps the two in sync. See StateModel.h.
    orbitcab::state::SlotIR slotState[2];
    std::atomic<bool>       slotAudioLoaded[2] { { false }, { false } };
    void setSlotAudioMirror (int slotIdx)
    {
        slotAudioLoaded[slotIdx].store (slotState[(size_t) slotIdx].status == orbitcab::state::SlotIR::Status::ready,
                                        std::memory_order_relaxed);
    }
    void applyTrimAndLoad (bool slotA);

    // TRIM/HEAD enable are params that reshape the IR, so toggling them must re-trim — but
    // the change can arrive on the audio thread (host automation). parameterChanged does
    // ONLY an atomic flag store (hard-RT-safe); a 30 Hz message-thread Timer polls the flag
    // and runs the actual reload off the audio thread. enginePrepared gates the poller so it
    // never rebuilds onto an un-prepared / released engine.
    void parameterChanged (const juce::String& id, float newValue) override;
    void timerCallback() override;
    std::atomic<bool> pendingTrimReloadA { false };
    std::atomic<bool> pendingTrimReloadB { false };
    std::atomic<bool> pendingPowerampReload { false };   // ampOn toggled / selection changed → reload the .nam
    std::atomic<bool> pendingPreampReload   { false };   // preampOn toggled / selection changed → reload the .nam
    std::atomic<bool> enginePrepared     { false };

    // Per-model output makeup (dB) applied on top of loudness normalisation — a single flat
    // offset for every poweramp (the measured level match for the tested rigs). Was a per-tube
    // table; now the library is free-form, so it's one constant.
    static constexpr float kPowerampTrimDb = 3.0f;
    // Same idea for the preamp — a single flat output makeup (dB) on top of loudness normalisation.
    static constexpr float kPreampTrimDb = 3.0f;

    // Raw .nam bytes for a library id — factory (BinaryData / PreampBinaryData) or user file, size-
    // capped; {} if the id isn't in the merged library or is oversized. The ONE byte source shared by
    // applyPoweramp/applyPreamp (load the model) AND buildStateTree (materialise the pool at save time,
    // so a select-then-save before the reload poll still embeds the .nam).
    juce::MemoryBlock powerampBytesFor (const juce::String& id) const;
    juce::MemoryBlock preampBytesFor   (const juce::String& id) const;

    // Resolve "ampSel" against the merged library + load that .nam into the amp stage off the
    // audio thread (BinaryData or file read + atomic swap). Off / empty / unresolved => clears it.
    void applyPoweramp();
    // Same for the preamp: resolve "preampSel" + load into the preamp stage (PreampBinaryData /
    // file / embedded pool). Off / empty / unresolved => clears it.
    void applyPreamp();
    // Report the rate-match latency to the host (PDC). 0 unless a NAM stage is on AND resampling
    // (host SR != model 48k) — so it's 0 in the common 48k case. The two stages' latencies sum.
    void updateLatency();

    // One-shot startup fade (#48) — a host-side cosmetic that ramps the output up from
    // silence on the first non-silent block after prepare/release, masking the engine's
    // auto-level + convolver warm-up. Audio-thread only (set in prepare/release, read in
    // processBlock), so plain members suffice.
    bool  softStartArmed = true;
    float softStart      = 1.0f;
    float softStartStep  = 0.0f;

    juce::StringArray userIRPaths;             // recent user-opened IRs (GLOBAL, per-machine — see AppPreferences)
    static constexpr int kMaxUserIRs = 50;
    void persistRecents();                     // write userIRPaths to the global prefs (system-wide, survives new instances)
    void loadRecentsFromPrefs();               // seed userIRPaths from the global prefs on construction
    void loadBundledByName (const juce::String& filename, bool slotA);

    // The v4 state pipeline — ONE capture/apply pair used by sessions, A/B/C/D, undo AND
    // presets, so the three serialisations can never drift (the root cause of the old bugs).
    bool decodeReaderIntoEngine (std::unique_ptr<juce::AudioFormatReader> reader, int slotIdx);  // decode → engine original; no identity
    void resolveSlot           (int slotIdx, orbitcab::state::SlotIR target);   // load `target` into the engine + commit to slotState
    orbitcab::state::SoundState captureLive();                                  // live params + both slots' identity (copyState mutates → non-const)
    void                        applyLive (const orbitcab::state::SoundState&); // restore params + resolve both slots
    orbitcab::state::Workspace  currentWorkspace();                             // live + active + the 4 registers
    void                        applyWorkspace (const orbitcab::state::Workspace&);
    static juce::String computeBlobId (const juce::MemoryBlock& canonicalWav);  // "ir-<hex>" content id of the embedded WAV
    juce::MemoryBlock  canonicalWavForSlot (int slotIdx) const;                 // the embedded 24-bit WAV (original SR) of the loaded original

    // Self-contained sessions: external (user) IRs are embedded in the saved state
    // so a project/preset restores even if the original file is gone. Bytes are kept here
    // (path → WAV blob, capped to kMaxIRSeconds) for every external IR loaded this session
    // and rehydrated from the saved IRPool on restore. Bundled IRs are never embedded.
    std::map<juce::String, juce::MemoryBlock> embeddedIRs;   // keyed by content id "ir-<hex>"

    // Same idea for the poweramp: the raw .nam bytes of every model loaded this session, keyed
    // by its ampSel id ("f:…"/"u:…"). buildStateTree deflates the referenced ones into a
    // <PowerampPool> so a saved session/preset carries the amp (reproducible on any machine /
    // public build); applyPoweramp prefers this pool over the library on restore.
    std::map<juce::String, juce::MemoryBlock> embeddedPoweramps;
    juce::CriticalSection powerampPoolLock;   // guards embeddedPoweramps: applyPoweramp (poll timer) vs
                                              // get/setStateInformation, which some hosts call off the message thread

    // Same again for the preamp stage: raw .nam bytes of every preamp loaded this session, keyed by
    // its preampSel id ("fp:…"/"up:…"), deflated into a <PreampPool> on save (v6).
    std::map<juce::String, juce::MemoryBlock> embeddedPreamps;
    juce::CriticalSection preampPoolLock;     // guards embeddedPreamps (same threads as powerampPoolLock)

    // A/B/C/D compare registers (message-thread only), each a full orbitcab::state::SoundState.
    // Inactive A/B/C/D registers (the active one IS the live state, so it's stored as
    // nullopt — `currentWorkspace`/`applyWorkspace` enforce that invariant). nullopt = a
    // never-used register that inherits the current sound on first switch.
    std::optional<orbitcab::state::SoundState> snapshots[kNumSnapshots];
    int activeSnapshot = 0;

    // Monotonic editor-sync counters (see the public *Revision() getters).
    std::atomic<juce::uint32> soundRev { 0 }, workspaceRev { 0 }, userIRRev { 0 };
    void bumpSound()     { soundRev.fetch_add     (1, std::memory_order_relaxed); }
    void bumpWorkspace() { workspaceRev.fetch_add (1, std::memory_order_relaxed); }

    // Builds the state tree (params + IR refs + embedded IR pool) shared by getStateInformation
    // (session) and getStateForPreset (portable). forPreset = true OMITS the A/B/C/D snapshots
    // and embeds ONLY the live slots' IR audio — so a shared .orbitcab carries just the current
    // sound, never the IRs you loaded into the compare registers (which may be proprietary).
    juce::ValueTree buildStateTree (bool forPreset = false);

    // Preset identity for the live state (preset-centric model). Embedded as the <meta> child
    // by buildStateTree, restored in setStateInformation. currentIrRefs() builds the
    // display-layer refs from the slot state (id left empty — the IR-hashing milestone fills
    // it; the resolve path's id branch stays dormant until then). Message-thread only.
    orbitcab::PresetMeta currentMeta;
    std::vector<orbitcab::IrRef> currentIrRefs() const;

    // Preset-centric tracking (message-thread). presetIsFactory => the current preset is the
    // read-only factory Default (Save forks). presetBaselineFingerprint = the hash of the
    // preset content at the last load/save; isPresetDirty() compares the live fingerprint.
    bool         presetIsFactory = true;
    juce::String presetBaselineFingerprint;
    juce::String stateFingerprint();           // hash of captureStateTree() — cheap dirty probe
    void         loadFactoryDefaultIR();        // bundled IR #16 into slot A (the factory Default cab)

    // Undo/redo state (message-thread only). Snapshots are ref-only (no embedded bytes —
    // those live in embeddedIRs for the instance), so the stacks stay tiny.
    std::vector<juce::ValueTree> undoStack, redoStack;
    juce::ValueTree undoBaseline, undoPrev;
    int undoSettle = 0;
    static constexpr int kMaxUndo        = 64;
    static constexpr int kUndoSettleTicks = 12;   // ~0.4 s of no change @ 30 Hz → commit

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrbitCabAudioProcessor)
};
