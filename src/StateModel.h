// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_data_structures/juce_data_structures.h>   // juce::ValueTree (+ pulls juce_core)

#include <array>
#include <optional>

//==============================================================================
// orbitcab::state — the SINGLE SOURCE OF TRUTH for a slot's IR identity and for
// every serialisation of the plugin's "sound" (session save, A/B/C/D registers,
// undo/redo, preset export). Before v4 the slot identity was split across loose
// processor members and projected by three independent, lossy writers
// (writeIRRefs / captureStateTree / buildStateTree) that disagreed about which
// fields they carried — so the display name, the active register, and the undo
// timeline drifted apart. This module makes ONE struct + ONE (de)serialiser pair
// that all of them route through, so they can never drift again.
//
// Pure data — juce_core only (no audio, no GUI, no host). Builds into the headless
// test target and is unit-tested directly (mirrors Metadata.h). The FUNCTIONAL IR
// load (decode → engine, embedded-bytes pool) lives in the processor; this layer
// is the declarative identity + the (de)serialisation contract around it.
//
// External-IR identity is CONTENT-ADDRESSED at load time: `ref` is "ir-<hex>", a
// hash of the canonical (embedded) WAV bytes — the SAME id in a session and in a
// portable preset. So export no longer rewrites refs (the old path→hash swap that
// baked the hash in as the display name); privacy comes from dropping `localPath`
// + recents in portable mode, not from mangling the ref.
//==============================================================================
namespace orbitcab::state
{

// Root <OrbitCabState> tree version this model reads/writes. v3 (and the param-only
// pre-v3 form) are migrated on load; see migrateLegacySound().
// v5 adds <PowerampPool> (the selected .nam embedded, deflated) so a session/preset is
// self-contained for the amp too — older builds ignore the pool and resolve from the library.
// v6 adds <PreampPool> (the same, for the second/preamp NAM stage) + the "preampSel" property;
// older builds ignore both, so it stays compatible in BOTH directions.
inline constexpr int kStateVersion = 6;

//============================================================ one slot's IR ====
struct SlotIR
{
    // Runtime status. `ready`/`missing` is resolved by the processor when it loads
    // the ref (bytes present → ready, bytes gone → missing); the tree stores only
    // "is a cab intended here" (occupied) + the identity, and the processor flips
    // ready↔missing on resolve. So serialise treats missing like ready (keep the
    // ref so a later relink works); only `empty` drops the identity.
    enum class Status { empty, ready, missing };

    Status       status = Status::empty;
    bool         bundled = false;     // true → `ref` is a bundled filename (stable key, never hashed)
    juce::String ref;                 // bundled filename | external content id "ir-<hex>"
    juce::String displayName;         // user-facing name; ALWAYS set for occupied slots
    juce::String localPath;           // session-only recovery path (external only); dropped in portable export
    float        trim = 1.0f;         // [0,1]; 1 = full length

    bool occupied() const { return status != Status::empty; }

    // Canonicalise an empty slot so two "no cab" slots hash/compare identically
    // (no stale ref/name/trim riding snapshots, undo or the dirty fingerprint).
    static SlotIR makeEmpty() { return {}; }

    // A loaded bundled IR — `ref` is the stable bundled filename (never content-hashed,
    // so a plugin update that re-masters a factory IR doesn't orphan old sessions).
    static SlotIR makeBundled (const juce::String& filename)
    {
        SlotIR s; s.status = Status::ready; s.bundled = true; s.ref = filename; s.displayName = filename;
        return s;
    }

    // A loaded external IR — `ref` is the content id "ir-<hex>"; `localPath` is the
    // session-only recovery hint (dropped in portable export).
    static SlotIR makeExternal (const juce::String& blobId, const juce::String& name, const juce::String& path)
    {
        SlotIR s; s.status = Status::ready; s.bundled = false; s.ref = blobId; s.displayName = name; s.localPath = path;
        return s;
    }
};

//============================================================ a full "sound" ===
// One self-contained sound: the parameter tree (opaque copy of the APVTS state —
// the model never inspects it) + both slots' IR identity. This is what a preset
// is, what each A/B/C/D register holds, and what one undo step restores.
struct SoundState
{
    juce::ValueTree        params;          // copy of the APVTS state tree (incl. the headTrim property)
    std::array<SlotIR, 2>  slots;           // [0] = A, [1] = B
};

//===================================================== the compare workspace ===
// The live sound + the 4 A/B/C/D registers + which one is active. The whole thing
// is one undo unit (so a register switch is exactly reversible) and is what a
// session persists. A portable preset persists only `live`.
struct Workspace
{
    SoundState                              live;
    int                                     active = 0;
    std::array<std::optional<SoundState>, 4> snapshots;   // inactive registers (live IS the active one)
};

//============================================================ serialisation ====
namespace detail
{
    inline const char* statusToString (SlotIR::Status s)
    {
        switch (s) { case SlotIR::Status::ready:   return "ready";
                     case SlotIR::Status::missing: return "missing";
                     case SlotIR::Status::empty:   break; }
        return "empty";
    }

