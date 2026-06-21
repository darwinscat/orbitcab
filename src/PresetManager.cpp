// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "PresetManager.h"

#include <algorithm>

namespace orbitcab
{

juce::File PresetManager::directory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Darwin's Cat").getChildFile ("OrbitCab").getChildFile ("Presets");
}

juce::Array<juce::File> PresetManager::list() const
{
    auto files = directory().findChildFiles (juce::File::findFiles, false, "*.orbitcab");
    std::sort (files.begin(), files.end(),
               [] (const juce::File& a, const juce::File& b)
               { return a.getFileName().compareNatural (b.getFileName()) < 0; });
    return files;
}

juce::Array<PresetManager::PresetEntry> PresetManager::listWithMeta() const
{
    juce::Array<PresetEntry> out;
    for (const auto& file : list())
    {
        PresetEntry e;
        e.file = file;

        // Read <meta> WITHOUT applying state: parse the preset's binary-XML and pull just the
        // <meta> child's JSON (no setStateInformation, no IR decode). Skip an absurdly large
        // file (same guard as loadFrom) so a crafted preset can't OOM the listing.
        juce::MemoryBlock block;
        if (file.getSize() > 0 && file.getSize() <= 256LL * 1024 * 1024 && file.loadFileAsData (block))
            if (auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize()))
                if (auto* m = xml->getChildByName ("meta"))
                    e.meta = orbitcab::PresetMeta::fromVar (orbitcab::parseJSON (m->getStringAttribute ("json")));

        if (e.meta.name.isEmpty())                       // pre-v3 / unreadable → name from filename
            e.meta.name = file.getFileNameWithoutExtension();
        out.add (e);
    }
    return out;
}

juce::File PresetManager::saveAs (const juce::String& name)
{
    auto dir = directory();
    dir.createDirectory();
    auto file = dir.getChildFile (juce::File::createLegalFileName (name) + ".orbitcab");
    proc.setPresetName (name);                       // stamp the preset's <meta> name first
    juce::MemoryBlock block;
    proc.getStateForPreset (block);
    file.replaceWithData (block.getData(), block.getSize());
    // The saved preset is now the current one: a clean, editable (non-factory) user preset.
    proc.setPresetFactory (false);
    proc.captureBaseline();
    return file;
}

bool PresetManager::loadFrom (const juce::File& file)
{
    // Refuse an absurdly large preset outright (a legit one with embedded IRs is tens of MB)
    // so a crafted/corrupt file can't be slurped into memory and OOM the host.
    if (file.getSize() > 256LL * 1024 * 1024)
        return false;
    juce::MemoryBlock block;
    if (! file.loadFileAsData (block))
        return false;
    proc.setStateInformation (block.getData(), (int) block.getSize());
    // A loaded preset file is a clean, editable (non-factory) user preset: re-baseline so
    // it reads as not-dirty, then dirties only when the user edits it. (A portable preset has
    // no embedded baseline; a session is restored by the host directly, not via loadFrom.)
    proc.setPresetFactory (false);
    proc.captureBaseline();
    return true;
}

bool PresetManager::deleteFile (const juce::File& file)
{
    // Move a USER preset to the Trash (recoverable). Guarded: must be a .orbitcab inside our
    // own preset dir, so a stray call can never delete an arbitrary file. Factory presets aren't
    // files (no folder copy), so they can't be reached here — the editor also gates the button.
    if (! file.existsAsFile() || ! file.hasFileExtension ("orbitcab") || ! file.isAChildOf (directory()))
        return false;
    return file.moveToTrash();
}

bool PresetManager::writeTo (juce::File file)
{
    if (file == juce::File())
        return false;
    if (! file.hasFileExtension ("orbitcab"))
        file = file.withFileExtension ("orbitcab");
    juce::MemoryBlock block;
    proc.getStateForPreset (block);     // embeds external IRs + strips paths → portable preset
    return file.replaceWithData (block.getData(), block.getSize());
}

} // namespace orbitcab
