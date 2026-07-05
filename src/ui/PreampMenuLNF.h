// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include "OrbitCabLookAndFeel.h"
#include "DeviceGlyph.h"

#include <functional>
#include <utility>

//==============================================================================
// A LookAndFeel for the preamp NAME combo's popup ONLY: it draws the model's device glyphs
// (N tubes / a PNP / a FET) on the RIGHT of each item, so the dropdown reads what each capture
// is at a glance. The per-item lookup is injected by the editor (`deviceFor`), keyed by item text.
//==============================================================================
namespace orbitcab::ui
{

class PreampMenuLNF : public OrbitCabLookAndFeel
{
public:
    std::function<DeviceSpec (const juce::String&)> deviceFor;   // item text → device spec (may be hybrid)

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                    int standardHeight, int& idealWidth, int& idealHeight) override
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, standardHeight, idealWidth, idealHeight);
        if (! isSeparator && deviceFor)
            if (const int total = deviceSpecCount (deviceFor (text)); total > 0)
                idealWidth += (int) ((float) total * (float) idealHeight * 0.72f) + 16;
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                            const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        juce::LookAndFeel_V4::drawPopupMenuItem (g, area, isSeparator, isActive, isHighlighted, isTicked,
                                                hasSubMenu, text, shortcutKeyText, icon, textColour);
        if (isSeparator || ! deviceFor)
            return;
        const auto spec  = deviceFor (text);
        const int  total = deviceSpecCount (spec);
        if (total <= 0)
            return;
        const float cell = (float) area.getHeight() * 0.72f;
        auto strip = area.toFloat().removeFromRight (cell * (float) total + 12.0f)
                                   .withSizeKeepingCentre (cell * (float) total, cell);
        drawDeviceSpecStatic (g, strip, spec);
    }
};

} // namespace orbitcab::ui
