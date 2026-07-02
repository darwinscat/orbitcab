// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// DISCRIMINATING repro for the "capture<->simulator switch → ~0.5 s volume RISE" bug.
//
// Two hypotheses were on the table:
//   (A) AutoLeveler spectral re-convergence — the mode switch changes the SPECTRUM hitting the
//       (frequency-dependent) cab, so the wet/dry RMS ratio steps; the leveler's makeup then
//       crawls to the new value over its ~0.15 s time-constant (~0.45 s settle) → an audible swell.
//   (B) Latency / PDC change on the mode switch (capture 0 @ 48k ↔ tube 31) → host re-sync.
//
// This test runs the FULL engine HEADLESS at 48 kHz. At 48 kHz the capture (NAM) latency is 0 and
// there is NO host, so setLatencySamples / PDC does not exist on this path. Therefore any output-
// level excursion measured here CANNOT be (B) — it can only be internal DSP, i.e. (A). The signal
// is a fixed-amplitude multi-sine (steady-state), so a moving OUTPUT envelope is the leveler alone,
// not the input. If a multi-dB swell decaying over ~0.5 s appears here, hypothesis (A) is proven and
// (B) is excluded for the 48 kHz case the user is on. The bounded-overshoot assertions are the
// regression GUARD: they are meant to FAIL on today's code (documenting the bug) and PASS once the
// switch no longer makes the leveler chase (the fix). Never loosen them to go green.
#include "core/CabEngine.h"
#include "core/Params.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

using namespace cab;

namespace
{
    // A steady-state, fixed-amplitude broadband excitation. White noise (reproducible LCG, phase-
    // continuous by the running index) → a flat spectrum that exercises every cab band and, unlike a
    // sparse multi-sine, has low short-time RMS ripple. Constant statistics by construction → any
    // movement in the OUTPUT envelope is the plugin, never the source.
    float excite (long long n, double /*sr*/)
    {
        juce::uint32 s = (juce::uint32) (n * 2654435761u + 1013904223u);
        s ^= s >> 15; s *= 2246822519u; s ^= s >> 13;
        return 0.12f * ((float) ((double) s / 4294967296.0) - 0.5f) * 2.0f;
    }

    // A deliberately NON-FLAT (frequency-dependent) "cab" IR: a fast broadband spike + a presence
    // resonance (~1.8 kHz) + a low resonance (~140 Hz). Its coloured magnitude response is what makes
    // the leveler's makeup depend on the SPECTRUM — i.e. what a mode switch perturbs.
    std::vector<float> resonantIR (double sr, int len = 400)
    {
        std::vector<float> ir ((size_t) len, 0.0f);
        for (int n = 0; n < len; ++n)
        {
            const double t = (double) n;
            double v = 0.0;
            if (n == 0) v += 0.5;                                                             // broadband click
            v += std::exp (-t / 45.0)  * std::sin (2.0 * juce::MathConstants<double>::pi * 1800.0 * t / sr);   // presence peak
            v += 0.6 * std::exp (-t / 220.0) * std::sin (2.0 * juce::MathConstants<double>::pi * 140.0 * t / sr); // low resonance
            ir[(size_t) n] = (float) (0.6 * v);
        }
        return ir;
    }

    float blockRms (const juce::AudioBuffer<float>& b, int numSamples)
    {
        double acc = 0.0;
        for (int n = 0; n < numSamples; ++n) { const double x = b.getSample (0, n); acc += x * x; }
        return (float) std::sqrt (acc / juce::jmax (1, numSamples));
    }
}

struct PowerAmpSwitchLevelTest : juce::UnitTest
{
    PowerAmpSwitchLevelTest() : juce::UnitTest ("PowerAmp mode-switch level (leveler swell)") {}

    // Drive the engine for `blocks` blocks of `blk` samples with mode `tube`?tube:capture, appending
    // per block: the output RMS to `env` and the leveler's makeup gain to `mk` (ripple-free — the
    // clean instrument for "is the leveler still crawling"). `idx` carries the excitation phase.
    void drive (CabEngine& e, Params p, bool tube, int blocks, int blk, long long& idx,
                std::vector<float>& env, std::vector<float>& mk, double sr)
    {
        p.powerAmpMode = tube ? PowerAmpMode::tube : PowerAmpMode::capture;
        juce::AudioBuffer<float> buf (2, blk);
        for (int b = 0; b < blocks; ++b)
        {
            for (int n = 0; n < blk; ++n) { const float x = excite (idx++, sr); buf.setSample (0, n, x); buf.setSample (1, n, x); }
            e.process (buf.getArrayOfWritePointers(), 2, blk, p, false);
            env.push_back (blockRms (buf, blk));
            mk.push_back  (e.autoLevelGain());
        }
    }

