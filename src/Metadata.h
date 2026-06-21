// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_core/juce_core.h>   // var, DynamicObject, JSON, Time, String, StringArray
#include <vector>

//==============================================================================
// orbitcab::Metadata — the versioned, forward-compatible model behind a preset's
// descriptive identity. The canonical representation is a JSON-shaped juce::var, so a
// preset embeds it as a <meta> child of OrbitCabState (see PluginProcessor) and:
//   * unknown keys ride along in `extra` and are re-emitted on round-trip — a preset
//     authored in a NEWER build, opened in an older one, never loses fields/tags;
//   * `schemaVersion` is stamped from day one (a read-back higher number is preserved,
//     not clamped, so the newer build still recognises its own preset).
//
// This layer is DESCRIPTIVE only — the browser/menu renders it without decoding audio
// or applying DSP state. The FUNCTIONAL IR load stays in the processor's <IR> node +
// embedded-bytes pool (the single source of truth); see IrRef below.
//
// Pure data — depends on juce_core only (no audio, no GUI, no host), so it builds into
// the headless test target and is trivially unit-testable.
//==============================================================================
namespace orbitcab
{

// Schema version for the preset <meta> block. Bump only when a KNOWN key changes meaning
// (additive fields don't need it — they survive via `extra` on older builds). A preset
// carrying a HIGHER number is preserved as-is on round-trip, never downgraded.
inline constexpr int kPresetSchemaVersion = 1;

//================================================================ helpers ====
namespace detail
{
    inline juce::StringArray toStringArray (const juce::var& v)
    {
        juce::StringArray out;
        if (auto* a = v.getArray())
            for (auto& e : *a)
                out.add (e.toString());
        return out;
    }

    inline juce::var fromStringArray (const juce::StringArray& sa)
    {
        juce::Array<juce::var> a;
        for (auto& s : sa)
            a.add (s);
        return juce::var (a);
    }

    inline void setIfNotEmpty (juce::DynamicObject& o, const char* key, const juce::String& s)
    {
        if (s.isNotEmpty())
            o.setProperty (key, s);
    }

    // Re-emit preserved unknown keys onto `o` (none collide with known keys, which are
    // written first). This is the forward-compat round-trip: read in fromVar → extra,
    // written back out here untouched.
    inline void mergeExtra (juce::DynamicObject& o, const juce::var& extra)
    {
        if (auto* e = extra.getDynamicObject())
            for (const auto& p : e->getProperties())
                o.setProperty (p.name, p.value);
    }
}

//=========================================================== preset's IR ref ==
// A preset's DESCRIPTIVE reference to one slot's IR — the browser shows these (which IR
// each slot uses) without decoding audio or applying state. The functional load still
// goes through the processor's own <IR> node + embedded-bytes pool, so this carries no
// functional data (no trim, no params): it's a thin denormalised cache, a *reference*
// not a copy. `id` (content hash) is RESERVED empty here and filled when IR hashing /
// the library land; resolve order becomes id → fallback → embedded, with the id branch
// dormant until then.
struct IrRef
{
    juce::String slot;        // "A" / "B"
    juce::String id;          // content hash — reserved empty (filled by the IR-hashing milestone)
    juce::String name;        // thin denormalised display name (offline-friendly)
    bool         bundled = false;
    juce::String fallback;    // bundled filename OR absolute file path (portable export strips the path)
    juce::var    extra;       // preserved unknown keys

    static IrRef fromVar (const juce::var& v)
    {
        IrRef r;
        auto* o = v.getDynamicObject();
        if (o == nullptr)
            return r;
        juce::DynamicObject::Ptr extra = new juce::DynamicObject();
        for (const auto& p : o->getProperties())
        {
            const juce::String  k   = p.name.toString();
            const juce::var&    val = p.value;
            if      (k == "slot")     r.slot     = val.toString();
            else if (k == "id")       r.id       = val.toString();
            else if (k == "name")     r.name     = val.toString();
            else if (k == "bundled")  r.bundled  = (bool) val;
            else if (k == "fallback") r.fallback = val.toString();
            else                      extra->setProperty (p.name, val);   // preserve
        }
        if (extra->getProperties().size() > 0)
            r.extra = juce::var (extra.get());
        return r;
    }

    juce::var toVar() const
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        detail::setIfNotEmpty (*o, "slot", slot);
        detail::setIfNotEmpty (*o, "id", id);
        detail::setIfNotEmpty (*o, "name", name);
        o->setProperty ("bundled", bundled);
        detail::setIfNotEmpty (*o, "fallback", fallback);
        detail::mergeExtra (*o, extra);
        return juce::var (o.get());
    }
};

//=============================================================== preset meta ==
// The descriptive identity embedded as a <meta> JSON child of OrbitCabState. Carries the
// preset name (the preset-centric model's current-preset name), provenance, and the
// display-layer IR refs. Unknown keys round-trip via `extra`.
struct PresetMeta
{
    int               schemaVersion = kPresetSchemaVersion;
    juce::String      name, description, author;
    juce::StringArray tags;
    std::vector<IrRef> irRefs;
    juce::String      createdAt, modifiedAt, appVersion;
    juce::var         extra;

    static PresetMeta fromVar (const juce::var& v)
    {
        PresetMeta m;
        auto* o = v.getDynamicObject();
        if (o == nullptr)
            return m;
        juce::DynamicObject::Ptr extra = new juce::DynamicObject();
        for (const auto& p : o->getProperties())
        {
            const juce::String  k   = p.name.toString();
            const juce::var&    val = p.value;
            if      (k == "schemaVersion") m.schemaVersion = (int) val;
            else if (k == "name")          m.name = val.toString();
            else if (k == "description")   m.description = val.toString();
            else if (k == "author")        m.author = val.toString();
            else if (k == "tags")          m.tags = detail::toStringArray (val);
            else if (k == "irRefs")
            {
                if (auto* a = val.getArray())
                    for (auto& e : *a)
                        m.irRefs.push_back (IrRef::fromVar (e));
            }
            else if (k == "createdAt")     m.createdAt = val.toString();
            else if (k == "modifiedAt")    m.modifiedAt = val.toString();
            else if (k == "appVersion")    m.appVersion = val.toString();
            else                           extra->setProperty (p.name, val);   // preserve
        }
        if (extra->getProperties().size() > 0)
            m.extra = juce::var (extra.get());
        return m;
    }

    juce::var toVar() const
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("schemaVersion", schemaVersion);   // always emit (read-back number preserved)
        detail::setIfNotEmpty (*o, "name", name);
        detail::setIfNotEmpty (*o, "description", description);
        detail::setIfNotEmpty (*o, "author", author);
        if (! tags.isEmpty())  o->setProperty ("tags", detail::fromStringArray (tags));
        if (! irRefs.empty())
        {
            juce::Array<juce::var> a;
            for (const auto& r : irRefs)
                a.add (r.toVar());
            o->setProperty ("irRefs", juce::var (a));
        }
        detail::setIfNotEmpty (*o, "createdAt", createdAt);
        detail::setIfNotEmpty (*o, "modifiedAt", modifiedAt);
        detail::setIfNotEmpty (*o, "appVersion", appVersion);
        detail::mergeExtra (*o, extra);
        return juce::var (o.get());
    }
};

//================================================================ JSON glue ===
inline juce::String toJSON    (const juce::var& v)    { return juce::JSON::toString (v); }
inline juce::var    parseJSON (const juce::String& s) { return juce::JSON::parse (s); }
inline juce::String nowIso8601()                      { return juce::Time::getCurrentTime().toISO8601 (true); }

} // namespace orbitcab
