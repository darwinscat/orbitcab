// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <teq/EqEngine.h>
#include "OrbitCabLookAndFeel.h"

#include <cmath>
#include <functional>

//==============================================================================
// EqCurve — a tiny read-only display of the amp-EQ frequency response. The owner sets `getBands`
// (fills a teq::BandParams array, returns the count) and `sampleRate`; paint() evaluates the exact
// composite magnitude via teq::EqEngine::magnitudeDbFor() (the same math the audio path uses, so the
// curve never lies). Log frequency axis 20 Hz–20 kHz, ±kDbRange dB. Repainted ~30 Hz by the editor
// so it tracks the knobs live.
//==============================================================================
class EqCurve : public juce::Component
{
public:
    static constexpr double kDbRange = 18.0;   // ± vertical range

    std::function<int (teq::BandParams* out)> getBands;   // fill bands, return count; nullptr → nothing drawn
    double sampleRate = 48000.0;

    EqCurve() { setInterceptsMouseClicks (false, false); }   // purely decorative; clicks fall through

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (r.isEmpty() || ! getBands)
            return;

        constexpr double fLo = 20.0, fHi = 20000.0;
        const double logSpan = std::log (fHi / fLo);
        auto yFor = [&] (double db) noexcept
        {
            return r.getCentreY() - (float) (juce::jlimit (-kDbRange, kDbRange, db) / kDbRange) * (r.getHeight() * 0.5f);
        };

        teq::BandParams bands[teq::EqEngine::kMaxBands];
        const int n = getBands (bands);
        const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        const float yZero = yFor (0.0);

        juce::Path p;
        const int steps = juce::jmax (2, (int) r.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const double frac = (double) i / (double) steps;
            const double f    = fLo * std::exp (logSpan * frac);
            const double db   = teq::EqEngine::magnitudeDbFor (bands, n, f, fs);
            const float  x    = r.getX() + (float) frac * r.getWidth();
            const float  y    = yFor (db);
            if (i == 0) p.startNewSubPath (x, y);
            else        p.lineTo (x, y);
        }

        // Fill between the curve and the 0 dB line — the response "rests" on a horizontal shelf at
        // 0, the fill pinching to that line wherever the curve crosses 0 dB.
        juce::Path fill = p;
        fill.lineTo (r.getRight(), yZero);
        fill.lineTo (r.getX(),     yZero);
        fill.closeSubPath();
        g.setColour (juce::Colour (OrbitCabLookAndFeel::kAccent).withAlpha (0.14f));
        g.fillPath (fill);

        // the 0 dB shelf line itself
        g.setColour (juce::Colour (0x33ffffff));
        g.drawHorizontalLine ((int) yZero, r.getX(), r.getRight());

        // the curve on top
        g.setColour (juce::Colour (OrbitCabLookAndFeel::kAccent).withAlpha (0.95f));
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqCurve)
};
