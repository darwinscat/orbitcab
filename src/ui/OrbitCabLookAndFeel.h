// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// OrbitCab look — dark panels + gold accent, Genome-style. Rotary knobs draw
// their value in the centre. First cut; refine toward the reference over time.
//==============================================================================
class OrbitCabLookAndFeel final : public juce::LookAndFeel_V4
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
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 2.0f;
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
        g.setColour (on ? juce::Colour (kAccent) : juce::Colour (0xff55555a));
        g.strokePath (val, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour (on ? juce::Colour (kText) : juce::Colour (0xff70707a));
        g.setFont (juce::FontOptions (juce::jmax (12.0f, radius * 0.5f), juce::Font::bold));
        g.drawText (s.getTextFromValue (s.getValue()), bounds.toNearestInt(), juce::Justification::centred, false);
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
        juce::LookAndFeel_V4::drawButtonBackground (g, b, bg, highlighted, down);
        if (b.isColourSpecified (accentBorderColourId))
        {
            g.setColour (b.findColour (accentBorderColourId).withAlpha (b.isEnabled() ? 0.85f : 0.4f));
            g.drawRoundedRectangle (b.getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
        }
    }
};