    void runTest() override
    {
       #ifndef ORBITCAB_RES_DIR
        beginTest ("skipped — no ORBITCAB_RES_DIR"); expect (true);
       #else
        const double sr = 48000.0;          // the user's session rate: capture latency == 0, PDC excluded
        const int    maxBlock = 512;
        const int    blk = 64;              // fine envelope resolution (~1.33 ms/block)

        const juce::File nf = juce::File (ORBITCAB_RES_DIR).getChildFile ("preamps/V4KR-red-12h.nam");
        beginTest ("a real 48k capture model is present");
        expect (nf.existsAsFile(), "need a test .nam: " + nf.getFullPathName());
        if (! nf.existsAsFile()) return;
        juce::MemoryBlock mb; nf.loadFileAsData (mb);

        Params p;
        p.autoLevel = true;                 // the follower under test
        p.ampOn     = true;
        p.aLoaded   = true;
        p.slot[0].dryWet01 = 1.0f;          // full wet → the cab colour reaches the leveler
        p.powerAmpMode = PowerAmpMode::capture;
        // REALISTIC tube settings: the feel layer (sag/bias) has long-memory state, so a cold start
        // actually matters here (unlike drive 0 where it's ~stateless). This is the regime the user A/Bs.
        p.tube.driveDb  = 18.0f;
        p.tube.sag      = 0.5f;  p.tube.presence = 0.5f;  p.tube.depth = 0.5f;
        p.tube.load     = 0.3f;  p.tube.iron     = 0.5f;  p.tube.bias  = 0.7f;

        CabEngine e; e.prepare (sr, maxBlock, 2, p);
        expect (e.loadAmpModelBytes (mb.getData(), mb.getSize()), "capture .nam must load");

        probeMatch (e, sr);   // measure capture vs tube POST-AMP gain across param corners (decides G_tube scheme)

        // Load the coloured cab IR into slot A and pump until the off-thread convolver build is live.
        const auto ir = resonantIR (sr);
        const float* irPtr[1] = { ir.data() };
        e.setSlotOriginalIR (0, irPtr, 1, (int) ir.size(), sr);
        e.slotApplyTrim (0, false, 1.0f, false);
        {
            juce::AudioBuffer<float> warm (2, maxBlock);
            long long w = 0;
            for (int i = 0; i < 60; ++i)
            {
                for (int n = 0; n < maxBlock; ++n) { const float x = excite (w++, sr); warm.setSample (0, n, x); warm.setSample (1, n, x); }
                e.process (warm.getArrayOfWritePointers(), 2, maxBlock, p, false);
                juce::Thread::sleep (8);
            }
        }

        analyseSwitch (e, p, /*capture->tube*/ true);
        analyseSwitch (e, p, /*tube->capture*/ false);
       #endif
    }

    // dB ratio with a small floor to stay finite near silence.
    static float dB (float a, float b) { return 20.0f * std::log10 (juce::jmax (1.0e-9f, a) / juce::jmax (1.0e-9f, b)); }

    // Measure the POST-AMP gain (out/in RMS, dB) for params `p`: autoLevel OFF, empty cab (aLoaded=false
    // ⇒ output == the post-amp DRY signal, no cab colour). Feeds ~-18 dBFS broadband noise, discards the
    // first 300 ms (sag/filter settling), measures over the next ~600 ms. This is exactly the level the
    // AutoLeveler references, so it tells us the real capture-vs-tube post-amp loudness the match must equalise.
    float postAmpGainDb (CabEngine& e, Params p, double sr)
    {
        p.autoLevel = false;                 // measure the RAW post-amp level, no makeup
        p.aLoaded   = false;                 // empty slot A ⇒ output = post-amp dry (no cab)
        p.slot[0].dryWet01 = 0.0f;
        const int blk = 256;
        const int warm = (int) (0.30 * sr / blk), meas = (int) (0.60 * sr / blk);
        juce::AudioBuffer<float> buf (2, blk);
        double sumIn = 0.0, sumOut = 0.0; long long idx = 1;
        for (int b = 0; b < warm + meas; ++b)
        {
            double in = 0.0;
            for (int n = 0; n < blk; ++n)
            {
                juce::uint32 s = (juce::uint32) (idx++ * 2654435761u + 1013904223u); s ^= s >> 15; s *= 2246822519u; s ^= s >> 13;
                const float x = 0.218f * ((float) ((double) s / 4294967296.0) - 0.5f);   // ~-18 dBFS RMS white
                buf.setSample (0, n, x); buf.setSample (1, n, x); in += (double) x * x;
            }
            e.process (buf.getArrayOfWritePointers(), 2, blk, p, false);
            if (b >= warm)
            {
                sumIn += in;
                for (int n = 0; n < blk; ++n) { const double y = buf.getSample (0, n); sumOut += y * y; }
            }
        }
        return dB ((float) std::sqrt (sumOut), (float) std::sqrt (sumIn));
    }

