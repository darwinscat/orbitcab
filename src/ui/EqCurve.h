// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <teq/EqEngine.h>
#include "OrbitCabLookAndFeel.h"

#include <cmath>
#include <functional>
#include <vector>

//==============================================================================
// EqCurve — the amp-EQ frequency-response display AND its HPF/LPF interaction surface. The owner sets
// `getBands` (fills a teq::BandParams array — index 0 = HPF, last = LPF; returns the count) + `sampleRate`;
// paint() evaluates the exact composite magnitude via teq::EqEngine::magnitudeDbFor() (the same math the
// audio path uses, so the curve never lies). Log axis 20 Hz–20 kHz, ±kDbRange dB.
//
// INTERACTION (replaces the old HPF/LPF freq knobs): the LEFT edge is the HPF corner, the RIGHT edge the
// LPF corner — drag them horizontally to set the frequency (like the IR-waveform filter edges). The owner
// feeds the live HPF/LPF on-state + freq + ranges via setHpf/setLpf, and gets drag callbacks out.
//
// A faint SPECTRUM (the amp output) is drawn behind the curve on the same log axis.
//==============================================================================
class EqCurve : public juce::Component
{
public:
    static constexpr double kDbRange = 18.0;     // ± vertical range
    static constexpr float  kFLo = 20.0f, kFHi = 20000.0f;
    static constexpr float  kGrabPx = 16.0f;     // hit-test radius around a corner's x

    std::function<int (teq::BandParams* out)> getBands;   // fill bands, return count; nullptr → nothing drawn
    double sampleRate = 48000.0;

    // Drag callbacks — the owner writes the param (setValueNotifyingHost). hz is already clamped to range.
    std::function<void (float hz)> onHpfDragged, onLpfDragged;

    EqCurve() { setInterceptsMouseClicks (true, false); }   // draggable corners; children (checkboxes) still click

    // Live HPF/LPF state pushed by the editor each timer tick (from the params).
    void setHpf (bool on, float hz, float lo, float hi) { hpfOn = on; hpfHz = hz; hpfMin = lo; hpfMax = hi; }
    void setLpf (bool on, float hz, float lo, float hi) { lpfOn = on; lpfHz = hz; lpfMin = lo; lpfMax = hi; }
    void setSpectrum (const std::vector<float>& post) { postSpec = post; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (r.isEmpty() || ! getBands)
            return;

        const double logSpan = std::log ((double) kFHi / kFLo);
        auto yFor = [&] (double db) noexcept
        {
            const double d = juce::jlimit (-60.0, 60.0, db);
            return r.getCentreY() - (float) (d / kDbRange) * (r.getHeight() * 0.5f);
        };

        // --- spectrum (amp output) behind everything: faint filled area on the same log axis ---
        if (! postSpec.empty())
        {
            const int nb = (int) postSpec.size();
            juce::Path s;
            s.startNewSubPath (r.getX(), r.getBottom());
            for (int i = 0; i < nb; ++i)
            {
                const float x = r.getX() + r.getWidth() * (float) i / (float) (nb - 1);
                const float h = juce::jlimit (0.0f, 1.0f, postSpec[(size_t) i]) * r.getHeight() * 0.9f;
                s.lineTo (x, r.getBottom() - h);
            }
            s.lineTo (r.getRight(), r.getBottom());
            s.closeSubPath();
            g.setColour (juce::Colour (0x18ffffff));
            g.fillPath (s);
        }

        teq::BandParams bands[teq::EqEngine::kMaxBands];
        const int n = getBands (bands);
        const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        const float yZero = yFor (0.0);

        juce::Path p;
        const int steps = juce::jmax (2, (int) r.getWidth());
        for (int i = 0; i <= steps; ++i)
        {
            const double frac = (double) i / (double) steps;
            const double f    = (double) kFLo * std::exp (logSpan * frac);
            const double db   = teq::EqEngine::magnitudeDbFor (bands, n, f, fs);
            const float  x    = r.getX() + (float) frac * r.getWidth();
            if (i == 0) p.startNewSubPath (x, yFor (db));
            else        p.lineTo (x, yFor (db));
        }

        juce::Path fill = p;
        fill.lineTo (r.getRight(), yZero);
        fill.lineTo (r.getX(),     yZero);
        fill.closeSubPath();
        g.setColour (juce::Colour (OrbitCabLookAndFeel::kAccent).withAlpha (0.14f));
        g.fillPath (fill);

        g.setColour (juce::Colour (0x33ffffff));
        g.drawHorizontalLine ((int) yZero, r.getX(), r.getRight());

        // the curve on top
        g.setColour (juce::Colour (OrbitCabLookAndFeel::kAccent).withAlpha (0.95f));
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));

        // --- HPF / LPF draggable corner handles: a bright vertical grip + a grab dot, only when ON ---
        auto drawHandle = [&] (bool on, float hz, bool hot)
        {
            if (! on) return;
            const float x = xForFreq (hz, r);
            g.setColour (juce::Colour (OrbitCabLookAndFeel::kAccent).withAlpha (hot ? 0.95f : 0.6f));
            g.fillRect (juce::Rectangle<float> (x - 1.0f, r.getY(), 2.0f, r.getHeight()));
            g.fillEllipse (x - 4.0f, r.getCentreY() - 4.0f, 8.0f, 8.0f);
        };
        drawHandle (hpfOn, juce::jlimit (kFLo, kFHi, hpfHz), drag == Drag::hpf || hover == Drag::hpf);
        drawHandle (lpfOn, juce::jlimit (kFLo, kFHi, lpfHz), drag == Drag::lpf || hover == Drag::lpf);
    }

    void mouseMove (const juce::MouseEvent& e) override { hover = pick (e.position.x); updateCursor(); repaint(); }
    void mouseExit (const juce::MouseEvent&)     override { hover = Drag::none; setMouseCursor (juce::MouseCursor::NormalCursor); repaint(); }
    void mouseDown (const juce::MouseEvent& e) override { drag = pick (e.position.x); apply (e.position.x); }
    void mouseDrag (const juce::MouseEvent& e) override { if (drag != Drag::none) apply (e.position.x); }
    void mouseUp   (const juce::MouseEvent& e) override { drag = Drag::none; hover = pick (e.position.x); updateCursor(); repaint(); }

