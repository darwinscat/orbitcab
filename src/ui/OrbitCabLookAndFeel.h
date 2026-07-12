// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// OrbitCab look — dark panels + gold accent, Genome-style. Rotary knobs draw
// their value in the centre. First cut; refine toward the reference over time.
//==============================================================================
class OrbitCabLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Brand palette from darwinscat.com: violet accent, not the old gold.
    static constexpr juce::uint32 kBg          = 0xff0e0e10;
    static constexpr juce::uint32 kPanel       = 0xff1b1b1f;
    static constexpr juce::uint32 kAccent      = 0xff7c4dff;   // DC brand violet (Slot A / left)
    static constexpr juce::uint32 kAccentHover = 0xff9778ff;   // lifted (~+22% white)
    static constexpr juce::uint32 kAccentB     = 0xffff8822;   // orange accent (Slot B / right)
    static constexpr juce::uint32 kNeutral     = 0xffc0c0c8;   // global faders (no side colour)
    static constexpr juce::uint32 kText        = 0xffd8d8d8;
    static constexpr juce::uint32 kTrack       = 0xff333338;

    // custom colour id: when set on a TextButton, its border is drawn in this colour
    // (per-side accent on the browser buttons).
    static constexpr int accentBorderColourId = 0x10b1f00;

    OrbitCabLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (kBg));
        setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour (kAccent));
        setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (kTrack));
        setColour (juce::Slider::thumbColourId,               juce::Colour (kAccent));
        setColour (juce::Slider::trackColourId,               juce::Colour (kAccent));
        setColour (juce::Slider::backgroundColourId,          juce::Colour (0xff2a2a2e));
        setColour (juce::Slider::textBoxTextColourId,         juce::Colour (kText));
        setColour (juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId,                 juce::Colour (kText));
        setColour (juce::ToggleButton::textColourId,          juce::Colour (kText));
        setColour (juce::ToggleButton::tickColourId,          juce::Colour (kAccent));
        setColour (juce::ComboBox::backgroundColourId,        juce::Colour (kPanel));
        setColour (juce::ComboBox::textColourId,              juce::Colour (kText));
        setColour (juce::TextButton::buttonColourId,          juce::Colour (kPanel));
        setColour (juce::TextButton::textColourOffId,         juce::Colour (kText));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                           float pos, float startAngle, float endAngle,
                           juce::Slider& s) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (6.0f);
        const bool  clockTicks = s.getProperties().getWithDefault ("clockTicks", false);   // gain dial → clock marks
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 2.0f - (clockTicks ? 5.0f : 0.0f);
        const auto  centre = bounds.getCentre();
        const float angle  = startAngle + pos * (endAngle - startAngle);
        const float thick  = juce::jmax (3.0f, radius * 0.18f);
        const bool  on     = s.isEnabled();

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
        g.setColour (juce::Colour (kTrack));
        g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path val;
        val.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, angle, true);
        // Honour the slider's OWN fill colour (violet EQ knobs, orange gain dial) instead of a hardcoded accent.
        g.setColour (on ? s.findColour (juce::Slider::rotarySliderFillColourId) : juce::Colour (0xff55555a));
        g.strokePath (val, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Centre text: the UNIT when the slider carries a "unit" property (the EQ knobs — their NUMBER lives
        // in the text box below); otherwise the value itself when there's no text box (the gain dial → "13h").
        const auto unitProp = s.getProperties().getWithDefault ("unit", juce::var()).toString();
        const juce::String centreText = unitProp.isNotEmpty()
                                       ? unitProp
                                       : (s.getTextBoxPosition() == juce::Slider::NoTextBox ? s.getTextFromValue (s.getValue())
                                                                                            : juce::String());
        if (centreText.isNotEmpty())
        {
            g.setColour (on ? juce::Colour (kText) : juce::Colour (0xff70707a));
            g.setFont (juce::FontOptions (juce::jmax (11.0f, radius * 0.46f), juce::Font::bold));
            g.drawText (centreText, bounds.toNearestInt(), juce::Justification::centred, false);
        }

        // Centre GLYPH: a big bold letter drawn in the dial centre when the slider carries a "glyph"
        // property (the reverb TYPE knob → "R"). Larger than the unit text (fills the dial as a mark).
        const auto glyph = s.getProperties().getWithDefault ("glyph", juce::var()).toString();
        if (glyph.isNotEmpty())
        {
            g.setColour (on ? juce::Colour (kText) : juce::Colour (0xff70707a));
            const float gf = radius * (glyph.length() <= 1 ? 0.98f : 0.52f);   // single letter fills; a word ("REV") shrinks to fit
            g.setFont (juce::FontOptions (juce::jmax (10.0f, gf), juce::Font::bold));
            g.drawText (glyph, bounds.toNearestInt(), juce::Justification::centred, false);
        }

        // Clock-style ticks around the dial — one per discrete stop (the gain dial's 7h…17h positions),
        // lit up to the current value in the dial's own colour, the rest dim. Gated by the "clockTicks" prop.
        if (clockTicks)
        {
            const int   stops   = juce::jmax (2, juce::roundToInt (s.getMaximum() - s.getMinimum()) + 1);
            const int   litUpTo = juce::roundToInt (pos * (float) (stops - 1));
            const float r0      = radius + thick * 0.5f + 1.5f;
            const float r1      = r0 + 3.5f;
            const auto  litCol  = on ? s.findColour (juce::Slider::rotarySliderFillColourId) : juce::Colour (0xff55555a);
            for (int i = 0; i < stops; ++i)
            {
                const float a   = startAngle + (float) i / (float) (stops - 1) * (endAngle - startAngle);
                const bool  lit = i <= litUpTo;
                g.setColour (lit ? litCol : juce::Colour (0xff48484e));
                g.drawLine (centre.x + std::sin (a) * r0, centre.y - std::cos (a) * r0,
                            centre.x + std::sin (a) * r1, centre.y - std::cos (a) * r1, lit ? 1.8f : 1.2f);
            }
        }
    }

    // Checkbox: accent-bordered box, filled inner square when ticked. The
    // accent comes from the toggle's tickColourId (set per slot).
    void drawTickBox (juce::Graphics& g, juce::Component& comp, float x, float y, float w, float h,
                      bool ticked, bool isEnabled,
                      bool /*highlighted*/, bool /*down*/) override
    {
        const auto accent = comp.findColour (juce::ToggleButton::tickColourId);
        const juce::Rectangle<float> box (x, y, w, h);
        g.setColour (juce::Colour (0xff202027));
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (accent.withAlpha (isEnabled ? 0.9f : 0.4f));
        g.drawRoundedRectangle (box.reduced (0.5f), 3.0f, 1.2f);
        if (ticked)
        {
            g.setColour (accent.withAlpha (isEnabled ? 1.0f : 0.5f));
            g.fillRoundedRectangle (box.reduced (w * 0.27f), 2.0f);
        }
    }

    // TextButton: keep the base look, add an accent border when accentBorderColourId is
    // set on the button (the slot browser buttons).
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                               bool highlighted, bool down) override
    {
        // Coloured radio / mode buttons (CLEAN / CRUNCH / HI-GAIN / BOOST / tube types / PP-SE …) carry their
        // accent in buttonOnColourId. SELECTED = filled with that accent; UNSELECTED = NO background, just a
        // coloured border (a touch thicker) — per Oleh, replacing the old dim-fill-when-unselected style.
        // Plain buttons (no on-colour) keep the stock look.
        //
        // Opt-out ("orbitStockToggle"): two-fill toggles whose OFF state is the bright/active one — the I/II
        // mute badge (off = playing/accent, on = muted/dark) and the A/B/C/D snapshots — must keep the stock
        // fill-in-both-states look; the border style would blank the active button (its border would be drawn
        // in the *dark* buttonOnColourId). Those buttons set this flag so they fall through to the stock path.
        const bool stockToggle = (bool) b.getProperties().getWithDefault ("orbitStockToggle", false);
        if (b.isColourSpecified (juce::TextButton::buttonOnColourId) && ! stockToggle)
        {
            const auto  bounds = b.getLocalBounds().toFloat().reduced (1.0f);
            const auto  accent = b.findColour (juce::TextButton::buttonOnColourId);
            const bool  on     = b.getToggleState();
            const float a      = b.isEnabled() ? 1.0f : 0.4f;
            if (on)
            {
                g.setColour (accent.withAlpha (a * (down ? 0.85f : 1.0f)));
                g.fillRoundedRectangle (bounds, 4.0f);
            }
            else
            {
                if (highlighted) { g.setColour (accent.withAlpha (a * 0.14f)); g.fillRoundedRectangle (bounds, 4.0f); }   // faint hover
                g.setColour (accent.withAlpha (a * (highlighted ? 1.0f : 0.75f)));
                g.drawRoundedRectangle (bounds, 4.0f, 2.0f);   // coloured border, a bit thicker
            }
        }
        else
        {
            juce::LookAndFeel_V4::drawButtonBackground (g, b, bg, highlighted, down);
        }

        // "Modified since you dialed it in" marker (A/B/C/D snapshots): a small warm dot, top-right.
        if ((bool) b.getProperties().getWithDefault ("orbitDirty", false))
        {
            const auto r = b.getLocalBounds().toFloat();
            const float d = 4.0f;
            g.setColour (juce::Colour (0xffff8a3d).withAlpha (b.isEnabled() ? 0.95f : 0.4f));   // family orange
            g.fillEllipse (r.getRight() - d - 3.0f, r.getY() + 3.0f, d, d);
        }

        if (b.isColourSpecified (accentBorderColourId))
        {
            g.setColour (b.findColour (accentBorderColourId).withAlpha (b.isEnabled() ? 0.85f : 0.4f));
            g.drawRoundedRectangle (b.getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
        }
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrbitCabLookAndFeel)
};
