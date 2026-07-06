// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "DeviceSpec.h"   // DeviceType / DeviceSpec / parse (pure juce_core — the data model, unit-tested)

#include <array>
#include <cmath>

//==============================================================================
// orbitcab::ui — schematic glyphs for a preamp's active device: a triode (tube), a bipolar
// transistor (PNP-style), a FET, a DSP chip, or a diode. Driven by the model metadata "device"
// spec (see DeviceSpec.h) so the UI shows WHAT a capture is — e.g. 4 tubes for a V4, one for a
// Volt, a PNP for the transistor ISA, or a tube+PNP for a hybrid.
//==============================================================================
namespace orbitcab::ui
{

// One symbol inside `r` (a square-ish cell), stroked in `c`. Kept schematic-simple so it reads at ~20 px.
inline void drawDeviceGlyph (juce::Graphics& g, juce::Rectangle<float> r, DeviceType type, juce::Colour c)
{
    if (type == DeviceType::none)
        return;

    const float R  = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
    const auto  ctr = r.getCentre();
    const float cx = ctr.x, cy = ctr.y;
    const float sw = juce::jmax (1.0f, R * 0.11f);   // stroke width
    g.setColour (c);
    const juce::PathStrokeType stroke (sw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    const float er = R * 0.82f;   // envelope radius

    // DSP / IC — a chip package with legs + a pin-1 dot (no valve envelope).
    if (type == DeviceType::dsp)
    {
        const float bw = er * 1.20f, bh = er * 1.00f;
        auto body = juce::Rectangle<float> (bw, bh).withCentre (ctr);
        g.drawRoundedRectangle (body, sw * 1.3f, sw);
        juce::Path legs;
        for (int k = 0; k < 3; ++k)
        {
            const float y = body.getY() + bh * (0.22f + 0.28f * (float) k);
            legs.startNewSubPath (body.getX() - er * 0.30f, y); legs.lineTo (body.getX(), y);
            legs.startNewSubPath (body.getRight(), y);          legs.lineTo (body.getRight() + er * 0.30f, y);
        }
        g.strokePath (legs, stroke);
        const float dot = sw * 1.8f;   // pin-1 marker
        g.fillEllipse (juce::Rectangle<float> (dot, dot).withCentre ({ body.getX() + bw * 0.22f, body.getY() + bh * 0.26f }));
        return;
    }

    // Diode: anode triangle → cathode bar, with leads out each side (no valve envelope).
    if (type == DeviceType::diode)
    {
        const float w = er * 0.86f, h = er * 0.94f;
        const float lx = cx - w * 0.5f, rx = cx + w * 0.5f;
        juce::Path tri;
        tri.startNewSubPath (lx, cy - h * 0.5f);
        tri.lineTo          (rx, cy);
        tri.lineTo          (lx, cy + h * 0.5f);
        tri.closeSubPath();
        g.strokePath (tri, stroke);
        juce::Path leads;
        leads.startNewSubPath (rx, cy - h * 0.5f);        leads.lineTo (rx, cy + h * 0.5f);   // cathode bar
        leads.startNewSubPath (cx - er - R * 0.34f, cy);  leads.lineTo (lx, cy);              // anode lead
        leads.startNewSubPath (rx, cy);                   leads.lineTo (cx + er + R * 0.34f, cy); // cathode lead
        g.strokePath (leads, stroke);
        return;
    }

    // Envelope circle (tube / BJT / FET sit in one, schematic style).
    g.drawEllipse (juce::Rectangle<float> (2 * R * 0.82f, 2 * R * 0.82f).withCentre (ctr), sw);

    juce::Path p;
    if (type == DeviceType::tube)
    {
        // Triode: plate (top bar) + grid (dashed) + cathode (shallow V), with leads out top/left/bottom.
        const float w = er * 0.62f;
        // plate
        p.startNewSubPath (cx - w, cy - er * 0.42f);
        p.lineTo         (cx + w, cy - er * 0.42f);
        p.startNewSubPath (cx, cy - er * 0.42f);   // plate lead up
        p.lineTo         (cx, cy - er - R * 0.34f);
        // cathode (V) + lead down
        p.startNewSubPath (cx - w * 0.7f, cy + er * 0.30f);
        p.lineTo         (cx,             cy + er * 0.52f);
        p.lineTo         (cx + w * 0.7f, cy + er * 0.30f);
        p.startNewSubPath (cx, cy + er * 0.52f);
        p.lineTo         (cx, cy + er + R * 0.34f);
        // grid lead out the left
        p.startNewSubPath (cx - er * 0.9f, cy);
        p.lineTo         (cx - er - R * 0.34f, cy);
        g.strokePath (p, stroke);
        // grid: a short dashed bar (drawn as discrete segments — reliable at any size)
        juce::Path grid;
        const int   nd  = 4;
        const float seg = (2 * w) / (float) (nd * 2 - 1);
        for (int k = 0; k < nd; ++k)
        {
            const float x0 = cx - w + (float) k * 2.0f * seg;
            grid.startNewSubPath (x0, cy);
            grid.lineTo (juce::jmin (x0 + seg, cx + w), cy);
        }
        g.strokePath (grid, stroke);
    }
    else if (type == DeviceType::pnp)
    {
        // BJT, PNP: vertical base bar; base lead left; collector up-right, emitter down-right with the
        // arrow pointing INTO the base (PNP).
        const float bx = cx - er * 0.18f;              // base bar x
        const float bt = cy - er * 0.42f, bb = cy + er * 0.42f;
        p.startNewSubPath (bx, bt); p.lineTo (bx, bb);                 // base bar
        p.startNewSubPath (cx - er - R * 0.34f, cy); p.lineTo (bx, cy); // base lead (left)
        p.startNewSubPath (bx, bt + er * 0.16f); p.lineTo (cx + er * 0.5f, cy - er * 0.62f);   // collector
        p.startNewSubPath (bx, bb - er * 0.16f); p.lineTo (cx + er * 0.5f, cy + er * 0.62f);   // emitter
        g.strokePath (p, stroke);
        // emitter arrow (PNP → points toward the base bar)
        const juce::Point<float> tip (bx + er * 0.10f, bb - er * 0.24f);
        const float a = juce::MathConstants<float>::pi * 0.28f, len = er * 0.34f;
        const float dir = std::atan2 ((bb - er * 0.16f) - (cy + er * 0.62f), bx - (cx + er * 0.5f));
        juce::Path arr;
        arr.startNewSubPath (tip);
        arr.lineTo (tip.x - len * std::cos (dir - a), tip.y - len * std::sin (dir - a));
        arr.startNewSubPath (tip);
        arr.lineTo (tip.x - len * std::cos (dir + a), tip.y - len * std::sin (dir + a));
        g.strokePath (arr, stroke);
    }
    else // fet
    {
        // JFET: vertical channel bar; gate lead from the left; drain (top-right) + source (bottom-right).
        const float chx = cx + er * 0.10f;
        const float ct = cy - er * 0.46f, cb = cy + er * 0.46f;
        p.startNewSubPath (chx, ct); p.lineTo (chx, cb);                                   // channel
        p.startNewSubPath (cx - er - R * 0.34f, cy); p.lineTo (chx - er * 0.02f, cy);      // gate lead
        p.startNewSubPath (chx, ct + er * 0.16f); p.lineTo (chx + er * 0.72f, ct + er * 0.16f); // drain
        p.lineTo          (chx + er * 0.72f, cy - er - R * 0.10f);
        p.startNewSubPath (chx, cb - er * 0.16f); p.lineTo (chx + er * 0.72f, cb - er * 0.16f); // source
        p.lineTo          (chx + er * 0.72f, cy + er + R * 0.10f);
        g.strokePath (p, stroke);
        // gate arrow (points into the channel)
        const juce::Point<float> tip (chx - er * 0.02f, cy);
        juce::Path arr;
        arr.startNewSubPath (tip.x - er * 0.30f, cy - er * 0.20f);
        arr.lineTo (tip);
        arr.lineTo (tip.x - er * 0.30f, cy + er * 0.20f);
        g.strokePath (arr, stroke);
    }
}

// Per-family colours: the glyph stroke and the (behind-glyph) glow. Warm=tube, blue=BJT, green=FET.
inline juce::Colour deviceStroke (DeviceType t)
{
    switch (t) { case DeviceType::tube: return juce::Colour (0xfff2c793);
                 case DeviceType::pnp:  return juce::Colour (0xffc3dbf6);
                 case DeviceType::fet:  return juce::Colour (0xffc3edcf);
                 case DeviceType::dsp:  return juce::Colour (0xfff2b8b4);
                 case DeviceType::diode: return juce::Colour (0xffeef0f4);
                 default: break; }
    return juce::Colour (0xffb8b0a0);
}
inline juce::Colour deviceGlow (DeviceType t)
{
    switch (t) { case DeviceType::tube: return juce::Colour (0xffff9a3c);   // warm amber
                 case DeviceType::pnp:  return juce::Colour (0xff4e9ae8);   // blue (BJT)
                 case DeviceType::fet:  return juce::Colour (0xff58c877);   // green (FET)
                 case DeviceType::dsp:  return juce::Colour (0xffe23b3b);   // red (DSP chip)
                 case DeviceType::diode: return juce::Colour (0xffffffff);  // white (diode)
                 default: break; }
    return juce::Colours::transparentBlack;
}

// Draw a spec as a STATIC row (no glow) — used in the combo popup items. Each glyph is stroked in its
// own family colour, so a hybrid reads as a warm tube next to a blue transistor.
inline void drawDeviceSpecStatic (juce::Graphics& g, juce::Rectangle<float> area, const DeviceSpec& spec)
{
    const int total = deviceSpecCount (spec);
    if (total <= 0)
        return;
    const float cell = juce::jmin (area.getHeight(), area.getWidth() / (float) total);
    auto row = area.withSizeKeepingCentre (cell * (float) total, cell);
    for (const auto& [type, cnt] : spec)
        for (int i = 0; i < cnt; ++i)
            drawDeviceGlyph (g, row.removeFromLeft (cell).reduced (cell * 0.12f), type, deviceStroke (type));
}

// The device glyph strip BELOW the preamp combo: N schematic glyphs, each over a soft flickering
// glow tinted by device family — warm amber (tube), blue (BJT), green (FET). The flicker is the same
// organic one-pole shimmer the poweramp heater tubes use, advanced by the editor's 30 Hz timer.
class DeviceStrip : public juce::Component
{
public:
    DeviceStrip() { setInterceptsMouseClicks (false, false); glowL.fill (0.84f); }

    void set (DeviceSpec s)
    {
        if (s == spec) return;
        spec = std::move (s); repaint();
    }

    // Advance the heater/glow flicker one frame + repaint (editor 30 Hz timer, only while visible).
    void tick()
    {
        if (spec.empty())
            return;
        phase += 1.0f;
        for (size_t i = 0; i < glowL.size(); ++i)
        {
            const float ph      = phase * 0.21f + (float) i * 1.7f;
            const float shimmer = 0.10f * std::sin (ph) + 0.05f * std::sin (ph * 2.63f + 1.0f);
            const float jitter  = (rng.nextFloat() - 0.5f) * 0.05f;
            const float target  = juce::jlimit (0.5f, 1.0f, 0.84f + shimmer + jitter);
            glowL[i] = glowL[i] * 0.7f + target * 0.3f;
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const int total = deviceSpecCount (spec);
        if (total <= 0)
            return;
        auto area = getLocalBounds().toFloat();
        const float cell = juce::jmin (area.getHeight(), area.getWidth() / (float) total);
        auto row = area.withSizeKeepingCentre (cell * (float) total, cell);
        int gi = 0;
        for (const auto& [type, cnt] : spec)
            for (int i = 0; i < cnt; ++i, ++gi)
            {
                auto c = row.removeFromLeft (cell);
                const auto  ctr  = c.getCentre();
                const float rad  = cell * 0.66f;
                const float lvl  = glowL[(size_t) juce::jlimit (0, (int) glowL.size() - 1, gi)];
                const auto  glow = deviceGlow (type);   // per-glyph colour → hybrid glows amber + blue
                juce::ColourGradient grad (glow.withAlpha (0.60f * lvl), ctr.x, ctr.y,
                                           glow.withAlpha (0.0f),        ctr.x + rad, ctr.y, true);
                g.setGradientFill (grad);
                g.fillEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (ctr));
                drawDeviceGlyph (g, c.reduced (cell * 0.14f), type, deviceStroke (type));
            }
    }

private:
    DeviceSpec            spec;
    float                 phase = 0.0f;
    std::array<float, 12> glowL {};
    juce::Random          rng;
};

} // namespace orbitcab::ui
