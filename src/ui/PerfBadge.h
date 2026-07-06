// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "OrbitCabLookAndFeel.h"

//==============================================================================
// PerfBadge — a small clickable readout next to the version: "<latency> ms · <CPU>%". The editor
// pushes a Stats snapshot each timer tick (it owns the processor handle); the badge just displays.
// Click → a CallOutBox (same pattern as VersionBadge) with the per-stage DSP-load breakdown
// (Preamp / EQ / Reverb / Poweramp / Cab + Total), kept live by its own timer while open.
//
// The CPU figure is a wall-clock estimate (% of the block's real-time budget), smoothed in the
// engine — noisy and machine-dependent by nature, so it's shown as an approximate gauge.
//==============================================================================
class PerfBadge final : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    struct Stats { int latencySamples = 0; float latencyMs = 0.0f, total = 0.0f, preamp = 0.0f, eq = 0.0f, reverb = 0.0f, poweramp = 0.0f, cab = 0.0f; };

    PerfBadge()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("DSP latency & load — click for the per-stage breakdown");
    }

    void setStats (const Stats& s) { stats = s; repaint(); }
    const Stats& getStats() const noexcept { return stats; }

    void paint (juce::Graphics& g) override
    {
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        const juce::uint32 col = stats.total < 50.0f ? 0xff70707a
                               : stats.total < 80.0f ? 0xffe0b020 : 0xffe06060;
        g.setColour (juce::Colour (col));
        g.drawText (juce::String (stats.latencySamples) + " smp  " + juce::String (stats.total, 1) + "%",
                    getLocalBounds().toFloat(), juce::Justification::centredRight, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()))
            showPopup();
    }

private:
    Stats stats;

    struct Panel final : public juce::Component, private juce::Timer
    {
        explicit Panel (PerfBadge& b) : owner (&b) { setSize (236, 198); startTimerHz (20); }   // +1 row (Reverb)
        void timerCallback() override { repaint(); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().reduced (14, 12);
            const Stats s = owner != nullptr ? owner->getStats() : Stats {};

            g.setColour (juce::Colour (OrbitCabLookAndFeel::kText));
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText ("DSP load", r.removeFromTop (18), juce::Justification::centredLeft, false);

            g.setFont (juce::FontOptions (11.0f));
            g.setColour (juce::Colour (0xff9a9aa4));
            g.drawText ("Latency " + juce::String (s.latencySamples) + " smp  \xc2\xb7  " + juce::String (s.latencyMs, 2) + " ms"
                        + (s.latencySamples > 0 ? "  (PDC)" : "  (zero)"),
                        r.removeFromTop (16), juce::Justification::centredLeft, false);
            r.removeFromTop (6);

            struct Row { const char* name; float v; juce::uint32 col; };
            const Row rows[] = {
                { "Preamp",   s.preamp,   OrbitCabLookAndFeel::kAccent  },
                { "EQ",       s.eq,       OrbitCabLookAndFeel::kNeutral },
                { "Reverb",   s.reverb,   OrbitCabLookAndFeel::kAccent  },
                { "Poweramp", s.poweramp, OrbitCabLookAndFeel::kAccent  },
                { "Cab",      s.cab,      OrbitCabLookAndFeel::kAccentB },
            };
            for (auto& row : rows)
                drawBar (g, r.removeFromTop (20), row.name, row.v, juce::Colour (row.col), false);
            r.removeFromTop (4);
            drawBar (g, r.removeFromTop (22), "Total", s.total, juce::Colour (0xffd0d0d8), true);
        }

        static void drawBar (juce::Graphics& g, juce::Rectangle<int> rr, const juce::String& name,
                             float pct, juce::Colour col, bool bold)
        {
            g.setFont (juce::FontOptions (11.0f, bold ? juce::Font::bold : juce::Font::plain));
            g.setColour (juce::Colour (OrbitCabLookAndFeel::kText));
            g.drawText (name, rr.removeFromLeft (60), juce::Justification::centredLeft, false);
            g.drawText (juce::String (pct, 1) + "%", rr.removeFromRight (44),
                        juce::Justification::centredRight, false);
            auto track = rr.reduced (4, 0).withSizeKeepingCentre (juce::jmax (1, rr.getWidth() - 8), 6).toFloat();
            g.setColour (juce::Colour (0x33ffffff));
            g.fillRoundedRectangle (track, 3.0f);
            g.setColour (col);
            g.fillRoundedRectangle (track.withWidth (juce::jlimit (0.0f, 1.0f, pct / 100.0f) * track.getWidth()), 3.0f);
        }

        juce::Component::SafePointer<PerfBadge> owner;
    };

    void showPopup()
    {
        auto panel = std::make_unique<Panel> (*this);
        if (auto* top = getTopLevelComponent())
            juce::CallOutBox::launchAsynchronously (std::move (panel),
                                                    top->getLocalArea (this, getLocalBounds()), top);
        else
            juce::CallOutBox::launchAsynchronously (std::move (panel), getScreenBounds(), nullptr);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerfBadge)
};
