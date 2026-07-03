// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// orbitcab::brand — the shared OrbitCab brand mark primitives: the orbit "target" (concentric
// violet/lilac/orange rings — "a cabinet on orbit") and the Michroma wordmark font. One source of
// truth so the editor header (HeaderBrand) and the version/(i) popover render an IDENTICAL mark.
// Pure drawing helpers — no assets of their own; the caller supplies the embedded typeface.
//==============================================================================
namespace orbitcab::brand
{
    // The orbit mark, centred at (cx, cy), diameter d. Stroke widths scale from the d=40 SVG design.
    inline void drawOrbit (juce::Graphics& g, float cx, float cy, float d, bool hover = false)
    {
        const float r = d * 0.5f;
        const float s = d / 40.0f;
        g.setColour (juce::Colour (0xff0b0b11));                                       // dark planet body
        g.fillEllipse (cx - r, cy - r, d, d);
        g.setColour (hover ? juce::Colour (0xff9778ff).brighter (0.2f) : juce::Colour (0xff9778ff));
        g.drawEllipse (cx - r, cy - r, d, d, 2.0f * s);                                // violet outer ring
        const float r2 = r * (13.0f / 20.0f);
        g.setColour (juce::Colour (0xffb9a6ff));                                       // lilac middle ring
        g.drawEllipse (cx - r2, cy - r2, r2 * 2.0f, r2 * 2.0f, 2.0f * s);
        const float r3 = r * (6.0f / 20.0f);
        g.setColour (juce::Colour (0xffff8a3d));                                       // orange inner ring
        g.drawEllipse (cx - r3, cy - r3, r3 * 2.0f, r3 * 2.0f, 2.5f * s);
    }

    // The wordmark font — embedded Michroma (OFL) via the supplied typeface; bold system fallback if null.
    inline juce::Font wordmarkFont (juce::Typeface::Ptr typeface, float height)
    {
        if (typeface != nullptr)
            return juce::Font (juce::FontOptions().withHeight (height).withTypeface (typeface));
        return juce::Font (juce::FontOptions (height, juce::Font::bold));
    }

    // Rendered width of `s` in font `f` (for laying the wordmark + trailing text out by hand).
    inline float textWidth (const juce::Font& f, const juce::String& s)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, s, 0.0f, 0.0f);
        return ga.getBoundingBox (0, -1, true).getWidth();
    }
}
