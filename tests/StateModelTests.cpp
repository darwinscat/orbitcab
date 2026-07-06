// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for orbitcab::state — the single-source-of-truth slot/sound/
// workspace model + its (de)serialisation, dirty fingerprint and v3→v4 migration.
// These cover the bug classes the v4 redesign exists to kill: the display name must
// survive every serialisation (session/snapshot/undo/preset), an empty slot must
// canonicalise, the fingerprint must ignore machine-local paths, and legacy state
// must migrate. juce_core only — no audio, no host.
#include <juce_core/juce_core.h>

#include "StateModel.h"

using namespace orbitcab::state;

namespace
{
    juce::ValueTree makeParams (const juce::String& tag, double gain)
    {
        juce::ValueTree p ("PARAMS");
        p.setProperty ("tag", tag, nullptr);
        p.setProperty ("gain", gain, nullptr);
        p.setProperty ("headTrim", true, nullptr);
        return p;
    }

    SlotIR readyExternal()
    {
        SlotIR s;
        s.status = SlotIR::Status::ready;
        s.bundled = false;
        s.ref = "ir-abc123";
        s.displayName = "Mesa 4x12.wav";
        s.localPath = "/Users/x/IR/Mesa 4x12.wav";
        s.trim = 0.72f;
        return s;
    }

    SlotIR readyBundled()
    {
        SlotIR s;
        s.status = SlotIR::Status::ready;
        s.bundled = true;
        s.ref = "16-nacho-guacamole.wav";
        s.displayName = "16-nacho-guacamole.wav";
        s.trim = 1.0f;
        return s;
    }
}

struct StateModelTest : juce::UnitTest
{
    StateModelTest() : juce::UnitTest ("StateModel") {}

