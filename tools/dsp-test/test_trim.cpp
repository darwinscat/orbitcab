// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Offline DSP check: does TRIM actually shorten the convolved IR tail?
// Renders an impulse through the real OrbitCabAudioProcessor at full trim vs 25% trim and
// measures where the output decays to silence. If trim works, the 25% tail is much shorter.
#include "../../src/PluginProcessor.h"
#include "../../src/IRLibrary.h"
#include "../../src/FactoryPresets.h"   // kDefaultPresetName (the bundled first-start default)
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

    // ---- preset-centric model: factory default + dirty tracking + persistence ----
    // First start IS the read-only bundled default preset (Roche Limit), clean. Editing dirties
    // it; re-baselining (== Save) cleans it; and both the dirty state and the factory flag
    // ride a DAW session save/load (so a dirty default "*" survives a reload).
    {
        OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
        const bool defName    = a.presetMeta().name == orbitcab::kDefaultPresetName;   // "Roche Limit"
        const bool defFactory = a.isPresetFactory();
        const bool defIR      = a.isSlotALoaded() && a.isSlotBundled (true);            // default's box I = a bundled IR
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
        const bool factoryRidesState = b.isPresetFactory() && b.presetMeta().name == orbitcab::kDefaultPresetName;

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

    // ---- clear slot: emptying A (no cab) rides the state; a fresh instance still loads ----
    {
        OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
        const bool startLoaded = a.isSlotALoaded();          // default: A holds the factory cab
        a.clearSlotA();
        const bool clearedA = ! a.isSlotALoaded();
        juce::MemoryBlock st; a.getStateInformation (st);
        OrbitCabAudioProcessor b; b.prepareToPlay (sr, block);
        b.setStateInformation (st.getData(), (int) st.getSize());
        pump (60);
        const bool ridesEmpty = ! b.isSlotALoaded();         // empty A survived save/load

        OrbitCabAudioProcessor c; c.prepareToPlay (sr, block);
        const bool defaultLoaded = c.isSlotALoaded();        // back-compat: fresh instance keeps a cab in A

        const bool clrOk = startLoaded && clearedA && ridesEmpty && defaultLoaded;
        allPass &= clrOk;
        std::printf ("CLEAR TEST: startLoaded=%d clearedA=%d ridesEmpty=%d defaultLoaded=%d\n",
                     startLoaded, clearedA, ridesEmpty, defaultLoaded);
        std::printf ("RESULT: %s\n", clrOk ? "SLOT CLEAR WORKS (empty A rides state; default still loads)"
                                           : "SLOT CLEAR BROKEN");
    }

    // ---- a shared preset = the ACTIVE register only; A/B/C/D + their IRs are session-only ----
    // Load external IR1 as the live sound (register A), switch to B, load external IR2, and leave
    // B active. The session embeds both IRs in a <Workspace> (live + registers); a portable preset
    // is the live <Sound> only and embeds ONLY the active IR — so compare IRs (often proprietary)
    // can't leak.
    {
        auto mkIR = [&] (const char* nm, float freq) -> juce::File
        {
            auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (nm);
            const int n = (int) (0.05 * sr);
            juce::AudioBuffer<float> b (1, n);
            for (int i = 0; i < n; ++i)
            { const float t = (float) i / (float) sr; b.setSample (0, i, std::cos (juce::MathConstants<float>::twoPi * freq * t) * std::exp (-t * 40.0f)); }
            f.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os = f.createOutputStream();
            if (os != nullptr)
                if (auto w = wav.createWriterFor (os, juce::AudioFormatWriter::Options{}
                                                         .withSampleRate (sr).withNumChannels (1).withBitsPerSample (24)))
                    w->writeFromAudioSampleBuffer (b, 0, n);   // w destroyed at end of the if → finalises the WAV
            return f;
        };
        auto hasWorkspace = [] (const juce::MemoryBlock& mb) { auto x = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize()); return x != nullptr && x->getChildByName ("Workspace") != nullptr; };
        auto poolN    = [] (const juce::MemoryBlock& mb) { auto x = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize()); auto* p = x ? x->getChildByName ("IRPool") : nullptr; return p ? p->getNumChildElements() : (x ? 0 : -1); };

        OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
        auto ir1 = mkIR ("orbitcab_live.wav", 700.0f);
        auto ir2 = mkIR ("orbitcab_compare.wav", 1500.0f);
        a.loadIRFromFile (ir1, true); pump (80);        // register A live sound = IR1
        a.switchToSnapshot (1); pump (80);              // -> B
        a.loadIRFromFile (ir2, true); pump (80);        // B's sound = IR2 (the compare IR); leave B ACTIVE

        juce::MemoryBlock sess; a.getStateInformation (sess);
        juce::MemoryBlock pres; a.getStateForPreset  (pres);
        const bool sessSnaps =   hasWorkspace (sess);   // session keeps the compare registers (v4 <Workspace>)
        const bool presNo    = ! hasWorkspace (pres);   // preset is the live <Sound> only — no registers
        const int  sPool = poolN (sess), pPool = poolN (pres);
        ir1.deleteFile(); ir2.deleteFile();

        // session embeds both IRs (IR1 in snapshot A + IR2 live); preset embeds only the active (B) IR
        const bool scopeOk = sessSnaps && presNo && sPool >= 2 && pPool == 1;
        allPass &= scopeOk;
        std::printf ("PRESET SCOPE TEST: session(snaps=%d pool=%d) preset(noSnaps=%d pool=%d)\n",
                     sessSnaps, sPool, presNo, pPool);
        std::printf ("RESULT: %s\n", scopeOk ? "PRESET = ACTIVE SOUND ONLY (A/B/C/D + their IRs stay in session)"
                                             : "PRESET SCOPE BROKEN");
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

    // ============================================================================
    // v4 BUG-SCENARIO REGRESSION TESTS — the EXACT reported failures, driven through
    // the real OrbitCabAudioProcessor and asserted behaviour-first: each check states
    // the CORRECT outcome from the spec, then proves the code obeys it (not the reverse).
    // ============================================================================
    auto makeExt = [&] (const char* nm, float freq) -> juce::File
    {
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (nm);
        const int n = (int) (0.06 * sr);
        juce::AudioBuffer<float> b (1, n);
        for (int i = 0; i < n; ++i)
        { const float t = (float) i / (float) sr; b.setSample (0, i, std::cos (juce::MathConstants<float>::twoPi * freq * t) * std::exp (-t * 35.0f)); }
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os = f.createOutputStream();
        if (os != nullptr)
            if (auto w = wav.createWriterFor (os, juce::AudioFormatWriter::Options{}.withSampleRate (sr).withNumChannels (1).withBitsPerSample (24)))
                w->writeFromAudioSampleBuffer (b, 0, n);
        return f;
    };

    // ---- BUG A (the reported one): a portable preset's external-IR display name must survive
    //      an import, then an A/B/C/D round-trip, then undo — it must NEVER show the content id.
    {
        auto extf = makeExt ("orbitcab_namebug.wav", 850.0f);
        const juce::String fileName = extf.getFileName();
        juce::MemoryBlock preset;
        { OrbitCabAudioProcessor src; src.prepareToPlay (sr, block); src.loadIRFromFile (extf, true); pump (150); src.getStateForPreset (preset); }
        extf.deleteFile();                                   // a shared preset has no original file

        OrbitCabAudioProcessor p; p.prepareToPlay (sr, block);
        p.setStateInformation (preset.getData(), (int) preset.getSize());   // "import"
        pump (200);

        const bool refIsContentId = p.getSlotRef (true).startsWith ("ir-");   // external identity is content-addressed
        const bool nameOnImport   = p.getSlotName (true) == fileName;          // …but the UI shows the filename
        p.switchToSnapshot (1); pump (60); p.switchToSnapshot (0); pump (60);  // A→B→A (the old name-loss trigger)
        const bool nameAfterABCD  = p.getSlotName (true) == fileName;
        for (int i = 0; i < 20; ++i) p.undoTick();
        if (auto* q = p.apvts.getParameter ("gain")) q->setValueNotifyingHost (0.66f);
        for (int i = 0; i < 20; ++i) p.undoTick();
        p.undo(); pump (60);
        const bool nameAfterUndo  = p.getSlotName (true) == fileName;

        const bool ok = refIsContentId && nameOnImport && nameAfterABCD && nameAfterUndo;
        allPass &= ok;
        std::printf ("BUG-A TEST: refIsId=%d import=%d afterABCD=%d afterUndo=%d (shown=\"%s\")\n",
                     refIsContentId, nameOnImport, nameAfterABCD, nameAfterUndo, p.getSlotName (true).toRawUTF8());
        std::printf ("RESULT: %s\n", ok ? "DISPLAY NAME SURVIVES (no id-instead-of-filename)"
                                        : "BUG A REGRESSED (name lost / shows the id)");
    }

    // ---- re-export cascade: import a preset then export it again → the real name must persist
    //      (the old makePortable derived the name from the already-hashed ref → baked in "ir-…").
    {
        auto extf = makeExt ("orbitcab_reexport.wav", 1200.0f);
        const juce::String fileName = extf.getFileName();
        juce::MemoryBlock p1;
        { OrbitCabAudioProcessor src; src.prepareToPlay (sr, block); src.loadIRFromFile (extf, true); pump (150); src.getStateForPreset (p1); }
        extf.deleteFile();
        OrbitCabAudioProcessor mid; mid.prepareToPlay (sr, block);
        mid.setStateInformation (p1.getData(), (int) p1.getSize()); pump (150);
        juce::MemoryBlock p2; mid.getStateForPreset (p2);                       // re-export
        OrbitCabAudioProcessor dst; dst.prepareToPlay (sr, block);
        dst.setStateInformation (p2.getData(), (int) p2.getSize()); pump (150);
        const bool ok = dst.getSlotName (true) == fileName;
        allPass &= ok;
        std::printf ("RE-EXPORT TEST: name after import→re-export→import = \"%s\"\n", dst.getSlotName (true).toRawUTF8());
        std::printf ("RESULT: %s\n", ok ? "RE-EXPORT KEEPS THE NAME" : "RE-EXPORT CASCADE REGRESSED (name became the id)");
    }

    // ---- BUG B: loading a preset must RESET the A/B/C/D registers — pressing a register after
    //      the load must not recall the previous project's sound.
    {
        auto irX = makeExt ("orbitcab_regX.wav", 500.0f);
        OrbitCabAudioProcessor p; p.prepareToPlay (sr, block);
        p.switchToSnapshot (1); p.loadIRFromFile (irX, true); pump (120);       // register 1 = a distinct IR
        const juce::String regName = p.getSlotName (true);
        p.switchToSnapshot (0); pump (60);
        irX.deleteFile();
        juce::MemoryBlock preset;
        { OrbitCabAudioProcessor d; d.prepareToPlay (sr, block); d.getStateForPreset (preset); }   // a fresh Default
        p.setStateInformation (preset.getData(), (int) preset.getSize()); pump (150);
        const bool activeReset   = p.getActiveSnapshot() == 0;
        const juce::String before = p.getSlotName (true);
        p.switchToSnapshot (1); pump (80);                                      // the OLD register 1 must be gone
        const juce::String after  = p.getSlotName (true);
        const bool registerCleared = (after != regName) && (after == before);   // inherits current, not stale regX
        const bool ok = activeReset && registerCleared;
        allPass &= ok;
        std::printf ("BUG-B TEST: active=%d regWas=\"%s\" afterSwitch=\"%s\"\n",
                     p.getActiveSnapshot(), regName.toRawUTF8(), after.toRawUTF8());
        std::printf ("RESULT: %s\n", ok ? "PRESET LOAD RESETS A/B/C/D" : "BUG B REGRESSED (stale register recalled)");
    }

    // ---- BUG C: a snapshot switch is undoable — undo restores BOTH the live sound AND the
    //      active-register index (the chosen fully-reversible compare model).
    {
        OrbitCabAudioProcessor p; p.prepareToPlay (sr, block);
        auto setG  = [&] (float v) { if (auto* q = p.apvts.getParameter ("gain")) q->setValueNotifyingHost (v); };
        auto getG  = [&] { return p.apvts.getRawParameterValue ("gain")->load(); };
        auto ticks = [&] (int n) { for (int i = 0; i < n; ++i) p.undoTick(); };
        setG (0.20f); ticks (20);                          // register 0 settles
        const float reg0 = getG();
        p.switchToSnapshot (1); ticks (20);                // switch is one undo step
        setG (0.85f); ticks (20);                          // edit is another
        const float reg1 = getG();
        const int activeBefore = p.getActiveSnapshot();
        p.undo();                                          // undo the edit
        p.undo();                                          // undo the switch itself
        const int   activeAfter = p.getActiveSnapshot();
        const float soundAfter  = getG();
        // gain is stored in dB (getG is the raw value), so assert the INVARIANT, not a 0..1
        // literal: the two registers differ, and undo restores BOTH reg0's sound and active=0.
        const bool ok = std::abs (reg1 - reg0) > 1.0f && activeBefore == 1
                      && activeAfter == 0 && std::abs (soundAfter - reg0) < 1e-2f;
        allPass &= ok;
        std::printf ("BUG-C TEST: reg0=%.2f reg1=%.2f activeBefore=%d → activeAfter=%d sound=%.2f\n",
                     reg0, reg1, activeBefore, activeAfter, soundAfter);
        std::printf ("RESULT: %s\n", ok ? "SNAPSHOT SWITCH IS UNDOABLE (sound + active index)"
                                        : "BUG C REGRESSED (undo ignores the register)");
    }

    // ---- BUG F: a state whose external IR has no embedded bytes AND no file on disk must
    //      resolve to MISSING (slot not loaded, no phantom IR), keeping the name for relink.
    {
        auto extf = makeExt ("orbitcab_missing.wav", 950.0f);
        const juce::String fileName = extf.getFileName();
        juce::MemoryBlock session;
        { OrbitCabAudioProcessor src; src.prepareToPlay (sr, block); src.loadIRFromFile (extf, true); pump (150); src.getStateInformation (session); }
        extf.deleteFile();                                   // file gone…
        juce::MemoryBlock stripped;                           // …and strip the embedded pool → unresolvable
        if (auto xml = juce::AudioProcessor::getXmlFromBinary (session.getData(), (int) session.getSize()))
        {
            if (auto* pool = xml->getChildByName ("IRPool")) xml->removeChildElement (pool, true);
            juce::AudioProcessor::copyXmlToBinary (*xml, stripped);
        }
        OrbitCabAudioProcessor p; p.prepareToPlay (sr, block);
        p.setStateInformation (stripped.getData(), (int) stripped.getSize()); pump (200);
        const bool notLoaded = ! p.isSlotALoaded();
        const bool isMissing =   p.isSlotMissing (true);
        const bool nameKept  =   p.getSlotName (true) == fileName;
        const bool ok = notLoaded && isMissing && nameKept;
        allPass &= ok;
        std::printf ("BUG-F TEST: notLoaded=%d missing=%d nameKept=%d\n", notLoaded, isMissing, nameKept);
        std::printf ("RESULT: %s\n", ok ? "MISSING IR HANDLED (no phantom, name kept for relink)"
                                        : "BUG F REGRESSED (phantom IR or lost name)");
    }

    // ---- BUG G: clearSlotA and clearSlotB both canonicalise (trim→1, name cleared, status empty).
    {
        OrbitCabAudioProcessor p; p.prepareToPlay (sr, block);
        auto extf = makeExt ("orbitcab_clear.wav", 600.0f);
        using Status = orbitcab::state::SlotIR::Status;
        p.loadIRFromFile (extf, true);  p.setTrim (0.3f, true);  pump (120); p.clearSlotA();
        const bool aOk = ! p.isSlotALoaded() && p.getSlotName (true).isEmpty()
                       && std::abs (p.getTrim (true)  - 1.0f) < 1e-4f && p.getSlotIR (true).status  == Status::empty;
        p.loadIRFromFile (extf, false); p.setTrim (0.3f, false); pump (120); p.clearSlotB();
        const bool bOk = ! p.isSlotBLoaded() && p.getSlotName (false).isEmpty()
                       && std::abs (p.getTrim (false) - 1.0f) < 1e-4f && p.getSlotIR (false).status == Status::empty;
        extf.deleteFile();
        const bool ok = aOk && bOk;
        allPass &= ok;
        std::printf ("BUG-G TEST: clearA ok=%d clearB ok=%d\n", aOk, bOk);
        std::printf ("RESULT: %s\n", ok ? "CLEAR SLOT IS SYMMETRIC + CANONICAL" : "BUG G REGRESSED (asymmetric clear)");
    }

    // ---- RECENTS are a GLOBAL per-machine list (AppPreferences), NOT session state. Both checks
    //      touch the REAL global prefs, so snapshot + restore them — this harness must not clobber
    //      the user's actual recent-IR list.
    {
        juce::String savedRecents;
        { OrbitCabAudioProcessor s; s.prepareToPlay (sr, block); savedRecents = s.appPreferences().getString ("userIRs"); }

        // (1) switching to a preset must NOT wipe the accumulated user-IR list.
        {
            OrbitCabAudioProcessor p; p.prepareToPlay (sr, block);
            auto extf = makeExt ("orbitcab_recent.wav", 720.0f);
            p.addUserIR (extf);
            const bool had = p.getUserIRPaths().contains (extf.getFullPathName());
            juce::MemoryBlock preset;                         // a portable preset carries NO recents
            { OrbitCabAudioProcessor d; d.prepareToPlay (sr, block); d.getStateForPreset (preset); }
            p.setStateInformation (preset.getData(), (int) preset.getSize()); pump (120);
            const bool kept = p.getUserIRPaths().contains (extf.getFullPathName());
            extf.deleteFile();
            const bool ok = had && kept;
            allPass &= ok;
            std::printf ("RECENTS TEST: hadAfterOpen=%d keptAfterPreset=%d (count=%d)\n",
                         had, kept, p.getUserIRPaths().size());
            std::printf ("RESULT: %s\n", ok ? "RECENTS SURVIVE A PRESET LOAD"
                                            : "BUG: PRESET LOAD WIPES THE USER-IR LIST");
        }

        // (2) recents are SYSTEM-WIDE: a freshly-inserted plugin (new instance, no session state)
        //     must still see a folder added by a previous instance — through the global prefs.
        {
            auto extf = makeExt ("orbitcab_global.wav", 660.0f);
            { OrbitCabAudioProcessor a; a.prepareToPlay (sr, block); a.addUserIR (extf); }   // A adds → persists → dies
            OrbitCabAudioProcessor b; b.prepareToPlay (sr, block);                            // fresh instance B
            const bool seen = b.getUserIRPaths().contains (extf.getFullPathName());
            extf.deleteFile();
            allPass &= seen;
            std::printf ("RECENTS GLOBAL TEST: fresh instance sees prior instance's folder = %d\n", seen);
            std::printf ("RESULT: %s\n", seen ? "RECENTS ARE SYSTEM-WIDE (survive a new instance)"
                                              : "BUG: RECENTS NOT SYSTEM-WIDE (lost on re-insert)");
        }

        // restore the user's real global recents — no side effects from running this harness.
        { OrbitCabAudioProcessor s; s.prepareToPlay (sr, block); s.appPreferences().setString ("userIRs", savedRecents); }
    }

    // ---- FACTORY PRESETS: bundled, read-only; the first-start default is Roche Limit ----
    {
        const auto facs = orbitcab::factoryPresets();
        const bool hasFacs    = facs.size() >= 10;
        const bool hasDefault = orbitcab::findFactoryPreset (orbitcab::kDefaultPresetName).data != nullptr;

        OrbitCabAudioProcessor p; p.prepareToPlay (sr, block);
        const bool defaultIsRoche = p.isPresetFactory() && p.presetMeta().name == orbitcab::kDefaultPresetName;

        bool loadOther = false;                               // load a DIFFERENT factory preset → read-only, named, loaded
        for (const auto& fp : facs)
            if (fp.name != orbitcab::kDefaultPresetName)
            {
                p.loadFactoryPresetState (fp.data, fp.size); pump (60);
                loadOther = p.isPresetFactory() && p.presetMeta().name == fp.name && p.isSlotALoaded() && ! p.isPresetDirty();
                break;
            }

        const bool ok = hasFacs && hasDefault && defaultIsRoche && loadOther;
        allPass &= ok;
        std::printf ("FACTORY PRESETS TEST: count=%d hasDefault=%d defaultIsRoche=%d loadOther=%d\n",
                     (int) facs.size(), hasDefault, defaultIsRoche, loadOther);
        std::printf ("RESULT: %s\n", ok ? "FACTORY PRESETS WORK (bundled, read-only, default=Roche Limit)"
                                        : "FACTORY PRESETS BROKEN");
    }

    // ---- POWERAMP embed (v5): the selected .nam rides the save, deflated + lossless ----
    // The amp is a library SELECTION (ampSel), but for a REPRODUCIBLE project its bytes are
    // embedded in a <PowerampPool> (like an external IR). Prove: (1) a saved session/preset
    // embeds the model, (2) the deflated blob decodes back to a valid .nam (lossless), (3) ampSel
    // rides into a fresh instance and the amp re-arms from the pool there.
    {
        auto powerPoolN = [] (const juce::MemoryBlock& mb)
        { auto x = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
          auto* p = x ? x->getChildByName ("PowerampPool") : nullptr; return p ? p->getNumChildElements() : -1; };

        juce::String ampId;   // first factory amp (the test build embeds the .nam; skip cleanly if none)
        { OrbitCabAudioProcessor probe; for (const auto& e : probe.powerampLibrary()) if (e.factory) { ampId = e.id; break; } }

        if (ampId.isEmpty())
        {
            std::printf ("POWERAMP EMBED TEST: skipped (no factory .nam in this build)\n");
        }
        else
        {
            OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
            if (auto* p = a.apvts.getParameter ("ampOn")) p->setValueNotifyingHost (1.0f);
            a.selectPoweramp (ampId);
            pump (250);                                        // poll → applyPoweramp loads + stashes bytes

            const bool srcEmbeds = a.exportEmbedsAmp();
            juce::MemoryBlock sess; a.getStateInformation (sess);
            juce::MemoryBlock pres; a.getStateForPreset  (pres);
            const int sPool = powerPoolN (sess), pPool = powerPoolN (pres);

            // base64 -> inflate -> must parse as a real .nam (proves deflate/inflate is lossless)
            bool decodesToNam = false; int rawSz = 0, deflSz = 0;
            if (auto x = juce::AudioProcessor::getXmlFromBinary (sess.getData(), (int) sess.getSize()))
                if (auto* pool = x->getChildByName ("PowerampPool"))
                    if (auto* e = pool->getFirstChildElement())
                    {
                        juce::MemoryOutputStream defl;
                        if (juce::Base64::convertFromBase64 (defl, e->getStringAttribute ("nam")))
                        {
                            deflSz = (int) defl.getDataSize();
                            juce::MemoryInputStream in (defl.getData(), defl.getDataSize(), false);
                            juce::GZIPDecompressorInputStream gz (in);
                            juce::MemoryBlock raw; gz.readIntoMemoryBlock (raw);
                            rawSz = (int) raw.getSize();
                            auto j = juce::JSON::parse (juce::String::fromUTF8 ((const char*) raw.getData(), (int) raw.getSize()));
                            decodesToNam = j.isObject() && j.hasProperty ("architecture") && j.hasProperty ("weights");
                        }
                    }

            OrbitCabAudioProcessor b; b.prepareToPlay (sr, block);
            b.setStateInformation (sess.getData(), (int) sess.getSize());
            pump (250);
            const bool selRides = b.selectedPowerampId() == ampId;   // ampSel rode the save
            const bool bEmbeds  = b.exportEmbedsAmp();                // re-armed from the pool on the fresh instance
            const bool shrank   = deflSz > 0 && rawSz > 0 && deflSz < rawSz;   // deflate actually compressed

            // the loaded amp renders cleanly in the full chain (finite — no NaN/Inf from the model
            // or the rate-matcher). Proves the embedded model actually runs, not just deserialises.
            bool rendersClean = true;
            {
                juce::AudioBuffer<float> bb (2, block); juce::MidiBuffer mm;
                for (int k = 0; k < 24; ++k)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < block; ++i)
                            bb.setSample (ch, i, 0.3f * std::sin (juce::MathConstants<float>::twoPi * 300.0f * (float) (k * block + i) / (float) sr));
                    b.processBlock (bb, mm);
                    for (int ch = 0; ch < 2 && rendersClean; ++ch)
                        for (int i = 0; i < block; ++i)
                            if (! std::isfinite (bb.getSample (ch, i))) { rendersClean = false; break; }
                }
            }

            // v4 back-compat: a save WITHOUT a <PowerampPool> (an older state, or a stripped one)
            // must still resolve the amp from the library — the pool is an add-on, not a requirement.
            bool v4Compat = false;
            if (auto x = juce::AudioProcessor::getXmlFromBinary (sess.getData(), (int) sess.getSize()))
            {
                if (auto* pool = x->getChildByName ("PowerampPool")) x->removeChildElement (pool, true);
                juce::MemoryBlock noPool; juce::AudioProcessor::copyXmlToBinary (*x, noPool);
                OrbitCabAudioProcessor d; d.prepareToPlay (sr, block);
                d.setStateInformation (noPool.getData(), (int) noPool.getSize());
                pump (250);
                v4Compat = d.selectedPowerampId() == ampId && d.exportEmbedsAmp();   // resolved from the library
            }

            const bool ampOk = srcEmbeds && sPool == 1 && pPool == 1 && decodesToNam
                               && selRides && bEmbeds && shrank && rendersClean && v4Compat;
            allPass &= ampOk;
            std::printf ("POWERAMP EMBED TEST: src=%d sess=%d pres=%d nam=%d sel=%d reload=%d render=%d v4compat=%d zip(%d->%d)\n",
                         srcEmbeds, sPool, pPool, decodesToNam, selRides, bEmbeds, rendersClean, v4Compat, rawSz, deflSz);
            std::printf ("RESULT: %s\n", ampOk ? "POWERAMP RIDES STATE (embedded, deflated, lossless, reproducible, v4-compatible)"
                                               : "POWERAMP EMBED BROKEN");
        }
    }

    // ---- PREAMP embed (v6): the selected preamp .nam rides the save, deflated + lossless ----
    // Exact mirror of the POWERAMP embed test, against the second NAM stage (preampOn / preampSel /
    // <PreampPool> / exportEmbedsPreamp). Proves the preamp embeds, decodes back losslessly, rides
    // into a fresh instance, re-arms there, renders cleanly, and stays v5-compatible (no pool).
    {
        auto preampPoolN = [] (const juce::MemoryBlock& mb)
        { auto x = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
          auto* p = x ? x->getChildByName ("PreampPool") : nullptr; return p ? p->getNumChildElements() : -1; };

        juce::String preId;   // first factory preamp (the test build embeds the .nam; skip cleanly if none)
        { OrbitCabAudioProcessor probe; for (const auto& e : probe.preampLibrary()) if (e.factory) { preId = e.id; break; } }

        if (preId.isEmpty())
        {
            std::printf ("PREAMP EMBED TEST: skipped (no factory .nam in this build)\n");
        }
        else
        {
            OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
            if (auto* p = a.apvts.getParameter ("preampOn")) p->setValueNotifyingHost (1.0f);
            a.selectPreamp (preId);
            pump (250);                                        // poll → applyPreamp loads + stashes bytes

            const bool srcEmbeds = a.exportEmbedsPreamp();
            juce::MemoryBlock sess; a.getStateInformation (sess);
            juce::MemoryBlock pres; a.getStateForPreset  (pres);
            const int sPool = preampPoolN (sess), pPool = preampPoolN (pres);

            bool decodesToNam = false; int rawSz = 0, deflSz = 0;
            if (auto x = juce::AudioProcessor::getXmlFromBinary (sess.getData(), (int) sess.getSize()))
                if (auto* pool = x->getChildByName ("PreampPool"))
                    if (auto* e = pool->getFirstChildElement())
                    {
                        juce::MemoryOutputStream defl;
                        if (juce::Base64::convertFromBase64 (defl, e->getStringAttribute ("nam")))
                        {
                            deflSz = (int) defl.getDataSize();
                            juce::MemoryInputStream in (defl.getData(), defl.getDataSize(), false);
                            juce::GZIPDecompressorInputStream gz (in);
                            juce::MemoryBlock raw; gz.readIntoMemoryBlock (raw);
                            rawSz = (int) raw.getSize();
                            auto j = juce::JSON::parse (juce::String::fromUTF8 ((const char*) raw.getData(), (int) raw.getSize()));
                            decodesToNam = j.isObject() && j.hasProperty ("architecture") && j.hasProperty ("weights");
                        }
                    }

            OrbitCabAudioProcessor b; b.prepareToPlay (sr, block);
            b.setStateInformation (sess.getData(), (int) sess.getSize());
            pump (250);
            const bool selRides = b.selectedPreampId() == preId;
            const bool bEmbeds  = b.exportEmbedsPreamp();
            const bool shrank   = deflSz > 0 && rawSz > 0 && deflSz < rawSz;

            bool rendersClean = true;
            {
                juce::AudioBuffer<float> bb (2, block); juce::MidiBuffer mm;
                for (int k = 0; k < 24; ++k)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < block; ++i)
                            bb.setSample (ch, i, 0.3f * std::sin (juce::MathConstants<float>::twoPi * 300.0f * (float) (k * block + i) / (float) sr));
                    b.processBlock (bb, mm);
                    for (int ch = 0; ch < 2 && rendersClean; ++ch)
                        for (int i = 0; i < block; ++i)
                            if (! std::isfinite (bb.getSample (ch, i))) { rendersClean = false; break; }
                }
            }

            // v5 back-compat: a save WITHOUT a <PreampPool> must still resolve the preamp from the library.
            bool v5Compat = false;
            if (auto x = juce::AudioProcessor::getXmlFromBinary (sess.getData(), (int) sess.getSize()))
            {
                if (auto* pool = x->getChildByName ("PreampPool")) x->removeChildElement (pool, true);
                juce::MemoryBlock noPool; juce::AudioProcessor::copyXmlToBinary (*x, noPool);
                OrbitCabAudioProcessor d; d.prepareToPlay (sr, block);
                d.setStateInformation (noPool.getData(), (int) noPool.getSize());
                pump (250);
                v5Compat = d.selectedPreampId() == preId && d.exportEmbedsPreamp();
            }

            const bool preOk = srcEmbeds && sPool == 1 && pPool == 1 && decodesToNam
                               && selRides && bEmbeds && shrank && rendersClean && v5Compat;
            allPass &= preOk;
            std::printf ("PREAMP EMBED TEST: src=%d sess=%d pres=%d nam=%d sel=%d reload=%d render=%d v5compat=%d zip(%d->%d)\n",
                         srcEmbeds, sPool, pPool, decodesToNam, selRides, bEmbeds, rendersClean, v5Compat, rawSz, deflSz);
            std::printf ("RESULT: %s\n", preOk ? "PREAMP RIDES STATE (embedded, deflated, lossless, reproducible, v5-compatible)"
                                                : "PREAMP EMBED BROKEN");
        }
    }

    // ---- PREAMP + POWERAMP together: both stages on, both render finite through the full chain ----
    // Proves the two NAM instances coexist (input → preamp → poweramp → cab) without NaN/Inf and that
    // both selections ride a save independently.
    {
        juce::String preId, ampId;
        { OrbitCabAudioProcessor probe;
          for (const auto& e : probe.preampLibrary())   if (e.factory) { preId = e.id; break; }
          for (const auto& e : probe.powerampLibrary()) if (e.factory) { ampId = e.id; break; } }

        if (preId.isEmpty() || ampId.isEmpty())
        {
            std::printf ("PRE+POWER TEST: skipped (need a factory preamp AND poweramp in this build)\n");
        }
        else
        {
            OrbitCabAudioProcessor a; a.prepareToPlay (sr, block);
            if (auto* p = a.apvts.getParameter ("preampOn")) p->setValueNotifyingHost (1.0f);
            if (auto* p = a.apvts.getParameter ("ampOn"))    p->setValueNotifyingHost (1.0f);
            a.selectPreamp (preId);
            a.selectPoweramp (ampId);
            pump (250);

            bool rendersClean = true;
            juce::AudioBuffer<float> bb (2, block); juce::MidiBuffer mm;
            for (int k = 0; k < 24; ++k)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < block; ++i)
                        bb.setSample (ch, i, 0.3f * std::sin (juce::MathConstants<float>::twoPi * 300.0f * (float) (k * block + i) / (float) sr));
                a.processBlock (bb, mm);
                for (int ch = 0; ch < 2 && rendersClean; ++ch)
                    for (int i = 0; i < block; ++i)
                        if (! std::isfinite (bb.getSample (ch, i))) { rendersClean = false; break; }
            }

            juce::MemoryBlock sess; a.getStateInformation (sess);
            OrbitCabAudioProcessor b; b.prepareToPlay (sr, block);
            b.setStateInformation (sess.getData(), (int) sess.getSize());
            pump (250);
            const bool bothRide  = b.selectedPreampId() == preId && b.selectedPowerampId() == ampId;
            const bool bothEmbed = b.exportEmbedsPreamp() && b.exportEmbedsAmp();   // both pools re-armed on restore

            const bool ok = rendersClean && bothRide && bothEmbed;
            allPass &= ok;
            std::printf ("PRE+POWER TEST: render=%d bothRide=%d bothEmbed=%d\n", rendersClean, bothRide, bothEmbed);
            std::printf ("RESULT: %s\n", ok ? "PREAMP + POWERAMP COEXIST (both render finite + ride state + re-arm)"
                                            : "PREAMP + POWERAMP BROKEN");
        }
    }

    std::printf ("\n==== %s ====\n", allPass ? "ALL DSP CHECKS PASSED" : "SOME DSP CHECKS FAILED");
    return allPass ? 0 : 1;
}