    inline SlotIR::Status statusFromString (const juce::String& s)
    {
        if (s == "ready")   return SlotIR::Status::ready;
        if (s == "missing") return SlotIR::Status::missing;
        return SlotIR::Status::empty;
    }
}

// --- one slot <Slot side=.. status=.. bundled=.. ref=.. name=.. trim=.. [path=..]> ---
inline juce::ValueTree toTree (const SlotIR& s, const juce::String& side, bool portable)
{
    juce::ValueTree t ("Slot");
    t.setProperty ("side", side, nullptr);

    if (! s.occupied())                       // canonical empty: identity-free
    {
        t.setProperty ("status", "empty", nullptr);
        t.setProperty ("trim", 1.0f, nullptr);
        return t;
    }

    t.setProperty ("status",  detail::statusToString (s.status), nullptr);
    t.setProperty ("bundled", s.bundled,     nullptr);
    t.setProperty ("ref",     s.ref,         nullptr);
    t.setProperty ("name",    s.displayName, nullptr);
    t.setProperty ("trim",    s.trim,        nullptr);
    if (! portable && ! s.bundled && s.localPath.isNotEmpty())
        t.setProperty ("path", s.localPath, nullptr);   // session-only recovery hint
    return t;
}

inline SlotIR slotFromTree (const juce::ValueTree& t)
{
    SlotIR s;
    if (! t.isValid())
        return s;
    s.status = detail::statusFromString (t.getProperty ("status", "empty").toString());
    if (s.status == SlotIR::Status::empty)
        return SlotIR::makeEmpty();
    s.bundled     = (bool) t.getProperty ("bundled", false);
    s.ref         = t.getProperty ("ref").toString();
    s.displayName = t.getProperty ("name").toString();
    s.localPath   = t.getProperty ("path").toString();
    s.trim        = (float) t.getProperty ("trim", 1.0f);
    if (s.ref.isEmpty())                       // occupied but no ref is meaningless → empty
        return SlotIR::makeEmpty();
    return s;
}

// --- a full sound <Sound><Params>(apvts copy)</Params><Slot A/><Slot B/></Sound> ---
inline juce::ValueTree toTree (const SoundState& s, bool portable)
{
    juce::ValueTree t ("Sound");
    juce::ValueTree p ("Params");
    if (s.params.isValid())
        p.appendChild (s.params.createCopy(), nullptr);
    t.appendChild (p, nullptr);
    t.appendChild (toTree (s.slots[0], "A", portable), nullptr);
    t.appendChild (toTree (s.slots[1], "B", portable), nullptr);
    return t;
}

inline SoundState soundFromTree (const juce::ValueTree& t)
{
    SoundState s;
    if (! t.isValid())
        return s;
    if (auto p = t.getChildWithName ("Params"); p.isValid() && p.getNumChildren() > 0)
        s.params = p.getChild (0).createCopy();
    for (auto child : t)
        if (child.hasType ("Slot"))
        {
            const auto side = child.getProperty ("side").toString();
            const int  idx  = (side == "B") ? 1 : 0;
            s.slots[(size_t) idx] = slotFromTree (child);
        }
    return s;
}

// --- the workspace <Workspace active=..><Live><Sound/></Live><Snaps><Snap .../>..</Snaps></Workspace> ---
inline juce::ValueTree toTree (const Workspace& w, bool portable)
{
    juce::ValueTree t ("Workspace");
    t.setProperty ("active", juce::jlimit (0, 3, w.active), nullptr);

    juce::ValueTree live ("Live");
    live.appendChild (toTree (w.live, portable), nullptr);
    t.appendChild (live, nullptr);

    juce::ValueTree snaps ("Snaps");
    for (int i = 0; i < 4; ++i)
    {
        juce::ValueTree snap ("Snap");
        snap.setProperty ("i", i, nullptr);
        if (w.snapshots[(size_t) i].has_value())
            snap.appendChild (toTree (*w.snapshots[(size_t) i], portable), nullptr);
        snaps.appendChild (snap, nullptr);
    }
    t.appendChild (snaps, nullptr);
    return t;
}

inline Workspace workspaceFromTree (const juce::ValueTree& t)
{
    Workspace w;
    if (! t.isValid())
        return w;
    w.active = juce::jlimit (0, 3, (int) t.getProperty ("active", 0));
    if (auto live = t.getChildWithName ("Live"); live.isValid() && live.getNumChildren() > 0)
        w.live = soundFromTree (live.getChild (0));
    if (auto snaps = t.getChildWithName ("Snaps"); snaps.isValid())
        for (auto snap : snaps)
        {
            const int i = juce::jlimit (0, 3, (int) snap.getProperty ("i", 0));
            if (snap.getNumChildren() > 0)
                w.snapshots[(size_t) i] = soundFromTree (snap.getChild (0));
        }
    return w;
}

//============================================================ dirty fingerprint =
// A cheap, stable hash of the PRESET-DEFINING part of a sound: params (incl. the
// headTrim property carried inside) + each slot's identity (bundled/ref/name/trim).
// Excludes localPath (a machine-local recovery hint) and the runtime status, so the
// same sound on two machines — or before/after a relink — hashes identically, and
// an empty slot can't smuggle stale trim into the result. Drives the "*" marker.
inline juce::String fingerprint (const SoundState& s)
{
    juce::String key;
    key << (s.params.isValid() ? s.params.toXmlString (juce::XmlElement::TextFormat().singleLine()) : juce::String());
    for (const auto& slot : s.slots)
    {
        key << "|";
        if (! slot.occupied())
            key << "empty";
        else
            key << (slot.bundled ? "B:" : "X:") << slot.ref << ":" << slot.displayName
                << ":" << juce::String (slot.trim, 4);
    }
    return juce::String (key.hashCode64());
}

//============================================================ v3 → v4 migration =
// Build a SoundState from a legacy v3 (or pre-v3 param-only) state. The legacy IR
// node carried aLoaded/aBundled/aRef/aName/aTrim (+ b…); external refs were either
// an absolute path (session) or a content hash "ir-<hex>" (portable preset). v4
// unifies on the content id, so:
//   * bundled            → keep filename ref, name from old aName or the filename.
//   * external hash ref  → keep it as the id; localPath unknown.
//   * external path ref  → the path becomes localPath; the id is recomputed by the
//                          processor from the embedded bytes (it owns the pool), so
//                          here we keep the path as BOTH ref and localPath and let
//                          the processor re-key it. Name from old aName or filename.
// `params` is the already-extracted APVTS tree. Names that were already corrupted to
// a hash in a pre-fix build can't be recovered — best effort, as agreed.
inline SlotIR migrateLegacySlot (const juce::ValueTree& ir, bool slotA)
{
    const juce::String pfx = slotA ? "a" : "b";
    const bool defaultLoaded = slotA;                      // pre-clear A defaulted loaded, B not
    const bool loaded = (bool) ir.getProperty (pfx + "Loaded", defaultLoaded);
    const juce::String ref = ir.getProperty (pfx + "Ref").toString();
    if (! loaded || ref.isEmpty())
        return SlotIR::makeEmpty();

    SlotIR s;
    s.status  = SlotIR::Status::ready;                     // tentative; processor resolve confirms
    s.bundled = (bool) ir.getProperty (pfx + "Bundled", true);
    s.ref     = ref;
    s.trim    = (float) ir.getProperty (pfx + "Trim", 1.0f);

    const juce::String oldName = ir.getProperty (pfx + "Name", {}).toString();
    if (s.bundled)
    {
        s.displayName = oldName.isNotEmpty() ? oldName : ref;
    }
    else if (ref.startsWith ("ir-"))                       // portable content hash
    {
        s.displayName = oldName.isNotEmpty() ? oldName : ref;
    }
    else                                                   // absolute path (session)
    {
        s.localPath   = ref;
        s.displayName = oldName.isNotEmpty() ? oldName : juce::File (ref).getFileName();
    }
    return s;
}

inline SoundState migrateLegacySound (const juce::ValueTree& legacyIR, const juce::ValueTree& params)
{
    SoundState s;
    s.params   = params.isValid() ? params.createCopy() : juce::ValueTree();
    s.slots[0] = legacyIR.isValid() ? migrateLegacySlot (legacyIR, true)  : SlotIR::makeEmpty();
    s.slots[1] = legacyIR.isValid() ? migrateLegacySlot (legacyIR, false) : SlotIR::makeEmpty();
    return s;
}

} // namespace orbitcab::state
