// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "OrbitCabLookAndFeel.h"
#include "BrandMark.h"          // orbitcab::brand — shared orbit mark + Michroma wordmark

#include <functional>
#include <cmath>

//==============================================================================
// Clickable header brand:
//   [cat logo]  ◎ OrbitCab        by Darwin's Cat
//                                 Cabinet IR Loader
// An orbit mark (concentric violet/lilac/orange rings — a "cabinet on orbit") sits left of
// the "OrbitCab" wordmark; a two-line block follows — "by Darwin's Cat" (accent, the brand
// link) over "Cabinet IR Loader" (dim), the two right-aligned to each other and the bottom
// line sharing the wordmark's baseline. All text in Michroma (embedded). The whole strip
// links to the IR page with a soft accent halo on hover, and sizes itself to its content.
// The orbit mark + wordmark font live in ui/BrandMark.h (shared with the (i) popover).
//==============================================================================
class HeaderBrand final : public juce::Component,
                          public juce::SettableTooltipClient
{
public:
    HeaderBrand() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    juce::Drawable*       logo = nullptr;          // cat mark — not owned (editor holds it)
    juce::Typeface::Ptr   wordmarkTypeface;        // Michroma — set by the editor (BinaryData)
    juce::Colour          accent { 0xff9778ff };
    std::function<void()> onLaunch;

    // Width the strip needs: cat square + orbit mark + "rbitCab" + the wider subtitle line.
    int preferredWidth (int height) const
    {
        const float h = (float) height;
        const float subW = orbitcab::brand::textWidth (font (subHeight (h)), kSub);   // byline right-aligns within this
        return height + kGap
             + (int) std::ceil (markDiameter (h) + kMarkGap + orbitcab::brand::textWidth (font (wordHeight (h)), kWord))
             + kSubGap + (int) std::ceil (subW) + kPadR;
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseUp    (const juce::MouseEvent& e) override
    {
        if (onLaunch && getLocalBounds().contains (e.getPosition()))
            onLaunch();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float h = (float) getHeight();

        if (hover)                                  // soft accent halo (ореол)
        {
            g.setColour (accent.withAlpha (0.16f));
            g.fillRoundedRectangle (b, 8.0f);
        }

        // cat logo (square at the strip height)
        auto logoArea = b.removeFromLeft (h);
        if (logo != nullptr)
            logo->drawWithin (g, logoArea.reduced (5.0f), juce::RectanglePlacement::centred, 1.0f);
        b.removeFromLeft ((float) kGap);

        // orbit mark = the "O" of OrbitCab (~ the cat-logo height)
        const float markD = markDiameter (h);
        const float cy    = b.getCentreY();
        orbitcab::brand::drawOrbit (g, b.getX() + markD * 0.5f, cy, markD, hover);
        float x = b.getX() + markD + (float) kMarkGap;   // pen x after the orbit

        // Shared baseline so "rbitCab" and "Cabinet IR Loader" sit on the SAME bottom line
        // (bottom-aligned, not centred). Baseline placed so the big wordmark is centred.
        const auto wf = font (wordHeight (h));
        const float baseline = cy + (wf.getAscent() - wf.getDescent()) * 0.5f;

        // "rbitCab" wordmark (Michroma, large)
        g.setFont (wf);
        g.setColour (hover ? juce::Colours::white : juce::Colour (0xffeef0f6));
        g.drawSingleLineText (kWord, juce::roundToInt (x), juce::roundToInt (baseline));
        x += orbitcab::brand::textWidth (wf, kWord) + (float) kSubGap;

        // Two stacked lines right of the wordmark:
        //   "by Darwin's Cat"   (accent, brand link — top)
        //   "Cabinet IR Loader" (dim grey — bottom, SAME baseline as "rbitCab")
        const auto sf = font (subHeight (h));      // bottom line
        const auto bf = font (bylineHeight (h));   // top line (byline)
        const float subW = orbitcab::brand::textWidth (sf, kSub);
        const float bylW = orbitcab::brand::textWidth (bf, kByline);

        // "by Darwin's Cat" — right-aligned to the subtitle's right edge (so the "Cat" end
        // lines up with the "Loader" end), and a bit higher than the bottom line.
        g.setFont (bf);
        g.setColour (hover ? accent.brighter (0.30f) : accent);
        g.drawSingleLineText (kByline, juce::roundToInt (x + subW - bylW),
                              juce::roundToInt (baseline - sf.getHeight() * 0.96f));

        // "Cabinet IR Loader" — bottom line, SAME baseline as "rbitCab"
        g.setFont (sf);
        g.setColour (hover ? juce::Colour (0xffc8c8d2) : juce::Colour (0xff8a8a94));
        g.drawSingleLineText (kSub, juce::roundToInt (x), juce::roundToInt (baseline));
    }

private:
    // Sizes scale with the strip height: orbit ≈ the cat-logo size, wordmark large,
    // subtitle ~half the wordmark. (orbit / wordHeight ≈ 1.5, matching the SVG.)
    static float markDiameter (float h) { return h * 0.78f; }
    static float wordHeight   (float h) { return h * 0.58f; }    // "rbitCab"
    static float subHeight    (float h) { return h * 0.30f; }    // "Cabinet IR Loader" (bottom line)
    static float bylineHeight (float h) { return h * 0.21f; }    // "by Darwin's Cat" (top line, a bit smaller)

    juce::Font font (float height) const { return orbitcab::brand::wordmarkFont (wordmarkTypeface, height); }

    bool hover = false;
    static constexpr int kGap = 8, kMarkGap = 7, kSubGap = 16, kPadR = 12;
    const juce::String kWord   = "OrbitCab";           // full word; orbit mark sits left as an icon
    const juce::String kSub    = "Cabinet IR Loader";  // loads cabinet IRs (correct order)
    const juce::String kByline = juce::String::fromUTF8 ("by Darwin\xe2\x80\x99s Cat");

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBrand)
};
