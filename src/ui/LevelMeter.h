// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>

//==============================================================================
// A thin vertical peak meter (dBFS). The processor publishes a per-block peak
// (atomic, RT-safe); the editor's timer pushes it here via setLevel, which applies
// the ballistics (instant attack, smooth release) + a short peak-hold. Pure UI — no
// audio-thread work. Colours follow the brand: green / amber / clip-red (#ff6b6b).
//==============================================================================
class LevelMeter final : public juce::Component
{
public:
    LevelMeter() = default;

    // Called from the editor timer with the latest linear peak (0..~1+).
    void setLevel (float linearPeak)
    {
        linearPeak = juce::jmax (0.0f, linearPeak);

        if (linearPeak >= level) level = linearPeak;            // instant attack
        else                     level += (linearPeak - level) * kRelease;

        if (linearPeak >= peakHold) { peakHold = linearPeak; peakHoldTicks = 0; }
        else if (++peakHoldTicks > kPeakHoldTicks) peakHold *= kPeakDecay;

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff141417));
        g.fillRoundedRectangle (r, 2.0f);

        auto dbToY = [&] (float db)
        {
            return juce::jmap (juce::jlimit (kMinDb, kMaxDb, db), kMinDb, kMaxDb, r.getBottom(), r.getY());
        };

        const float db = level > 0.0f ? juce::Decibels::gainToDecibels (level) : -120.0f;
        if (db > kMinDb)
        {
            const float y = dbToY (db);
            juce::ColourGradient grad (juce::Colour (0xff7be29a), r.getCentreX(), r.getBottom(),
                                       juce::Colour (0xffff6b6b), r.getCentreX(), r.getY(), false);
            grad.addColour (0.74, juce::Colour (0xfff5c57a));   // amber band near the top
            g.setGradientFill (grad);
            g.fillRoundedRectangle (r.withTop (y).reduced (1.0f, 0.0f), 1.5f);
        }

        // peak-hold tick
        const float pdb = peakHold > 0.0f ? juce::Decibels::gainToDecibels (peakHold) : -120.0f;
        if (pdb > kMinDb)
        {
            const float py = dbToY (pdb);
            g.setColour (colourForDb (pdb));
            g.fillRect (r.getX() + 1.0f, py - 1.0f, r.getWidth() - 2.0f, 2.0f);
        }

        // dBFS scale: ticks at 0/−6/−12/−24/−48 + labels when the meter is wide enough
        g.setFont (juce::FontOptions (8.0f));
        for (const int mark : { 0, -6, -12, -24, -48 })
        {
            const float y = dbToY ((float) mark);
            g.setColour (mark == 0 ? juce::Colour (0x44ffffff) : juce::Colour (0x1effffff));
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            if (getWidth() >= 24)
            {
                g.setColour (juce::Colour (0x66b0b0b0));
                g.drawText (juce::String (mark), juce::Rectangle<float> (r.getX(), y - 5.0f, r.getWidth(), 10.0f),
                            juce::Justification::centred, false);
            }
        }
    }

private:
    static juce::Colour colourForDb (float db)
    {
        if (db >= -3.0f)  return juce::Colour (0xffff6b6b);   // clip-red
        if (db >= -12.0f) return juce::Colour (0xfff5c57a);   // amber
        return juce::Colour (0xff7be29a);                     // green
    }

    static constexpr float kMinDb         = -60.0f;
    static constexpr float kMaxDb         =   6.0f;
    static constexpr float kRelease       = 0.25f;   // per timer tick
    static constexpr float kPeakDecay     = 0.92f;
    static constexpr int   kPeakHoldTicks = 24;      // ~0.8 s at 30 Hz

    float level         = 0.0f;
    float peakHold      = 0.0f;
    int   peakHoldTicks = 0;
};