    // Corner probe: how far is the TUBE post-amp gain from CAPTURE's, and how much does it move across the
    // feel params? Decides the match's G_tube scheme — if the tube gain is ~stable and near capture, a single
    // capture-referenced trim (Fable's (iii)) suffices; if it swings, the trim must track the tube too.
    void probeMatch (CabEngine& e, double sr)
    {
        beginTest ("MATCH probe: capture vs tube post-amp gain across param corners");
        Params cap; cap.ampOn = true; cap.powerAmpMode = PowerAmpMode::capture;
        const float gCap = postAmpGainDb (e, cap, sr);
        logMessage ("  G_cap (capture post-amp gain) : " + juce::String (gCap, 2) + " dB");

        auto tube = [] (float drive, float sag, float bias, float load, float iron, int voice)
        {
            Params p; p.ampOn = true; p.powerAmpMode = PowerAmpMode::tube;
            p.tube.driveDb = drive; p.tube.sag = sag; p.tube.bias = bias; p.tube.load = load;
            p.tube.iron = iron; p.tube.tubeType = voice; p.tube.presence = 0.5f; p.tube.depth = 0.5f;
            return p;
        };
        struct C { const char* name; Params p; };
        // default + one-at-a-time extremes + a couple of full corners
        float gMin = 1e9f, gMax = -1e9f;
        const std::pair<const char*, Params> corners[] = {
            { "default(d18 sag.5 bias.7 load.3 iron.5 6L6)", tube (18, 0.5f, 0.7f, 0.3f, 0.5f, 0) },
            { "drive 0",   tube (0,  0.5f, 0.7f, 0.3f, 0.5f, 0) },
            { "drive 36",  tube (36, 0.5f, 0.7f, 0.3f, 0.5f, 0) },
            { "sag 0",     tube (18, 0.0f, 0.7f, 0.3f, 0.5f, 0) },
            { "sag 1",     tube (18, 1.0f, 0.7f, 0.3f, 0.5f, 0) },
            { "bias 0",    tube (18, 0.5f, 0.0f, 0.3f, 0.5f, 0) },
            { "load 1",    tube (18, 0.5f, 0.7f, 1.0f, 0.5f, 0) },
            { "iron 1",    tube (18, 0.5f, 0.7f, 0.3f, 1.0f, 0) },
            { "EL34",      tube (18, 0.5f, 0.7f, 0.3f, 0.5f, 1) },
            { "KT88",      tube (18, 0.5f, 0.7f, 0.3f, 0.5f, 3) },
            { "hot corner",tube (36, 1.0f, 0.7f, 1.0f, 1.0f, 3) },
            { "clean corner",tube (0, 0.0f, 0.0f, 0.0f, 0.0f, 2) },
        };
        for (const auto& c : corners)
        {
            const float g = postAmpGainDb (e, c.second, sr);
            gMin = juce::jmin (gMin, g); gMax = juce::jmax (gMax, g);
            logMessage ("  G_tube " + juce::String (c.first).paddedRight (' ', 40) + juce::String (g, 2)
                        + " dB   (trim=G_cap residual " + juce::String (g, 2) + " dB)");
        }
        logMessage ("  --> tube gain spread across corners: " + juce::String (gMax - gMin, 2)
                    + " dB;  |worst vs unity| = " + juce::String (juce::jmax (std::abs (gMin), std::abs (gMax)), 2) + " dB");
        logMessage ("  --> if trim=G_cap (assume tube=unity): worst A/B residual = |G_tube| above; "
                    "if trim=G_cap-G_tube: exact at each corner");
        expect (true);   // report-only: informs the G_tube design decision
    }