    void runTest() override
    {
        //--------------------------------------------------------------------
        beginTest ("SlotIR: external ready slot round-trips incl. display name + localPath (session)");
        {
            const auto s = readyExternal();
            const auto r = slotFromTree (toTree (s, "A", /*portable*/ false));
            expect (r.status == SlotIR::Status::ready);
            expect (! r.bundled);
            expectEquals (r.ref, juce::String ("ir-abc123"));
            expectEquals (r.displayName, juce::String ("Mesa 4x12.wav"));   // THE name must survive (bug A)
            expectEquals (r.localPath, juce::String ("/Users/x/IR/Mesa 4x12.wav"));
            expectWithinAbsoluteError (r.trim, 0.72f, 1.0e-6f);
        }

        //--------------------------------------------------------------------
        beginTest ("SlotIR: portable mode drops localPath but keeps ref + display name");
        {
            const auto r = slotFromTree (toTree (readyExternal(), "A", /*portable*/ true));
            expect (r.localPath.isEmpty(), "portable export must not leak the local path");
            expectEquals (r.ref, juce::String ("ir-abc123"));
            expectEquals (r.displayName, juce::String ("Mesa 4x12.wav"));
        }

        //--------------------------------------------------------------------
        beginTest ("SlotIR: bundled slot keeps its filename ref (stable key, never hashed)");
        {
            const auto r = slotFromTree (toTree (readyBundled(), "A", false));
            expect (r.bundled);
            expectEquals (r.ref, juce::String ("16-nacho-guacamole.wav"));
            expectEquals (r.displayName, juce::String ("16-nacho-guacamole.wav"));
        }

        //--------------------------------------------------------------------
        beginTest ("SlotIR: empty slot canonicalises (no stale ref/name/trim)");
        {
            SlotIR dirty;                              // simulate a slot that WAS loaded then cleared sloppily
            dirty.status = SlotIR::Status::empty;
            dirty.ref = "ir-stale"; dirty.displayName = "old"; dirty.trim = 0.3f; dirty.localPath = "/old";
            const auto r = slotFromTree (toTree (dirty, "B", false));
            expect (! r.occupied());
            expect (r.ref.isEmpty() && r.displayName.isEmpty() && r.localPath.isEmpty(), "empty slot is identity-free");
            expectWithinAbsoluteError (r.trim, 1.0f, 1.0e-6f);   // canonical trim
        }

        //--------------------------------------------------------------------
        beginTest ("SlotIR: occupied-but-ref-less degrades to empty");
        {
            juce::ValueTree t ("Slot");
            t.setProperty ("side", "A", nullptr);
            t.setProperty ("status", "ready", nullptr);   // claims ready but carries no ref
            expect (! slotFromTree (t).occupied());
        }

        //--------------------------------------------------------------------
        beginTest ("SoundState: params + both slots survive a round-trip");
        {
            SoundState s;
            s.params   = makeParams ("hello", -6.0);
            s.slots[0] = readyExternal();
            s.slots[1] = readyBundled();

            const auto r = soundFromTree (toTree (s, false));
            expect (r.params.isValid());
            expectEquals (r.params.getProperty ("tag").toString(), juce::String ("hello"));
            expectWithinAbsoluteError ((double) r.params.getProperty ("gain"), -6.0, 1.0e-9);
            expectEquals (r.slots[0].displayName, juce::String ("Mesa 4x12.wav"));
            expect (r.slots[1].bundled);
            expectEquals (r.slots[1].ref, juce::String ("16-nacho-guacamole.wav"));
        }

        //--------------------------------------------------------------------
        beginTest ("Workspace: active index + present/absent registers round-trip");
        {
            Workspace w;
            w.active = 2;
            w.live.params   = makeParams ("live", 0.0);
            w.live.slots[0] = readyBundled();
            SoundState reg;
            reg.params   = makeParams ("regB", -3.0);
            reg.slots[0] = readyExternal();
            w.snapshots[1] = reg;                      // only register B populated

            const auto r = workspaceFromTree (toTree (w, false));
            expectEquals (r.active, 2);
            expectEquals (r.live.params.getProperty ("tag").toString(), juce::String ("live"));
            expect (! r.snapshots[0].has_value(), "register A stays empty");
            expect (r.snapshots[1].has_value(), "register B round-trips");
            expectEquals (r.snapshots[1]->slots[0].displayName, juce::String ("Mesa 4x12.wav"));
            expect (! r.snapshots[2].has_value() && ! r.snapshots[3].has_value());
        }

        //--------------------------------------------------------------------
        beginTest ("fingerprint: stable across re-serialise, ignores localPath, reacts to identity");
        {
            SoundState base;
            base.params   = makeParams ("fp", 1.0);
            base.slots[0] = readyExternal();
            base.slots[1] = SlotIR::makeEmpty();

            const auto f0 = fingerprint (base);
            // re-serialise + read back → identical hash
            expectEquals (fingerprint (soundFromTree (toTree (base, false))), f0);

            // localPath must NOT affect dirty (same sound on another machine)
            auto movedPath = base;
            movedPath.slots[0].localPath = "/somewhere/else/Mesa 4x12.wav";
            expectEquals (fingerprint (movedPath), f0);

            // name change MUST flip it (display-name corruption is now observable)
            auto renamed = base; renamed.slots[0].displayName = "Other.wav";
            expect (fingerprint (renamed) != f0, "name change must dirty");

            // ref / trim / params changes flip it
            auto reref = base; reref.slots[0].ref = "ir-different";
            expect (fingerprint (reref) != f0, "ref change must dirty");
            auto retrim = base; retrim.slots[0].trim = 0.5f;
            expect (fingerprint (retrim) != f0, "trim change must dirty");
            auto reparam = base; reparam.params = makeParams ("fp", 2.0);
            expect (fingerprint (reparam) != f0, "param change must dirty");
        }

        //--------------------------------------------------------------------
        beginTest ("fingerprint: an empty slot's hidden trim cannot affect dirty (bug G)");
        {
            SoundState a; a.params = makeParams ("g", 0.0); a.slots[0] = readyBundled();
            SoundState b = a;
            b.slots[1].status = SlotIR::Status::empty;   // empty…
            b.slots[1].trim   = 0.1f;                     // …with stale trim that must be ignored
            a.slots[1] = SlotIR::makeEmpty();
            expectEquals (fingerprint (a), fingerprint (b));
        }

        //--------------------------------------------------------------------
        beginTest ("migration: legacy bundled / external-path / portable-hash / empty-B");
        {
            // a legacy v3 <IR> node
            juce::ValueTree ir ("IR");
            ir.setProperty ("aLoaded", true,  nullptr);
            ir.setProperty ("aBundled", false, nullptr);
            ir.setProperty ("aRef", "/Users/x/IR/Greenback.wav", nullptr);   // absolute path (session)
            ir.setProperty ("aName", {}, nullptr);                            // no stored name → from filename
            ir.setProperty ("aTrim", 0.8f, nullptr);
            ir.setProperty ("bLoaded", false, nullptr);                       // B empty

            const auto s = migrateLegacySound (ir, makeParams ("mig", 0.0));
            expect (s.slots[0].status == SlotIR::Status::ready);
            expect (! s.slots[0].bundled);
            expectEquals (s.slots[0].localPath, juce::String ("/Users/x/IR/Greenback.wav"));
            expectEquals (s.slots[0].displayName, juce::String ("Greenback.wav"));   // derived from filename
            expectWithinAbsoluteError (s.slots[0].trim, 0.8f, 1.0e-6f);
            expect (! s.slots[1].occupied(), "legacy bLoaded=false → empty B");

            // portable-hash external with a stored name
            juce::ValueTree ir2 ("IR");
            ir2.setProperty ("aLoaded", true, nullptr);
            ir2.setProperty ("aBundled", false, nullptr);
            ir2.setProperty ("aRef", "ir-deadbeef", nullptr);
            ir2.setProperty ("aName", "Vintage 30.wav", nullptr);
            const auto s2 = migrateLegacySound (ir2, juce::ValueTree());
            expectEquals (s2.slots[0].ref, juce::String ("ir-deadbeef"));
            expectEquals (s2.slots[0].displayName, juce::String ("Vintage 30.wav"));
            expect (s2.slots[0].localPath.isEmpty(), "portable hash has no local path");

            // bundled legacy with no stored name → from filename ref
            juce::ValueTree ir3 ("IR");
            ir3.setProperty ("bLoaded", true, nullptr);
            ir3.setProperty ("bBundled", true, nullptr);
            ir3.setProperty ("bRef", "03-grinder.wav", nullptr);
            const auto s3 = migrateLegacySound (ir3, juce::ValueTree());
            expect (s3.slots[1].bundled);
            expectEquals (s3.slots[1].displayName, juce::String ("03-grinder.wav"));
        }
    }
};

static StateModelTest stateModelTest;
