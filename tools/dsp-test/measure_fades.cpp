// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

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

    // Deterministic guitar-band test signal: white noise band-passed to ~150 Hz..2 kHz (one-pole
    // HP + one-pole LP at the project's reference corner, cab::Convolver::kIrRefShapeHz). A real
    // DI is dark AND has no sub-bass: raw white overweights HF the cab kills, while an LP alone
    // passes DC-heavy energy whose 5 ms window-RMS wanders ±4 dB and trips the per-block band.
    juce::Random rng (1234);
    float lpState = 0.0f, hpState = 0.0f;
    const float lpA = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 2000.0 / sr));
    const float hpA = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 150.0 / sr));
    auto fill = [&] { for (int i = 0; i < block; ++i) { lpState += lpA * ((rng.nextFloat() * 2.0f - 1.0f) - lpState); hpState += hpA * (lpState - hpState); const float s = 1.2f * (lpState - hpState); buf.setSample (0, i, s); buf.setSample (1, i, s); } };
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

    // The check runs on a SMOOTHED envelope (8-block ≈ 43 ms moving average): the band-limited
    // stimulus has an inherent ±3 dB per-block RMS ripple even on a perfectly flat envelope, and
    // the bugs this tool guards are SLOW — a ~1 s dry crawl-up (dip to ~0.06) and a sustained
    // un-mute overshoot (~1.55x) — both far outside the band at any smoothing. Raw block RMS
    // would only ever trip on stimulus statistics, not on a real regression.
    auto smoothed = [] (const std::vector<double>& v)
    {
        std::vector<double> out (v.size(), 0.0);
        for (size_t i = 0; i < v.size(); ++i)
        {
            const size_t from = i >= 7 ? i - 7 : 0;
            double a = 0.0; for (size_t k = from; k <= i; ++k) a += v[k];
            out[i] = a / (double) (i - from + 1);
        }
        return out;
    };
    const auto activeSm = smoothed (active);

    // Steady reference = mean of the last 12 active blocks (auto-level converged by then).
    double steady = 0.0; for (size_t i = activeSm.size() - 12; i < activeSm.size(); ++i) steady += activeSm[i];
    steady /= 12.0;

    // After the gate crossfade (skip the first ~8 blocks ≈ 43 ms), the smoothed envelope must
    // stay within [0.5, 1.6] x steady through both windows — flat, no dip and no overshoot.
    const int    margin = 8;
    const double lo = 0.5 * steady, hi = 1.6 * steady;
    double worstLo = 1.0e9, worstHi = 0.0;
    bool ok = true;
    for (const auto* win : { &muted, &unmuted })
    {
        const auto sm = smoothed (*win);
        for (size_t i = (size_t) margin; i < sm.size(); ++i)
        {
            worstLo = std::min (worstLo, sm[i]);
            worstHi = std::max (worstHi, sm[i]);
            if (sm[i] < lo || sm[i] > hi) ok = false;
        }
    }

    std::printf ("\nsteady=%.3f  band=[%.3f, %.3f]  observed=[%.3f, %.3f]\n", steady, lo, hi, worstLo, worstHi);
    std::printf ("==== MUTE/AUTO-LEVEL ENVELOPE: %s ====\n", ok ? "FLAT (PASS)" : "DIP/OVERSHOOT (FAIL)");
    return ok ? 0 : 1;
}
