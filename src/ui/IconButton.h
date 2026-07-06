// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

//==============================================================================
// Vector icon button (no font glyphs — they don't render in the plugin font).
// Stroked marks, scaled from a 24×24 design box, with a hover wash:
//   • exportFile — arrow up out of a tray            • undo — counter-clockwise arrow
//   • importFile — arrow down into a tray            • redo — clockwise arrow
//   • settings   — a toothed gear (filled, hole punched via even-odd winding)
//==============================================================================
class IconButton final : public juce::Button
{
public:
    enum class Kind { exportFile, importFile, undo, redo, settings, trash, save, saveAs };

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
        else if (kind == Kind::settings)
        {
            juce::Path gear;
            buildGear (gear);
            g.fillPath (gear, t);                           // filled cog, centre hole punched
        }
        else if (kind == Kind::trash)
        {
            juce::Path p;
            buildTrash (p);
            g.strokePath (p, stroke, t);
        }
        else if (kind == Kind::save || kind == Kind::saveAs)
        {
            juce::Path p;
            buildFloppy (p, kind == Kind::saveAs);          // floppy; saveAs adds a "+" (new copy)
            g.strokePath (p, stroke, t);
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

    static void buildTrash (juce::Path& p)
    {
        // Lid line + small handle, a tapered open-top bin, and three ribs.
        p.startNewSubPath (4.0f, 7.0f);  p.lineTo (20.0f, 7.0f);                 // lid
        p.startNewSubPath (9.5f, 7.0f);  p.lineTo (9.5f, 4.5f);
        p.lineTo (14.5f, 4.5f);          p.lineTo (14.5f, 7.0f);                 // handle
        p.startNewSubPath (6.5f, 7.0f);  p.lineTo (7.6f, 21.0f);
        p.lineTo (16.4f, 21.0f);         p.lineTo (17.5f, 7.0f);                 // bin body (tapered)
        p.startNewSubPath (10.0f, 10.0f); p.lineTo (10.0f, 18.0f);              // ribs
        p.startNewSubPath (12.0f, 10.0f); p.lineTo (12.0f, 18.0f);
        p.startNewSubPath (14.0f, 10.0f); p.lineTo (14.0f, 18.0f);
    }

    static void buildFloppy (juce::Path& p, bool plus)
    {
        // Floppy-disk "save": body with a cut top-right corner + a shutter slot up top. The
        // plain save adds a label panel below; saveAs replaces it with a "+" (save a new copy).
        p.startNewSubPath (4.0f, 4.0f);  p.lineTo (15.0f, 4.0f); p.lineTo (20.0f, 9.0f);
        p.lineTo (20.0f, 20.0f);         p.lineTo (4.0f, 20.0f); p.closeSubPath();                    // body
        p.startNewSubPath (8.0f, 4.0f);  p.lineTo (8.0f, 8.5f);  p.lineTo (14.0f, 8.5f); p.lineTo (14.0f, 4.0f);  // shutter
        if (plus)
        {
            p.startNewSubPath (12.0f, 12.5f); p.lineTo (12.0f, 18.5f);                                // +
            p.startNewSubPath (9.0f, 15.5f);  p.lineTo (15.0f, 15.5f);
        }
        else
        {
            p.startNewSubPath (7.0f, 20.0f); p.lineTo (7.0f, 13.0f);
            p.lineTo (17.0f, 13.0f);         p.lineTo (17.0f, 20.0f);                                 // label panel
        }
    }

    static void buildGear (juce::Path& p)
    {
        // Toothed ring around the 24×24 box centre: alternate tip/valley radius for the
        // teeth, then punch the centre hole with an inner circle (even-odd winding).
        const float cx = 12.0f, cy = 12.0f;
        const int   teeth = 8;
        const float rTip = 10.5f, rValley = 8.0f, rHole = 3.7f;
        const int   steps = teeth * 2;
        for (int i = 0; i < steps; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) steps;
            const float r = (i % 2 == 0) ? rTip : rValley;
            const float x = cx + r * std::cos (a);
            const float y = cy + r * std::sin (a);
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        p.closeSubPath();
        p.setUsingNonZeroWinding (false);                   // even-odd → inner circle = hole
        p.addEllipse (cx - rHole, cy - rHole, rHole * 2.0f, rHole * 2.0f);
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
