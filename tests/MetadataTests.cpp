// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for orbitcab::Metadata — the versioned preset-identity model
// (PresetMeta + IrRef) behind the <meta> block. Proves the JSON round-trip preserves
// known fields AND unknown keys (forward-compat), and that schemaVersion / the reserved
// IrRef.id behave as the design requires. juce_core only — no audio, no host.
#include <juce_core/juce_core.h>

#include "Metadata.h"

using namespace orbitcab;

namespace
{
    // Full round-trip through the on-disk form: var → JSON string → parsed var → struct.
    PresetMeta roundTrip (const PresetMeta& m)
    {
        return PresetMeta::fromVar (parseJSON (toJSON (m.toVar())));
    }
}

struct MetadataTest : juce::UnitTest
{
    MetadataTest() : juce::UnitTest ("Metadata") {}

    void runTest() override
    {
        //--------------------------------------------------------------------
        beginTest ("PresetMeta: known fields + irRefs survive a JSON round-trip");
        {
            PresetMeta m;
            m.name        = "Crunch Box";
            m.description = "2x12 with a tilted SM57";
            m.author      = "Darwin";
            m.tags        = { "guitar", "crunch", "emerald" };
            m.createdAt   = "2026-06-21T10:00:00.000Z";
            m.appVersion  = "1.1.0";

            IrRef a; a.slot = "A"; a.name = "16-nacho-guacamole"; a.bundled = true;
            a.fallback = "16-nacho-guacamole.wav";
            IrRef b; b.slot = "B"; b.name = "my-cab"; b.bundled = false;
            b.fallback = "/Users/x/IR/my-cab.wav";
            m.irRefs = { a, b };

            const auto r = roundTrip (m);
            expectEquals (r.name, m.name);
            expectEquals (r.description, m.description);
            expectEquals (r.author, m.author);
            expectEquals (r.tags.size(), 3);
            expect (r.tags == m.tags, "tags array must round-trip in order");
            expectEquals (r.createdAt, m.createdAt);
            expectEquals (r.appVersion, m.appVersion);
            expectEquals ((int) r.irRefs.size(), 2);
            expectEquals (r.irRefs[0].slot, juce::String ("A"));
            expect (r.irRefs[0].bundled, "slot A is bundled");
            expectEquals (r.irRefs[0].fallback, juce::String ("16-nacho-guacamole.wav"));
            expectEquals (r.irRefs[1].slot, juce::String ("B"));
            expect (! r.irRefs[1].bundled, "slot B is an external file");
            expectEquals (r.irRefs[1].fallback, juce::String ("/Users/x/IR/my-cab.wav"));
        }

        //--------------------------------------------------------------------
        beginTest ("IrRef.id is reserved empty by default, but round-trips when set");
        {
            IrRef def;
            expect (def.id.isEmpty(), "id defaults empty (reserved for the hashing milestone)");

            // A future build that fills id must survive a round-trip unchanged.
            IrRef withId; withId.slot = "A"; withId.id = "sha256:deadbeef"; withId.bundled = true;
            const auto r = IrRef::fromVar (parseJSON (toJSON (withId.toVar())));
            expectEquals (r.id, juce::String ("sha256:deadbeef"));
        }

        //--------------------------------------------------------------------
        beginTest ("forward-compat: unknown keys on PresetMeta are preserved on round-trip");
        {
            // Simulate a preset authored by a NEWER build carrying fields this build doesn't
            // know (favorite, rating, a nested object). They must survive untouched.
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("schemaVersion", 2);
            o->setProperty ("name", "Future");
            o->setProperty ("favorite", true);
            o->setProperty ("rating", 5);
            juce::DynamicObject::Ptr nested = new juce::DynamicObject();
            nested->setProperty ("hue", 210);
            o->setProperty ("ui", juce::var (nested.get()));

            const auto m = PresetMeta::fromVar (juce::var (o.get()));
            expectEquals (m.name, juce::String ("Future"));
            expectEquals (m.schemaVersion, 2);   // higher read-back number is preserved, not clamped

            // Re-emit and confirm the unknown keys are still there.
            const auto out = m.toVar();
            auto* oo = out.getDynamicObject();
            expect (oo != nullptr);
            expect (oo->hasProperty ("favorite") && (bool) oo->getProperty ("favorite"), "favorite preserved");
            expectEquals ((int) oo->getProperty ("rating"), 5);
            expect (oo->hasProperty ("ui"), "nested unknown object preserved");
            expectEquals ((int) oo->getProperty ("ui").getDynamicObject()->getProperty ("hue"), 210);

            // And it survives a full string round-trip too.
            const auto r = roundTrip (m);
            const auto rv = r.toVar();
            expect ((bool) rv.getDynamicObject()->getProperty ("favorite"), "favorite survives JSON round-trip");
        }

        //--------------------------------------------------------------------
        beginTest ("forward-compat: unknown keys on an IrRef are preserved");
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("slot", "A");
            o->setProperty ("bundled", false);
            o->setProperty ("fallback", "/p/x.wav");
            o->setProperty ("gainDb", -3.5);    // a field a future build might add to a ref

            const auto r = IrRef::fromVar (juce::var (o.get()));
            expectEquals (r.slot, juce::String ("A"));
            const auto out = r.toVar();
            expect (out.getDynamicObject()->hasProperty ("gainDb"), "unknown IrRef key preserved");
            expectWithinAbsoluteError ((double) out.getDynamicObject()->getProperty ("gainDb"), -3.5, 1.0e-9);
        }

        //--------------------------------------------------------------------
        beginTest ("schemaVersion: a default-constructed meta emits the current version");
        {
            PresetMeta m; m.name = "Plain";
            const auto r = roundTrip (m);
            expectEquals (r.schemaVersion, kPresetSchemaVersion);
        }

        //--------------------------------------------------------------------
        beginTest ("defensive: a null/empty var parses to a clean default meta");
        {
            const auto m = PresetMeta::fromVar (juce::var());
            expect (m.name.isEmpty(), "no name");
            expect (m.irRefs.empty(), "no refs");
            expectEquals (m.schemaVersion, kPresetSchemaVersion);

            const auto r = IrRef::fromVar (juce::var());
            expect (r.slot.isEmpty() && r.fallback.isEmpty() && ! r.bundled, "empty IrRef");
        }
    }
};

static MetadataTest metadataTest;
