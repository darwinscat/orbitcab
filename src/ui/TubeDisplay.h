// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "OrbitCabLookAndFeel.h"

#include <cmath>

//==============================================================================
// TubeDisplay — a small schematic amplifier symbol feeding 1 or 2 power tubes with a warm, gently-
// flickering "heater" glow, for the revealed POWERAMP row. The tubes use the cleaner *stylised* v1
// silhouettes (6L6 coke-bottle, EL34 straight, EL84 slim, KT88 fat) — they read better at UI size
// than photo-literal ones. Active selection glows amber; tick() (the editor's 30 Hz timer) drives
// an organic flicker. Pure view — no audio, no state.
//==============================================================================
class TubeDisplay final : public juce::Component
{
public:
    TubeDisplay() { setInterceptsMouseClicks (false, false); }

    static constexpr int kMaxTubes = 8;   // glow buffer / drawn-tube cap (octal quads + headroom)

    void setSelection (int tubeIndex, int count, bool glowing)   // tube silhouette 0..3, count 0..N, glowing = amp on
    {
        tube = juce::jlimit (0, 3, tubeIndex);
        n    = juce::jlimit (0, kMaxTubes, count);   // 0 = "other": amp icon only, no tubes
        glow = glowing;
    }

    void setShowTubes (bool b) { showTubes = b; }   // off = keep the amp icon, hide the tubes

