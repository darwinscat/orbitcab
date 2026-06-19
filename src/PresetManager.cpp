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

juce::File PresetManager::saveAs (const juce::String& name)
{
    auto dir = directory();
    dir.createDirectory();
    auto file = dir.getChildFile (juce::File::createLegalFileName (name) + ".orbitcab");
    juce::MemoryBlock block;
    proc.getStateForPreset (block);
    file.replaceWithData (block.getData(), block.getSize());
    return file;
}

bool PresetManager::loadFrom (const juce::File& file)
{
    juce::MemoryBlock block;
    if (! file.loadFileAsData (block))
        return false;
    proc.setStateInformation (block.getData(), (int) block.getSize());
    return true;
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
