// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

//==============================================================================
// Vector icon button (no font glyphs — they don't render in the plugin font).
// Stroked marks, scaled from a 24×24 design box, with a hover wash:
//   • exportFile — arrow up out of a tray            • undo — counter-clockwise arrow
//   • importFile — arrow down into a tray            • redo — clockwise arrow
//==============================================================================
class IconButton final : public juce::Button
{
public:
    enum class Kind { exportFile, importFile, undo, redo };

    explicit IconButton (Kind k) : juce::Button ("icon"), kind (k)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    juce::Colour colour { 0xffc0c0c8 };
    bool framed = false;        // draw an A/B/C/D-style panel + neutral border (dims when disabled)

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat();

        if (framed)             // match the A/B/C/D snapshot buttons (panel fill + neutral border)
        {
            const auto rr = r.reduced (0.5f);
            g.setColour (juce::Colour (0xff1b1b1f));        // kPanel
            g.fillRoundedRectangle (rr, 4.0f);
            if (over || down)
            {
                g.setColour (juce::Colour (over ? 0x1effffff : 0x14ffffff));
                g.fillRoundedRectangle (rr, 4.0f);
            }
            g.setColour (colour.withAlpha (isEnabled() ? 0.85f : 0.4f));   // neutral border — dims when disabled
            g.drawRoundedRectangle (rr, 4.0f, 1.0f);
        }
        else if (over || down)                              // subtle hover/press wash
        {
            g.setColour (juce::Colour (over ? 0x1effffff : 0x14ffffff));
            g.fillRoundedRectangle (r.reduced (1.0f), 4.0f);
        }

        const float pad = juce::jmin (r.getWidth(), r.getHeight()) * (framed ? 0.28f : 0.20f);
        const auto  box = r.reduced (pad);

        auto t = juce::RectanglePlacement (juce::RectanglePlacement::centred)
                     .getTransformToFit (juce::Rectangle<float> (0.0f, 0.0f, 24.0f, 24.0f), box);
        if (kind == Kind::undo)                             // mirror the (clockwise) arrow
            t = juce::AffineTransform::scale (-1.0f, 1.0f).translated (24.0f, 0.0f).followedBy (t);

        g.setColour (colour.withMultipliedAlpha (isEnabled() ? (over ? 1.0f : 0.82f) : 0.35f));

        const juce::PathStrokeType stroke (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        if (kind == Kind::undo || kind == Kind::redo)
        {
            juce::Path arc, head;
            buildCircularArrow (arc, head);
            g.strokePath (arc, stroke, t);                  // the ring
            g.fillPath   (head, t);                         // solid triangular tip
        }
        else
        {
            juce::Path p;
            buildTrayArrow (p, kind == Kind::exportFile);
            g.strokePath (p, stroke, t);
        }
    }

private:
    static void buildTrayArrow (juce::Path& p, bool up)
    {
        // open-top tray (box bottom + sides)
        p.startNewSubPath (5.0f, 14.0f); p.lineTo (5.0f, 21.0f);
        p.lineTo (19.0f, 21.0f);         p.lineTo (19.0f, 14.0f);
        if (up)                                             // arrow up + head
        {
            p.startNewSubPath (12.0f, 16.0f); p.lineTo (12.0f, 4.0f);
            p.startNewSubPath (8.0f, 8.0f);   p.lineTo (12.0f, 4.0f); p.lineTo (16.0f, 8.0f);
        }
        else                                                // arrow down + head
        {
            p.startNewSubPath (12.0f, 4.0f);  p.lineTo (12.0f, 16.0f);
            p.startNewSubPath (8.0f, 12.0f);  p.lineTo (12.0f, 16.0f); p.lineTo (16.0f, 12.0f);
        }
    }

    static void buildCircularArrow (juce::Path& arc, juce::Path& head)
    {
        // ~285° clockwise ring + a solid triangular tip at the leading (end) point.
        const float cx = 12.0f, cy = 12.0f, r = 6.6f;
        const float a0 = juce::degreesToRadians (60.0f);
        const float a1 = juce::degreesToRadians (345.0f);
        arc.addCentredArc (cx, cy, r, r, 0.0f, a0, a1, true);

        // angle convention: clockwise from 12 o'clock → pos (sin,-cos), cw tangent (cos,sin)
        const juce::Point<float> e   (cx + r * std::sin (a1), cy - r * std::cos (a1));
        const juce::Point<float> tan (std::cos (a1), std::sin (a1));
        const juce::Point<float> nrm (-tan.y, tan.x);
        const float hl = 6.0f, hw = 3.6f;
        const auto  tip  = e + tan * (hl * 0.55f);
        const auto  back = e - tan * (hl * 0.45f);
        head.startNewSubPath (tip);
        head.lineTo (back + nrm * hw);
        head.lineTo (back - nrm * hw);
        head.closeSubPath();
    }

    Kind kind;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};