private:
    enum class Drag { none, hpf, lpf };

    float xForFreq (float f, juce::Rectangle<float> r) const
    {
        return r.getX() + r.getWidth() * std::log (juce::jlimit (kFLo, kFHi, f) / kFLo) / std::log (kFHi / kFLo);
    }
    float freqForX (float x, juce::Rectangle<float> r) const
    {
        const float t = juce::jlimit (0.0f, 1.0f, (x - r.getX()) / juce::jmax (1.0f, r.getWidth()));
        return kFLo * std::pow (kFHi / kFLo, t);
    }

    Drag pick (float x) const
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool nearHpf = hpfOn && std::abs (x - xForFreq (hpfHz, r)) < kGrabPx;
        const bool nearLpf = lpfOn && std::abs (x - xForFreq (lpfHz, r)) < kGrabPx;
        // If both corners are within grab range, pick the closer one.
        if (nearHpf && nearLpf)
            return (std::abs (x - xForFreq (hpfHz, r)) <= std::abs (x - xForFreq (lpfHz, r))) ? Drag::hpf : Drag::lpf;
        if (nearHpf) return Drag::hpf;
        if (nearLpf) return Drag::lpf;
        return Drag::none;
    }

    void apply (float x)
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (drag == Drag::hpf) { const float hz = juce::jlimit (hpfMin, hpfMax, freqForX (x, r)); hpfHz = hz; if (onHpfDragged) onHpfDragged (hz); repaint(); }
        else if (drag == Drag::lpf) { const float hz = juce::jlimit (lpfMin, lpfMax, freqForX (x, r)); lpfHz = hz; if (onLpfDragged) onLpfDragged (hz); repaint(); }
    }

    void updateCursor()
    {
        setMouseCursor ((hover == Drag::none) ? juce::MouseCursor::NormalCursor
                                              : juce::MouseCursor::LeftRightResizeCursor);
    }

    bool  hpfOn = false, lpfOn = false;
    float hpfHz = 80.0f, hpfMin = 20.0f,  hpfMax = 300.0f;
    float lpfHz = 10000.0f, lpfMin = 4000.0f, lpfMax = 12000.0f;
    std::vector<float> postSpec;
    Drag  drag = Drag::none, hover = Drag::none;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqCurve)
};
