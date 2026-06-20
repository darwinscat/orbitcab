// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Offline regression gate for the channel-mute / auto-level interaction (#45). Feeds a
// steady tone through the real processor with auto-level ON and a loud bundled cab, toggles
// MUTE A, and checks the output RMS stays near the steady level through both the mute and
// the un-mute (a flat envelope). The old bug dropped it to ~0.06 on mute (dry crawling up
// over ~1 s) and overshot to ~1.55 on un-mute (the "poof"); this catches either if it
// returns. Returns non-zero on failure so it can gate CI. Also prints the envelope.
#include "../../src/PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;          // MessageManager for the reload timer
    OrbitCabAudioProcessor proc;

    const double sr = 48000.0;
    const int    block = 256;                      // ~5.3 ms/block
    proc.prepareToPlay (sr, block);

    auto& apvts = proc.apvts;
    auto setP = [&] (const char* id, float norm) { if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (norm); };
    setP ("autoLevel", 1.0f);   // ON — the interaction under test
    setP ("bypass",    0.0f);
    setP ("mixA",      1.0f);   // 100% wet A

    auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
    pump (400);                 // bundled IR load + settle

    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;

    double ph = 0.0;
    const double w = juce::MathConstants<double>::twoPi * 220.0 / sr;
    auto fill = [&] { for (int i = 0; i < block; ++i) { const float s = (float) (0.5 * std::sin (ph)); ph += w; buf.setSample (0, i, s); buf.setSample (1, i, s); } };
    auto rms  = [&] { double a = 0.0; for (int i = 0; i < block; ++i) { const double v = buf.getSample (0, i); a += v * v; } return std::sqrt (a / block); };

    auto run = [&] (int blocks, std::vector<double>& dst, const char* tag)
    {
        for (int b = 0; b < blocks; ++b)
        {
            fill();
            proc.processBlock (buf, midi);
            const double r = rms();
            dst.push_back (r);
            if (b % 8 == 0)
                std::printf ("  %-8s t=%6.0f ms   outRMS=%.4f\n", tag, b * block * 1000.0 / sr, r);
        }
    };

    std::vector<double> active, muted, unmuted;
    std::printf ("=== A active, auto-level ON (settle) ===\n");                run (48, active,  "active");
    std::printf ("=== MUTE A ===\n");        setP ("muteA", 1.0f);            run (200, muted,  "muted");
    std::printf ("=== UN-MUTE A ===\n");     setP ("muteA", 0.0f);            run (200, unmuted, "unmuted");

    // Steady reference = mean of the last 12 active blocks (auto-level converged by then).
    double steady = 0.0; for (size_t i = active.size() - 12; i < active.size(); ++i) steady += active[i];
    steady /= 12.0;

    // After the gate crossfade (skip the first ~8 blocks ≈ 43 ms), the envelope must stay
    // within [0.5, 1.6] x steady through both windows — flat, no dip and no overshoot.
    const int    margin = 8;
    const double lo = 0.5 * steady, hi = 1.6 * steady;
    double worstLo = 1.0e9, worstHi = 0.0;
    bool ok = true;
    for (const auto* win : { &muted, &unmuted })
        for (size_t i = (size_t) margin; i < win->size(); ++i)
        {
            worstLo = std::min (worstLo, (*win)[i]);
            worstHi = std::max (worstHi, (*win)[i]);
            if ((*win)[i] < lo || (*win)[i] > hi) ok = false;
        }

    std::printf ("\nsteady=%.3f  band=[%.3f, %.3f]  observed=[%.3f, %.3f]\n", steady, lo, hi, worstLo, worstHi);
    std::printf ("==== MUTE/AUTO-LEVEL ENVELOPE: %s ====\n", ok ? "FLAT (PASS)" : "DIP/OVERSHOOT (FAIL)");
    return ok ? 0 : 1;
}
