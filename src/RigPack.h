// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_core/juce_core.h>

#include <utility>
#include <vector>

namespace orbitcab
{

//==============================================================================
// orbitcab::readRigPack — read an .orbitrig pack (NAMZ-FORMAT.md "The .orbitrig pack") into
// memory: every .namz as (basename, bytes) + the rig.json manifest text. Accepts BOTH shipping
// shapes — the exchange zip (Device.orbitrig.zip) and the unpacked working folder. Pure juce_core
// (headless-tested, RigPackTests); WHERE the models install (slot routing, the user libraries) is
// the processor's business, not this reader's.
//
// Safety: zip entries are taken by BASENAME only — entry paths never shape the destination
// (zip-slip proof), and any depth of folder nesting inside the zip is accepted. Every model read
// is capped at maxModelBytes (a crafted pack can't OOM the host); oversized/unreadable models are
// counted in `failed` rather than aborting the pack.
//==============================================================================

struct RigPack
{
    std::vector<std::pair<juce::String, juce::MemoryBlock>> models;   // basename → .namz bytes
    juce::String manifestText;                                        // rig.json ("" when absent)
    int failed = 0;                                                   // oversized / unreadable models
};

// Does this file LOOK like an .orbitrig pack at either shipping shape — the exchange zip
// (Device.orbitrig.zip) or the unpacked working folder (rig.json / loose .namz inside)? Cheap
// enough for drag-hover interest checks; readRigPack() is the real read.
inline bool looksLikeRigPack (const juce::File& f)
{
    if (f.hasFileExtension ("zip"))
        return f.getFileName().containsIgnoreCase (".orbitrig");
    return f.isDirectory()
           && (f.getChildFile ("rig.json").existsAsFile()
               || f.getNumberOfChildFiles (juce::File::findFiles, "*.namz") > 0);
}

inline RigPack readRigPack (const juce::File& src, juce::int64 maxModelBytes)
{
    RigPack pack;

    if (src.isDirectory())
    {
        for (const auto& f : src.findChildFiles (juce::File::findFiles, false, "*.namz"))
        {
            juce::MemoryBlock mb;
            if (f.getSize() <= maxModelBytes && f.loadFileAsData (mb))
                pack.models.push_back ({ f.getFileName(), std::move (mb) });
            else
                ++pack.failed;
        }
        pack.manifestText = src.getChildFile ("rig.json").loadFileAsString();
        return pack;
    }

    if (! src.existsAsFile())
        return pack;

    juce::ZipFile zip (src);
    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        const auto* entry = zip.getEntry (i);
        if (entry == nullptr)
            continue;
        const auto base = entry->filename.fromLastOccurrenceOf ("/", false, false);
        const bool isModel    = base.endsWithIgnoreCase (".namz");
        const bool isManifest = base == "rig.json" && pack.manifestText.isEmpty();
        if (! isModel && ! isManifest)
            continue;
        if (entry->uncompressedSize > maxModelBytes)
        {
            if (isModel) ++pack.failed;
            continue;
        }
        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
        juce::MemoryBlock mb;
        if (in == nullptr || in->readIntoMemoryBlock (mb) == 0)
        {
            if (isModel) ++pack.failed;
            continue;
        }
        if (isModel) pack.models.push_back ({ base, std::move (mb) });
        else         pack.manifestText = mb.toString();
    }
    return pack;
}

} // namespace orbitcab