    // Converge in the FROM mode, switch to the TO mode, and characterise the transient from the
    // ripple-free MAKEUP trajectory (the leveler's own state) plus the audible OUTPUT envelope:
    //   • makeup settle : ms after the switch until the makeup last sits >0.5 dB off its settled value
    //                     — this is the leveler "crawl" the bug is about (pre-fix ~0.5 s).
    //   • output overshoot : peak of a lightly-smoothed output RMS vs its settled level, in dB.
    void analyseSwitch (CabEngine& e, Params p, bool toTube)
    {
        const double sr = e.sampleRate();
        const int    blk = 64;
        const juce::String tag = toTube ? "capture->tube" : "tube->capture";
        beginTest (tag + " switch: leveler must not crawl (no ~0.5 s loudness swell)");

        long long idx = 1;
        std::vector<float> env, mk;
        drive (e, p, /*tube*/ ! toTube, /*blocks*/ 700, blk, idx, env, mk, sr);   // ~930 ms: converge in FROM mode
        const int switchAt = (int) env.size();
        drive (e, p, /*tube*/   toTube, /*blocks*/ 1200, blk, idx, env, mk, sr);  // ~1.6 s in TO mode (fully settles)

        // Settled TO-mode makeup = median of the last 200 blocks (well past any transient).
        std::vector<float> mkTail (mk.end() - 200, mk.end());
        std::sort (mkTail.begin(), mkTail.end());
        const float mkSettled = mkTail[mkTail.size() / 2];

        // Ripple-free makeup metrics (the leveler's own state — no signal ripple):
        const auto mkAtMs = [&] (int ms) { return dB (mk[(size_t) juce::jmin ((int) mk.size() - 1,
                                                       switchAt + (int) ((double) ms * 0.001 * sr / blk))], mkSettled); };
        // Re-match speed: how far the makeup still is from settled at +150 ms / +400 ms. A slow crawl
        // (pre-fix) sits ~2-3 dB off at 150 ms; the fast-track closes most of it by ~100-150 ms.
        const float reMatch150 = std::abs (mkAtMs (150));
        const float reMatch400 = std::abs (mkAtMs (400));
        // Overshoot: once the makeup first comes within 0.75 dB of settled, how far does it swing PAST
        // (the swell a too-aggressive catch-up would add). Measured to +1 s.
        int reached = switchAt; for (int i = switchAt; i < (int) mk.size(); ++i) { if (std::abs (dB (mk[(size_t) i], mkSettled)) < 0.75f) { reached = i; break; } }
        float overshoot = 0.0f;
        for (int i = reached; i < juce::jmin ((int) mk.size(), switchAt + (int) (1.0 * sr / blk)); ++i)
            overshoot = juce::jmax (overshoot, std::abs (dB (mk[(size_t) i], mkSettled)));

        // Output envelope (confounded by the capture<->tube PERMANENT level gap + the tube's own bloom,
        // so REPORT-ONLY, not a guard): lightly smoothed RMS peak vs settled.
        std::vector<float> sm (env.size());
        const float a = 1.0f - std::exp (-(float) blk / (0.008f * (float) sr));
        sm[0] = env[0];
        for (size_t i = 1; i < env.size(); ++i) sm[i] = sm[i - 1] + a * (env[i] - sm[i - 1]);
        std::vector<float> outTail (sm.end() - 200, sm.end()); std::sort (outTail.begin(), outTail.end());
        const float outSettled = outTail[outTail.size() / 2];
        float peak = 0.0f; for (int i = switchAt; i < (int) sm.size(); ++i) peak = juce::jmax (peak, sm[(size_t) i]);

        {
            juce::String traj = "  makeup traj dB@ms:";
            for (int ms : { 0, 25, 50, 100, 150, 200, 300, 450, 600, 900, 1300 })
                traj += " " + juce::String (ms) + "=" + juce::String (mkAtMs (ms), 1);
            logMessage (traj);
        }
        logMessage ("  re-match @150/@400  : " + juce::String (reMatch150, 2) + " / " + juce::String (reMatch400, 2) + " dB");
        logMessage ("  makeup overshoot    : " + juce::String (overshoot, 2) + " dB");
        logMessage ("  output peak/settled : " + juce::String (dB (peak, outSettled), 2) + " dB  (report-only: incl. level gap + bloom)");

        expect (std::isfinite (reMatch150) && std::isfinite (overshoot), "metrics must be finite");
        // GUARDS on the leveler's re-match (the swell the bug is about). Meant to FAIL on the pre-fix
        // slow-crawl leveler, PASS with the fast-track catch-up. Never loosen to go green.
        expect (reMatch150 < 1.5f, tag + " makeup still " + juce::String (reMatch150, 2) + " dB off at +150 ms — leveler crawling (want < 1.5)");
        expect (reMatch400 < 0.9f, tag + " makeup still " + juce::String (reMatch400, 2) + " dB off at +400 ms (want < 0.9)");
        expect (overshoot  < 1.2f, tag + " makeup overshoots " + juce::String (overshoot, 2) + " dB past settled (want < 1.2)");
    }
};

static PowerAmpSwitchLevelTest powerAmpSwitchLevelTest;
