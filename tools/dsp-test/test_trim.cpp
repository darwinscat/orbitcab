// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Offline DSP check: does TRIM actually shorten the convolved IR tail?
// Renders an impulse through the real OrbitCabAudioProcessor at full trim vs 25% trim and
// measures where the output decays to silence. If trim works, the 25% tail is much shorter.
#include "../../src/PluginProcessor.h"
#include "../../src/IRLibrary.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <cstdio>
#include <cmath>

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;          // MessageManager for the AsyncUpdater
    OrbitCabAudioProcessor proc;

    const double sr = 48000.0;
    const int    block = 512;
    proc.prepareToPlay (sr, block);

    // Accumulated pass/fail so this harness can be a real CI gate: any BROKEN result
    // flips it and the process exits non-zero (it used to always return 0).
    bool allPass = true;

    auto& apvts = proc.apvts;
    auto setP = [&] (const char* id, float norm) { if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (norm); };
    setP ("autoLevel", 0.0f);   // off — don't let level-matching mask the change
    setP ("bypass",    0.0f);
    setP ("mixA",      1.0f);   // 100% wet (normalised 1.0 over 0..100)
    setP ("trimOnA",   1.0f);   // enable trim on slot A

    auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
    pump (200);

    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;

    auto tailMs = [&] (float trim) -> double
    {
        proc.setTrim (trim, true);
        pump (500);                                // async reload + background engine build
        for (int b = 0; b < 12; ++b) { buf.clear(); proc.processBlock (buf, midi); }  // settle crossfade

        double lastMs = 0.0;
        int    total  = 0;
        const int blocks = (int) (2.0 * sr / block);
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            if (b == 0) { buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f); }   // impulse
            proc.processBlock (buf, midi);
            for (int i = 0; i < block; ++i)
                if (std::abs (buf.getSample (0, i)) > 1.0e-4f)
                    lastMs = (total + i) * 1000.0 / sr;
            total += block;
        }
        return lastMs;
    };

    const double full    = tailMs (1.0f);
    const double trimmed = tailMs (0.25f);
    const bool trimOk = full > 1.0 && trimmed < full * 0.6;
    allPass &= trimOk;
    std::printf ("TRIM TEST: full tail ~%.1f ms, trimmed(0.25) tail ~%.1f ms\n", full, trimmed);
    std::printf ("RESULT: %s\n", trimOk ? "TRIM WORKS (tail shortened)"
                                        : "TRIM BROKEN (tail unchanged)");

    // ---- HEAD trim: leading silence is removed when HEAD is enabled ----
    // Build a synthetic IR: 50 ms of silence, then a 100 ms decaying tone (onset is crisp
    // because cos starts at its peak). Convolving an impulse with it reproduces the IR, so
    // the output onset == the IR onset; HEAD on should pull that onset ~50 ms earlier.
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("orbitcab_head_test.wav");
    {
        const int leadN = (int) (0.050 * sr);
        const int bodyN = (int) (0.100 * sr);
        juce::AudioBuffer<float> irbuf (1, leadN + bodyN);
        irbuf.clear();
        for (int i = 0; i < bodyN; ++i)
        {
            const float t = (float) i / (float) sr;
            irbuf.setSample (0, leadN + i,
                             std::cos (juce::MathConstants<float>::twoPi * 800.0f * t) * std::exp (-t * 30.0f));
        }
        tmp.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os = tmp.createOutputStream();
        if (os != nullptr)
            if (auto writer = wav.createWriterFor (os, juce::AudioFormatWriter::Options{}
                                                           .withSampleRate (sr)
                                                           .withNumChannels (1)
                                                           .withBitsPerSample (24)))
                writer->writeFromAudioSampleBuffer (irbuf, 0, irbuf.getNumSamples());
    }
    proc.loadIRFromFile (tmp, true);
    pump (300);

    auto onsetMs = [&] (bool head) -> double
    {
        setP ("trimOnA", 0.0f);
        proc.setHeadTrim (head);                   // global session setting (no longer a param)
        pump (500);
        for (int b = 0; b < 12; ++b) { buf.clear(); proc.processBlock (buf, midi); }   // settle

        int total = 0;
        const int blocks = (int) (1.0 * sr / block);
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            if (b == 0) { buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f); }   // impulse
            proc.processBlock (buf, midi);
            for (int i = 0; i < block; ++i)
                if (std::abs (buf.getSample (0, i)) > 1.0e-3f)
                    return (total + i) * 1000.0 / sr;
            total += block;
        }
        return -1.0;
    };

    const double onOff = onsetMs (false);
    const double onOn  = onsetMs (true);
    tmp.deleteFile();
    const bool headOk = onOff > 30.0 && onOn >= 0.0 && onOn < onOff - 30.0;
    allPass &= headOk;
    std::printf ("HEAD TEST: onset full ~%.1f ms, head-trimmed ~%.1f ms\n", onOff, onOn);
    std::printf ("RESULT: %s\n", headOk ? "HEAD WORKS (leading silence removed)"
                                        : "HEAD BROKEN (onset unchanged)");

    // ---- A/B/C/D snapshots: live-register independence ----
    // Register A holds value X; switch to B (inherits A), change to Y; switch back to A
    // (must still be X); switch to B (must be Y). Proves switching captures + recalls.
    auto getGain = [&] { return proc.apvts.getRawParameterValue ("gain")->load(); };
    setP ("gain", 0.20f); pump (60);
    const float aVal = getGain();                 // register A's value
    proc.switchToSnapshot (1); pump (60);          // -> B (fresh, inherits A)
    setP ("gain", 0.90f); pump (60);
    const float bVal = getGain();                 // register B's value
    proc.switchToSnapshot (0); pump (60);          // -> A: must be unchanged
    const float aBack = getGain();
    proc.switchToSnapshot (1); pump (60);          // -> B: must be the change
    const float bBack = getGain();
    const bool snapOk = std::abs (aBack - aVal) < 1.0e-2f
                      && std::abs (bBack - bVal) < 1.0e-2f
                      && std::abs (aVal - bVal) > 0.1f;
    allPass &= snapOk;
    std::printf ("SNAPSHOT TEST: A=%.2f (recall %.2f), B=%.2f (recall %.2f)\n", aVal, aBack, bVal, bBack);
    std::printf ("RESULT: %s\n", snapOk ? "SNAPSHOTS WORK (registers independent)"
                                        : "SNAPSHOTS BROKEN");

    // ---- an external IR embedded in the state survives the file being deleted ----
    {
        auto extf = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("orbitcab_embed_test.wav");
        {
            const int en = (int) (0.08 * sr);
            juce::AudioBuffer<float> eb (1, en);
            for (int i = 0; i < en; ++i)
            {
                const float t = (float) i / (float) sr;
                eb.setSample (0, i, std::cos (juce::MathConstants<float>::twoPi * 1000.0f * t) * std::exp (-t * 40.0f));
            }
            extf.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os = extf.createOutputStream();
            if (os != nullptr)
                if (auto w = wav.createWriterFor (os, juce::AudioFormatWriter::Options{}
                                                          .withSampleRate (sr).withNumChannels (1).withBitsPerSample (24)))
                    w->writeFromAudioSampleBuffer (eb, 0, en);
        }

        juce::MemoryBlock state;
        { OrbitCabAudioProcessor pSave; pSave.prepareToPlay (sr, block); pSave.loadIRFromFile (extf, true);
          pump (200); pSave.getStateInformation (state); }
        extf.deleteFile();                                   // the original file is now GONE

        OrbitCabAudioProcessor pLoad; pLoad.prepareToPlay (sr, block);
        pLoad.setStateInformation (state.getData(), (int) state.getSize());
        pump (300);
        auto sp = [&] (const char* id, float v) { if (auto* q = pLoad.apvts.getParameter (id)) q->setValueNotifyingHost (v); };
        sp ("autoLevel", 0.0f); sp ("bypass", 0.0f); sp ("mixA", 1.0f); sp ("mixAB", 0.0f);
        pump (150);
        for (int b = 0; b < 12; ++b) { buf.clear(); pLoad.processBlock (buf, midi); }

        double tail = 0.0; int total = 0; const int blocks = (int) (0.5 * sr / block);
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            if (b == 0) { buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f); }
            pLoad.processBlock (buf, midi);
            for (int i = 0; i < block; ++i)
                if (std::abs (buf.getSample (0, i)) > 1.0e-3f) tail = (total + i) * 1000.0 / sr;
            total += block;
        }
        const bool embedOk = tail > 5.0;
        allPass &= embedOk;
        std::printf ("EMBED TEST: restored tail ~%.1f ms (original file deleted before restore)\n", tail);
        std::printf ("RESULT: %s\n", embedOk ? "EMBED WORKS (IR restored from the session, no file)"
                                             : "EMBED BROKEN");
    }

    // ---- undo / redo: a settled edit is one step, undo restores it, redo replays it ----
    {
        OrbitCabAudioProcessor pu; pu.prepareToPlay (sr, block);
        auto setG  = [&] (float nrm) { if (auto* q = pu.apvts.getParameter ("gain")) q->setValueNotifyingHost (nrm); };
        auto getG  = [&] { return pu.apvts.getRawParameterValue ("gain")->load(); };
        auto ticks = [&] (int n) { for (int i = 0; i < n; ++i) pu.undoTick(); };   // simulate the editor timer

        setG (0.30f); ticks (20);                       // baseline X settles
        const float x = getG();
        setG (0.80f); ticks (20);                       // X→Y commits one undo step
        const float y = getG();
        const bool didUndo = pu.undo();   const float afterUndo = getG();
        const bool didRedo = pu.redo();   const float afterRedo = getG();
        const bool ok = didUndo && didRedo && std::abs (x - y) > 0.1f
                      && std::abs (afterUndo - x) < 1.0e-2f && std::abs (afterRedo - y) < 1.0e-2f;
        allPass &= ok;
        std::printf ("UNDO TEST: X=%.2f Y=%.2f, undo→%.2f redo→%.2f\n", x, y, afterUndo, afterRedo);
        std::printf ("RESULT: %s\n", ok ? "UNDO/REDO WORKS" : "UNDO/REDO BROKEN");
    }

    // ---- state: params + IR refs + trim round-trip into a FRESH instance ----
    // The golden test for the (frozen) state format: capture → restore must preserve every
    // parameter, the per-slot IR reference, and the trim position.
    {
        OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
        auto sa = [&] (const char* id, float v) { if (auto* q = a.apvts.getParameter (id)) q->setValueNotifyingHost (v); };
        sa ("gain", 0.70f); sa ("hpfOnA", 1.0f); sa ("autoLevel", 0.0f);
        a.setTrim (0.40f, true);
        a.setPresetName ("Crunch Test");                 // preset <meta> name (v3) must ride the state
        pump (200);
        juce::MemoryBlock state; a.getStateInformation (state);

        OrbitCabAudioProcessor b; b.prepareToPlay (sr, block);
        b.setStateInformation (state.getData(), (int) state.getSize());
        pump (200);

        auto raw = [] (OrbitCabAudioProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        const bool gainOk = std::abs (raw (b, "gain") - raw (a, "gain")) < 1.0e-3f;
        const bool hpfOk  = raw (b, "hpfOnA") > 0.5f;
        const bool trimOk = std::abs (b.getTrim (true) - 0.40f) < 0.05f;
        const bool refOk  = b.getSlotRef (true) == a.getSlotRef (true) && b.isSlotBundled (true);

        // <meta> (v3): the descriptive name + the per-slot IR refs (display-layer) round-trip,
        // with the reserved id still empty and the fallback mirroring the functional ref.
        const auto& refs = b.presetMeta().irRefs;
        const bool metaNameOk = b.presetMeta().name == "Crunch Test";
        const bool metaRefOk  = ! refs.empty() && refs[0].slot == "A" && refs[0].bundled
                                && refs[0].id.isEmpty() && refs[0].fallback == a.getSlotRef (true);

        const bool stOk = gainOk && hpfOk && trimOk && refOk && metaNameOk && metaRefOk;
        allPass &= stOk;
        std::printf ("STATE TEST: gain=%d hpf=%d trim=%d ref=%d meta(name=%d ref=%d) (refA=\"%s\")\n",
                     gainOk, hpfOk, trimOk, refOk, metaNameOk, metaRefOk, b.getSlotRef (true).toRawUTF8());
        std::printf ("RESULT: %s\n", stOk ? "STATE ROUND-TRIP WORKS (params+refs+trim+meta)"
                                          : "STATE ROUND-TRIP BROKEN");
    }

    // ---- headTrim is a state property (NOT a host param): rides save/load + undo/redo ----
    // Default is ON. Flip OFF, prove it round-trips into a fresh instance, and that undo
    // restores it — captureStateTree() snapshots apvts.copyState(), which carries the
    // property, so both the snapshot-based undo and the serialized session pick it up.
    {
        OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
        a.setHeadTrim (false);                          // non-default (default is on)
        pump (60);
        juce::MemoryBlock state; a.getStateInformation (state);

        OrbitCabAudioProcessor b; b.prepareToPlay (sr, block);   // fresh → headTrim defaults ON
        b.setStateInformation (state.getData(), (int) state.getSize());
        pump (60);
        const bool saveLoadOk = ! b.getHeadTrim();      // OFF rode the state into a fresh instance

        OrbitCabAudioProcessor u; u.prepareToPlay (sr, block);
        auto ticks = [&] (int n) { for (int i = 0; i < n; ++i) u.undoTick(); };
        ticks (20);                                     // baseline (headTrim on) settles
        u.setHeadTrim (false);                          // on→off commits one undo step
        ticks (20);
        const bool didUndo = u.undo();   const bool undoOk = didUndo && u.getHeadTrim();     // → on
        const bool didRedo = u.redo();   const bool redoOk = didRedo && ! u.getHeadTrim();   // → off

        const bool htOk = saveLoadOk && undoOk && redoOk;
        allPass &= htOk;
        std::printf ("HEADTRIM STATE TEST: save/load=%d undo=%d redo=%d\n", saveLoadOk, undoOk, redoOk);
        std::printf ("RESULT: %s\n", htOk ? "HEADTRIM RIDES STATE (save/load + undo/redo)"
                                          : "HEADTRIM STATE BROKEN");
    }

    // ---- migration: a pre-1.1 session (no headTrim property) loads with HEAD off ----
    // HEAD used to be per-slot params (default off); now it's the global `headTrim` property
    // (default on). Stripping the property mimics a 1.0.x save — setStateInformation must fall
    // back to off (the old default), while a normal save keeps whatever was stored.
    {
        OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
        a.apvts.state.removeProperty ("headTrim", nullptr);   // simulate a 1.0.x save (never had it)
        juce::MemoryBlock oldState; a.getStateInformation (oldState);

        OrbitCabAudioProcessor b; b.prepareToPlay (sr, block);   // fresh defaults headTrim ON
        b.setStateInformation (oldState.getData(), (int) oldState.getSize());
        pump (60);
        const bool migOk = ! b.getHeadTrim();                 // absent on load → migrated to OFF

        OrbitCabAudioProcessor c; c.prepareToPlay (sr, block); c.setHeadTrim (true);
        juce::MemoryBlock newState; c.getStateInformation (newState);
        OrbitCabAudioProcessor d; d.prepareToPlay (sr, block);
        d.setStateInformation (newState.getData(), (int) newState.getSize());
        pump (60);
        const bool keepOk = d.getHeadTrim();                  // present → preserved ON

        const bool migrateOk = migOk && keepOk;
        allPass &= migrateOk;
        std::printf ("MIGRATION TEST: old(no headTrim)->off=%d  new(on)->on=%d\n", migOk, keepOk);
        std::printf ("RESULT: %s\n", migrateOk ? "HEADTRIM MIGRATION WORKS (absent -> off, present -> kept)"
                                               : "HEADTRIM MIGRATION BROKEN");
    }

    // ---- preset-centric model: factory Default + dirty tracking + persistence ----
    // First start IS the read-only factory "Default" (IR #16 + HPF), clean. Editing dirties
    // it; re-baselining (== Save) cleans it; and both the dirty state and the factory flag
    // ride a DAW session save/load (so a dirty "Default *" survives a reload).
    {
        OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
        const bool defName    = a.presetMeta().name == "Default";
        const bool defFactory = a.isPresetFactory();
        const bool defIR      = a.getSlotRef (true).startsWith ("16") && a.isSlotBundled (true);
        const bool cleanStart = ! a.isPresetDirty();

        if (auto* q = a.apvts.getParameter ("gain")) q->setValueNotifyingHost (0.8f);   // edit a sound param
        const bool dirtyAfterEdit = a.isPresetDirty();

        a.captureBaseline();                                  // == Save: re-baseline → clean
        const bool cleanAfterSave = ! a.isPresetDirty();

        // Edit again, then a session round-trip must preserve the dirty + the Default identity.
        if (auto* q = a.apvts.getParameter ("gain")) q->setValueNotifyingHost (0.3f);
        juce::MemoryBlock st; a.getStateInformation (st);
        OrbitCabAudioProcessor b; b.prepareToPlay (sr, block);
        b.setStateInformation (st.getData(), (int) st.getSize());
        pump (60);
        const bool dirtyRidesState   = b.isPresetDirty();
        const bool factoryRidesState = b.isPresetFactory() && b.presetMeta().name == "Default";

        // A clean Default round-trips as clean (fingerprint is stable across save/load).
        OrbitCabAudioProcessor c; c.prepareToPlay (sr, block);
        juce::MemoryBlock cst; c.getStateInformation (cst);
        OrbitCabAudioProcessor d; d.prepareToPlay (sr, block);
        d.setStateInformation (cst.getData(), (int) cst.getSize());
        pump (60);
        const bool cleanRidesState = ! d.isPresetDirty();

        b.setHeadTrim (false);                               // a non-param state property...
        b.applyFactoryDefault();                              // ...that applyFactoryDefault must reset to factory (on)
        const bool resetClean = ! b.isPresetDirty() && b.isPresetFactory() && b.getHeadTrim();

        // Determinism: serialising twice with no change yields identical bytes (no modifiedAt
        // churn), so a host doesn't flag the session "modified" on every getStateInformation poll.
        OrbitCabAudioProcessor e; e.prepareToPlay (sr, block);
        juce::MemoryBlock s1, s2; e.getStateInformation (s1); e.getStateInformation (s2);
        const bool deterministic = (s1 == s2);

        const bool pcOk = defName && defFactory && defIR && cleanStart && dirtyAfterEdit
                          && cleanAfterSave && dirtyRidesState && factoryRidesState
                          && cleanRidesState && resetClean && deterministic;
        allPass &= pcOk;
        std::printf ("PRESET TEST: default(name=%d factory=%d ir=%d clean=%d) dirtyEdit=%d cleanSave=%d ride(dirty=%d factory=%d cleanRT=%d) reset=%d det=%d\n",
                     defName, defFactory, defIR, cleanStart, dirtyAfterEdit, cleanAfterSave,
                     dirtyRidesState, factoryRidesState, cleanRidesState, resetClean, deterministic);
        std::printf ("RESULT: %s\n", pcOk ? "PRESET-CENTRIC WORKS (factory Default + dirty + rides state)"
                                          : "PRESET-CENTRIC BROKEN");
    }

    // ---- IRLibrary: the shared bundled-IR enumeration (de-dup) ----
    {
        const auto bundled = orbitcab::bundledIRs();
        const bool countOk  = bundled.size() == 21;                          // Brutal 15 + Emerald 6
        const bool sortedOk = ! bundled.empty()
                              && bundled.front().name.startsWith ("01-")
                              && bundled.back().name.startsWith ("21-");      // natural sort, not lexical
        bool packOk = true;
        for (const auto& b : bundled)
        {
            const int n = b.name.getIntValue();
            const juce::String expect = (n >= 1 && n <= 15) ? "Brutal" : "Emerald";
            packOk = packOk && (b.pack == expect);
        }
        const bool libOk = countOk && sortedOk && packOk;
        allPass &= libOk;
        std::printf ("LIBRARY TEST: count=%d sorted=%d pack=%d (%d bundled IRs)\n",
                     countOk, sortedOk, packOk, (int) bundled.size());
        std::printf ("RESULT: %s\n", libOk ? "IR LIBRARY WORKS (21, sorted, pack-split)"
                                           : "IR LIBRARY BROKEN");
    }

    std::printf ("\n==== %s ====\n", allPass ? "ALL DSP CHECKS PASSED" : "SOME DSP CHECKS FAILED");
    return allPass ? 0 : 1;
}