    // Advance the flicker one frame + repaint. Driven by the editor's 30 Hz timer while visible.
    void tick()
    {
        phase += 1.0f;
        for (int i = 0; i < kMaxTubes; ++i)
        {
            const float ph = phase * 0.21f + (float) i * 1.7f;
            const float shimmer = 0.10f * std::sin (ph) + 0.05f * std::sin (ph * 2.63f + 1.0f);
            const float jitter  = (rng.nextFloat() - 0.5f) * 0.05f;
            const float target  = juce::jlimit (0.5f, 1.0f, 0.84f + shimmer + jitter);
            glowL[i] = glowL[i] * 0.7f + target * 0.3f;          // one-pole smooth → organic
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced (1.0f, 2.0f);
        drawAmpSymbol (g, area.removeFromLeft (46.0f), glow ? glowL[0] : 0.0f);   // amp unit (always)
        if (! showTubes || n <= 0) return;   // tubes hidden, or an "other" (0-tube) model → amp icon only
        area.removeFromLeft (4.0f);
        const float cell = area.getWidth() / (float) n;
        for (int i = 0; i < n; ++i)
            drawTube (g, area.withX (area.getX() + cell * (float) i).withWidth (cell),
                      tube, glow ? glowL[i] : 0.0f);
    }

private:
    // Stylised amplifier-unit icon (chassis + knobs + feet) feeding the power tubes — reads as
    // "the amp" rather than the too-engineering-y triangle symbol.
    void drawAmpSymbol (juce::Graphics& g, juce::Rectangle<float> region, float gl)
    {
        const float bw = juce::jmin (region.getWidth() - 4.0f, 42.0f);
        const float bh = juce::jmin (region.getHeight() - 12.0f, 28.0f);
        const float bx = region.getCentreX() - bw * 0.5f;
        const float by = region.getCentreY() - bh * 0.5f;

        if (gl > 0.0f) { g.setColour (juce::Colour (0xffff7a1e).withAlpha (0.13f * gl)); g.fillRoundedRectangle (bx, by, bw, bh, 3.0f); }
        const auto col = juce::Colour (0xff9fb0c8);
        g.setColour (col);
        g.drawRoundedRectangle (bx, by, bw, bh, 3.0f, 1.4f);                       // chassis

        // vent/grille dots (3×2), top-left
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                g.fillEllipse (bx + bw * 0.13f + (float) c * bw * 0.08f,
                               by + bh * 0.18f + (float) r * bh * 0.17f, 1.7f, 1.7f);

        // two small control knobs (under the grille)
        const float kr = bh * 0.12f, ky = by + bh * 0.64f;
        for (int i = 0; i < 2; ++i)
            g.drawEllipse (bx + bw * 0.16f + (float) i * bw * 0.16f - kr, ky - kr, kr * 2.0f, kr * 2.0f, 1.2f);

        // big knob (centre-right) with a pointer
        const float bkx = bx + bw * 0.66f, bky = by + bh * 0.5f, bkr = bh * 0.26f;
        g.drawEllipse (bkx - bkr, bky - bkr, bkr * 2.0f, bkr * 2.0f, 1.4f);
        g.drawLine (bkx, bky, bkx, bky - bkr, 1.2f);

        // feet
        g.drawLine (bx + bw * 0.2f, by + bh, bx + bw * 0.2f, by + bh + 3.0f, 1.4f);
        g.drawLine (bx + bw * 0.8f, by + bh, bx + bw * 0.8f, by + bh + 3.0f, 1.4f);
    }

    // Stylised v1 silhouettes (viewBox 64 x 120).
    static juce::Path glassPath (int t)
    {
        juce::Path p;
        switch (t)
        {
            case 1:  // EL34 — straight
                p.startNewSubPath (20,96); p.lineTo (20,34);
                p.cubicTo (20,20, 25,14, 32,14); p.cubicTo (39,14, 44,20, 44,34);
                p.lineTo (44,96); p.closeSubPath(); break;
            case 2:  // EL84 — slim
                p.startNewSubPath (25,96); p.lineTo (25,36);
                p.cubicTo (25,24, 28,17, 32,17); p.cubicTo (36,17, 39,24, 39,36);
                p.lineTo (39,96); p.closeSubPath(); break;
            case 3:  // KT88 — fat balloon
                p.startNewSubPath (15,96); p.cubicTo (13,72, 13,40, 24,24);
                p.cubicTo (27,19, 37,19, 40,24); p.cubicTo (52,40, 52,72, 49,96);
                p.closeSubPath(); break;
            default: // 6L6 — coke-bottle
                p.startNewSubPath (20,96); p.cubicTo (17,72, 16,44, 22,26);
                p.cubicTo (24,18, 27,14, 32,14); p.cubicTo (37,14, 40,18, 42,26);
                p.cubicTo (48,44, 47,72, 44,96); p.closeSubPath(); break;
        }
        return p;
    }

    void drawTube (juce::Graphics& g, juce::Rectangle<float> cell, int t, float gl)
    {
        juce::Graphics::ScopedSaveState save (g);
        const float scale = cell.getHeight() / 120.0f;
        const float tw = 64.0f * scale;
        g.addTransform (juce::AffineTransform::scale (scale)
                            .translated (cell.getCentreX() - tw * 0.5f, cell.getY()));

        // warm glow (active only) — radial amber, alpha follows the flicker
        if (gl > 0.0f)
        {
            juce::ColourGradient grad (juce::Colour (0xffffd08a).withAlpha (0.95f * gl), 32.0f, 74.0f,
                                       juce::Colour (0xffff5e10).withAlpha (0.0f),       32.0f, 34.0f, true);
            grad.addColour (0.42, juce::Colour (0xffff7a1e).withAlpha (0.5f * gl));
            g.setGradientFill (grad);
            g.fillEllipse (2.0f, 34.0f, 60.0f, 80.0f);
        }

        // glass envelope
        const auto glass = glassPath (t);
        g.setColour (juce::Colour (0x18aebfd1));  g.fillPath (glass);
        g.setColour (juce::Colour (0xff9fb0c8));  g.strokePath (glass, juce::PathStrokeType (1.4f));

        // getter (small mirror near the top)
        g.setColour (juce::Colour (0xff7d8aa0).withAlpha (0.55f));
        g.fillEllipse (23.0f, 16.0f, 18.0f, 8.0f);

        // plate / anode (warms up when glowing)
        g.setColour (juce::Colour (0xff14151a));  g.fillRoundedRectangle (24.0f, 40.0f, 16.0f, 44.0f, 2.0f);
        g.setColour (gl > 0.0f ? juce::Colour (0xff6a4d28).interpolatedWith (juce::Colour (0xff3a3f4a), 1.0f - gl)
                               : juce::Colour (0xff3a3f4a));
        g.drawRoundedRectangle (24.0f, 40.0f, 16.0f, 44.0f, 2.0f, 1.0f);
        g.setColour (juce::Colour (0xff2c303a));
        for (float x : { 28.0f, 32.0f, 36.0f }) g.drawLine (x, 42.0f, x, 82.0f, 1.0f);

        // heater filament (warm arc, glows with the flicker)
        if (gl > 0.0f)
        {
            juce::Path fil; fil.startNewSubPath (27.0f, 78.0f); fil.quadraticTo (32.0f, 70.0f, 37.0f, 78.0f);
            g.setColour (juce::Colour (0xffffd08a).withAlpha (juce::jlimit (0.0f, 1.0f, gl)));
            g.strokePath (fil, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // base + 3 pins
        g.setColour (juce::Colour (0xff1f2128));  g.fillRoundedRectangle (18.0f, 96.0f, 28.0f, 16.0f, 3.0f);
        g.setColour (juce::Colour (0xff7a8190));
        g.drawLine (24.0f, 112.0f, 24.0f, 120.0f, 1.8f);
        g.drawLine (32.0f, 112.0f, 32.0f, 121.0f, 1.8f);
        g.drawLine (40.0f, 112.0f, 40.0f, 120.0f, 1.8f);
    }

    int   tube = 0, n = 1;
    bool  glow = false, showTubes = true;
    float phase = 0.0f;
    float glowL[kMaxTubes] { 0.84f, 0.84f, 0.84f, 0.84f, 0.84f, 0.84f, 0.84f, 0.84f };
    juce::Random rng;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TubeDisplay)
};
