// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// The capture<->tube switch-cleanliness CONTRACT, as adversarial engine-level tests.
//
// History: the original complaint was a ~0.5 s loudness swell on every mode switch. Root causes
// found (in order): a fast-track leveler catch-up that PUMPED (removed); then a makeup slew
// implemented against the smoother's current value, which compounded with the 30 ms ramp into a
// block-size-dependent crawl (0.4 dB/s @ 64-sample blocks) that effectively FROZE the leveler —
// so the full post-amp + cab-spectral A/B step (measured 6.7–8.8 dB) played unlevelled.
//
// The shipped design this file guards:
//   • AutoLeveler: block-size-invariant dynamics — 150 ms followers, a REAL 9 dB/s hard slew,
//     and a bounded 40 dB/s fast glide reserved for deterministic retargets (route snap, on/off).
//   • CabEngine: per-route makeup MEMORY — a route's converged makeup is remembered while it
//     dwells and SNAPPED to when the route returns (the A/B workflow), keyed to a level-context
//     generation (EQ/filters/IR/models/knobs) so a stale value can never snap.
//   • PowerAmpRouter/AmpStage: the tube is level-matched to the loaded capture DETERMINISTICALLY
//     (the capture's reference gain is measured once at model load with the shared cab::levelprobe
//     stimulus; the tube offsets from its own baked anchor) — no follower in that path.
//
// Contract asserted below (never loosen to go green). Mode switches are deliberately HARD
// (user decision: the honest 0<->31 PDC re-report makes the host re-sync around the switch, so
// in-plugin transitions between the modes are removed — the makeup retarget lands INSTANTLY):
//   1. post-amp A/B honesty at the reference operating point: |G_cap − G_tube| ≤ 0.6 dB;
//   2. FIRST-ever visit to a route (nothing remembered): makeup converges at follower speed
//      inside the transition window (target ceiling 20 dB/s, applied ≤ 40; ≤ 9 dB/s after),
//      overshoot ≤ 0.3 dB, settled by ~1.2 s — honest one-time measurement;
//   3. REPEATED A/B (the user workflow): the snap lands the makeup INSTANTLY (within one hop)
//      at the remembered value ±the live wobble — including on NON-STATIONARY material (the
//      field bug: a settled()-write-gate never formed caches on real playing);
//   4. rapid-toggle abuse (10 Hz) stays finite, inside the mode envelope, and recovers;
//   5. a level-context change STALES the caches but the LEARNED PAIR DELTA survives: the next
//      switch snaps instantly to (current + delta) — near the truth — and only the small
//      residual converges.
#include "core/CabEngine.h"
#include "core/Params.h"
#include "core/LevelProbe.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

using namespace cab;

namespace
{
    // Steady-state, fixed-amplitude broadband excitation — the SHARED levelprobe LCG (one hash,
    // one place: LevelProbe.h), scaled to the battery's working level.
    float excite (long long n, double /*sr*/) { return 0.12f * levelprobe::white (n); }

    // NON-STATIONARY guitar-ish excitation: the same LCG under a 3 ms / 180 ms burst envelope,
    // peaks cycling −8..−16 dBFS. The leveler's target WANDERS ±~1 dB on this — exactly the
    // regime where the old settled()-gated cache never formed (the field bug).
    float exciteBursts (long long n, double sr)
    {
        const double t = (double) n / sr;
        const double period = 0.35;
        const double tin = t - std::floor (t / period) * period;
        static constexpr float peaks[4] = { 0.4f, 0.25f, 0.32f, 0.16f };
        const float pk = peaks[((long long) (t / period)) & 3];
        const double env = tin < 0.003 ? tin / 0.003 : std::exp (-(tin - 0.003) / 0.18);
        return pk * (float) env * levelprobe::white (n);
    }

    // A REAL factory cab IR. The battery originally used a synthetic high-Q resonant IR, but a
    // ~5 Hz-wide 140 Hz resonance makes the mix RMS statistically WANDER ±0.7 dB on noise at the
    // leveler's 150 ms window — the instrument, not the mechanism, got noisy. A real cab holds the
    // converged makeup to ±0.1 dB while still weighing the two modes' spectra ~4 dB apart (the
    // exact thing the route memory exists for).
    juce::AudioBuffer<float> factoryIR()
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        const juce::File f = juce::File (ORBITCAB_RES_DIR).getChildFile ("ir/01-cookie-monster.wav");
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
        if (rd == nullptr) return {};
        juce::AudioBuffer<float> ir ((int) rd->numChannels, (int) rd->lengthInSamples);
        rd->read (&ir, 0, (int) rd->lengthInSamples, 0, true, true);
        return ir;
    }
}

struct PowerAmpSwitchLevelTest : juce::UnitTest
{
    PowerAmpSwitchLevelTest() : juce::UnitTest ("PowerAmp mode-switch level (clean A/B contract)") {}

    double sr = 48000.0;
    static constexpr int kBlk = 64;

    static float dB (float g) { return juce::Decibels::gainToDecibels (g, -120.0f); }

    // Drive `blocks` blocks in the given mode, appending the leveler makeup (dB) per block.
    void drive (CabEngine& e, Params& p, bool tube, int blocks, long long& idx, std::vector<float>* mkDb,
                bool bursts = false)
    {
        p.powerAmpMode = tube ? PowerAmpMode::tube : PowerAmpMode::capture;
        juce::AudioBuffer<float> buf (2, kBlk);
        for (int b = 0; b < blocks; ++b)
        {
            for (int n = 0; n < kBlk; ++n)
            {
                const float x = bursts ? exciteBursts (idx, sr) : excite (idx, sr);
                ++idx;
                buf.setSample (0, n, x); buf.setSample (1, n, x);
            }
            e.process (buf.getArrayOfWritePointers(), 2, kBlk, p, false);
            if (mkDb != nullptr) mkDb->push_back (dB (e.autoLevelGain()));
        }
    }

    int blocksFor (double seconds) const { return (int) std::llround (seconds * sr / kBlk); }

    // Post-amp gain (dB) of the CURRENT params on the shared levelprobe stimulus: no cab,
    // leveler off ⇒ output == post-amp dry. Exactly the operating point the deterministic
    // capture<->tube level-match is calibrated at.
    float refStimGainDb (CabEngine& e, Params p)
    {
        p.autoLevel = false;
        p.aLoaded   = false;
        const int total = (int) ((levelprobe::kSettleSec + levelprobe::kMeasureSec + 0.4) * sr);
        std::vector<float> stim ((size_t) total);
        levelprobe::fill (stim.data(), total, sr);
        juce::AudioBuffer<float> buf (2, kBlk);
        const int skip = (int) (0.4 * sr);   // route fade + stage smoothing settle
        double si = 0.0, so = 0.0;
        for (int off = 0; off + kBlk <= total; off += kBlk)
        {
            for (int n = 0; n < kBlk; ++n) { const float x = stim[(size_t) (off + n)]; buf.setSample (0, n, x); buf.setSample (1, n, x); }
            if (off >= skip) for (int n = 0; n < kBlk; ++n) { const double x = buf.getSample (0, n); si += x * x; }
            e.process (buf.getArrayOfWritePointers(), 2, kBlk, p, false);
            if (off >= skip) for (int n = 0; n < kBlk; ++n) { const double y = buf.getSample (0, n); so += y * y; }
        }
        return (float) (20.0 * std::log10 (std::max (1.0e-9, std::sqrt (so / std::max (1.0, si)))));
    }

    void runTest() override
    {
       #ifndef ORBITCAB_RES_DIR
        beginTest ("skipped — no ORBITCAB_RES_DIR"); expect (true);
       #else
        const juce::File nf = juce::File (ORBITCAB_RES_DIR).getChildFile ("preamps/V4KR-red-12h.nam");
        beginTest ("a real 48k capture model is present");
        expect (nf.existsAsFile(), "need a test .nam: " + nf.getFullPathName());
        if (! nf.existsAsFile()) return;
        juce::MemoryBlock mb; nf.loadFileAsData (mb);

        Params p;
        p.autoLevel = true;
        p.ampOn     = true;
        p.aLoaded   = true;
        p.slot[0].dryWet01 = 1.0f;
        p.powerAmpMode = PowerAmpMode::capture;
        // Realistic tube settings — the regime the user A/Bs (feel layer engaged).
        p.tube.driveDb = 18.0f;
        p.tube.sag = 0.5f;  p.tube.presence = 0.5f;  p.tube.depth = 0.5f;
        p.tube.load = 0.3f; p.tube.iron = 0.5f;      p.tube.bias  = 0.7f;

        //================================================================ 0. anchor guard
        {
            // Re-measure the tube route's reference gain with NO capture armed (capDb inactive)
            // and pin it to the kTubeRefGainDb anchor baked in PowerAmpRouter.cpp (−1.96 dB,
            // measured @48 kHz via orbitcab_switch_probe's refprobe). If the levelprobe stimulus,
            // the voicing tables, or the knee calibration change, THIS fails and both must be
            // re-measured together — the capture level-match silently drifts otherwise.
            beginTest ("tube reference-gain anchor matches the baked kTubeRefGainDb");
            CabEngine e; e.prepare (sr, 512, 2, p);
            Params pt = p; pt.powerAmpMode = PowerAmpMode::tube;
            const float gTubeAnchor = refStimGainDb (e, pt);
            expectWithinAbsoluteError<float> (gTubeAnchor, -1.96f, 0.35f,
                "tube ref gain " + juce::String (gTubeAnchor, 2)
                + " dB drifted from the baked anchor — re-measure kTubeRefGainDb (PowerAmpRouter.cpp)");
        }

        //================================================================ 1. reference honesty
        {
            CabEngine e; e.prepare (sr, 512, 2, p);
            expect (e.loadAmpModelBytes (mb.getData(), mb.getSize()), "capture .nam must load");

            beginTest ("A/B honesty: capture vs tube post-amp gain at the reference stimulus");
            Params pc = p; pc.powerAmpMode = PowerAmpMode::capture;
            Params pt = p; pt.powerAmpMode = PowerAmpMode::tube;
            const float gCap  = refStimGainDb (e, pc);
            const float gTube = refStimGainDb (e, pt);
            logMessage ("  G_cap = " + juce::String (gCap, 2) + " dB, G_tube = " + juce::String (gTube, 2)
                        + " dB, A/B step = " + juce::String (gCap - gTube, 2) + " dB");
            // Pre-fix this was ~1.9 dB (tube at its dry-referenced calibration). The deterministic
            // capture match must hold it near zero AT THIS OPERATING POINT. Residual budget: the
            // engine-path probe re-measures through 64-sample blocking + route fades (±ε).
            expect (std::fabs (gCap - gTube) < 0.6f,
                    "capture<->tube step at the reference stimulus is " + juce::String (gCap - gTube, 2)
                    + " dB — the deterministic level-match is broken (want |step| < 0.6)");
        }

        //================================================================ engine for the switch battery
        CabEngine e; e.prepare (sr, 512, 2, p);
        expect (e.loadAmpModelBytes (mb.getData(), mb.getSize()), "capture .nam must load");
        const juce::AudioBuffer<float> ir = factoryIR();
        beginTest ("a factory cab IR is present");
        expect (ir.getNumSamples() > 0, "need resources/ir/01-cookie-monster.wav");
        if (ir.getNumSamples() == 0) return;
        const float* irPtr[2] = { ir.getReadPointer (0), ir.getReadPointer (ir.getNumChannels() > 1 ? 1 : 0) };
        e.setSlotOriginalIR (0, irPtr, ir.getNumChannels() >= 2 ? 2 : 1, ir.getNumSamples(), sr);
        e.slotApplyTrim (0, false, 1.0f, false);
        e.seedAutoLevel();   // the real adapter flow: seed the makeup from the IR energy at load
        {
            juce::AudioBuffer<float> warm (2, 512);
            long long w = 0;
            for (int i = 0; i < 60; ++i)
            {
                for (int n = 0; n < 512; ++n) { const float x = excite (w++, sr); warm.setSample (0, n, x); warm.setSample (1, n, x); }
                e.process (warm.getArrayOfWritePointers(), 2, 512, p, false);
                juce::Thread::sleep (8);
            }
        }

        long long idx = 1;
        std::vector<float> mk;

        //================================================================ 2. first visit
        beginTest ("FIRST tube visit: honest bounded convergence, no overshoot, no pump");
        drive (e, p, /*tube*/ false, blocksFor (2.2), idx, nullptr);         // fully converge + dwell capture
        const float mkCap0 = dB (e.autoLevelGain());
        mk.clear();
        drive (e, p, /*tube*/ true, blocksFor (1.6), idx, &mk);              // first tube visit
        {
            const float settled = mk.back();
            // (a) rate bounds: inside the 0.35 s transition window the applied gain may move at
            //     up to 40 dB/s (target ceiling 20); after it, the normal 9 dB/s hard cap rules.
            const float stepFast = (float) (40.0 * kBlk / sr) * 1.02f + 1.0e-3f;
            const float stepSlow = (float) (9.0  * kBlk / sr) * 1.02f + 1.0e-3f;
            const size_t windowB = (size_t) blocksFor (0.40);
            float worstIn = 0.0f, worstOut = 0.0f;
            for (size_t i = 1; i < mk.size(); ++i)
            {
                const float d = std::fabs (mk[i] - mk[i - 1]);
                if (i <= windowB) worstIn = juce::jmax (worstIn, d); else worstOut = juce::jmax (worstOut, d);
            }
            expect (worstIn  <= stepFast, "first-visit window step " + juce::String (worstIn, 4) + " dB/block (> 40 dB/s)");
            expect (worstOut <= stepSlow, "post-window step " + juce::String (worstOut, 4) + " dB/block (> 9 dB/s)");
            // (b) no overshoot past the settled value beyond 0.3 dB
            const bool rising = settled > mk.front();
            float overshoot = 0.0f;
            for (float v : mk) overshoot = juce::jmax (overshoot, rising ? v - settled : settled - v);
            expect (overshoot < 0.3f, "first-visit makeup overshot " + juce::String (overshoot, 2) + " dB past settled");
            // (c) converged within 1.2 s
            const size_t at12 = (size_t) juce::jmin ((int) mk.size() - 1, blocksFor (1.2));
            expect (std::fabs (mk[at12] - settled) < 0.3f,
                    "first-visit makeup still " + juce::String (std::fabs (mk[at12] - settled), 2) + " dB off at +1.2 s");
            logMessage ("  first-visit: settled " + juce::String (settled, 2) + " dB (from " + juce::String (mk.front(), 2)
                        + "), overshoot " + juce::String (overshoot, 2) + " dB");
        }
        const float mkTube = mk.back();

        //================================================================ 3. repeated A/B snaps
        beginTest ("repeated A/B: the snap lands INSTANTLY at the remembered makeup");
        for (int round = 0; round < 3; ++round)
        {
            // We are IN tube after phase 2, so each round goes capture first, then tube.
            for (int toTube = 0; toTube <= 1; ++toTube)
            {
                const bool tube = toTube == 1;
                mk.clear();
                drive (e, p, tube, blocksFor (0.8), idx, &mk);
                const float want = tube ? mkTube : mkCap0;
                const size_t at30 = (size_t) juce::jmin ((int) mk.size() - 1, blocksFor (0.030));
                expect (std::fabs (mk[at30] - want) < 0.75f,
                        juce::String (tube ? "->tube" : "->capture") + " round " + juce::String (round)
                        + ": makeup " + juce::String (mk[at30], 2) + " dB at +30 ms, want ~" + juce::String (want, 2)
                        + " (instant snap missing)");
            }
        }

        //================================================================ 3b. the FIELD BUG regression
        // Non-stationary material: the leveler's target wanders, so a settled()-style write gate
        // never lets caches form and every switch re-fades from scratch. Contract: after one
        // exploratory cycle ON BURSTS, the A/B snap must land within one hop, wobble allowance
        // included. FAILS on the settled()-gated build.
        beginTest ("repeated A/B on BURSTS (non-stationary): caches must form and snap instantly");
        {
            long long bidx = 1;
            drive (e, p, false, blocksFor (1.2), bidx, nullptr, true);   // capture on bursts (dwell+cache)
            drive (e, p, true,  blocksFor (1.2), bidx, nullptr, true);   // tube on bursts (dwell+cache)
            const float mkTubeB = dB (e.autoLevelGain());
            drive (e, p, false, blocksFor (0.9), bidx, nullptr, true);   // back (re-dwell capture)
            mk.clear();
            drive (e, p, true,  blocksFor (0.6), bidx, &mk, true);       // the measured switch
            const size_t at30 = (size_t) juce::jmin ((int) mk.size() - 1, blocksFor (0.030));
            expect (std::fabs (mk[at30] - mkTubeB) < 1.25f,
                    "bursts A/B: makeup " + juce::String (mk[at30], 2) + " dB at +30 ms, want ~"
                    + juce::String (mkTubeB, 2) + " ±wobble (cache never formed on live material?)");
        }

        //================================================================ 4. rapid toggle abuse
        beginTest ("rapid A/B toggling (10 Hz) stays finite, inside the mode envelope, and recovers");
        {
            // Each accepted switch snaps INSTANTLY toward the other route's remembered value, so
            // the honest picture under abuse is: values jump between the two mode makeups, never
            // outside that envelope (+1.5 dB live-wobble slack), always finite, full recovery.
            const float envLo = juce::jmin (mkCap0, mkTube) - 1.5f;
            const float envHi = juce::jmax (mkCap0, mkTube) + 1.5f;
            for (int i = 0; i < 20; ++i)
            {
                mk.clear();
                drive (e, p, (i & 1) == 0, blocksFor (0.1), idx, &mk);
                for (float v : mk)
                {
                    expect (std::isfinite (v), "non-finite makeup under toggle abuse");
                    expect (v > envLo && v < envHi,
                            "makeup " + juce::String (v, 2) + " dB left the mode envelope ["
                            + juce::String (envLo, 2) + ", " + juce::String (envHi, 2) + "]");
                }
            }
            mk.clear();
            drive (e, p, true, blocksFor (1.0), idx, &mk);           // recover in tube
            expect (std::fabs (mk.back() - mkTube) < 0.5f, "makeup did not recover after toggle abuse");
        }

        //================================================================ 5. stale context, live delta
        beginTest ("after a context change the switch snaps to (current + learned delta), near the truth");
        {
            drive (e, p, false, blocksFor (1.0), idx, nullptr);      // converge + dwell capture
            p.slot[0].dryWet01 = 0.55f;                              // context change while IN capture
            drive (e, p, false, blocksFor (0.8), idx, nullptr);      // re-dwell under the new context
            mk.clear();
            drive (e, p, true, blocksFor (1.4), idx, &mk);           // switch: tube CACHE is stale,
                                                                     // but the capture->tube DELTA is learned
            const float settledT = mk.back();                        // the true new-context tube makeup
            // The delta is an ESTIMATE (learned full-wet, applied post-dilution — worst case ≈
            // 2.3 dB error here). The snap reseeds the followers at the estimate, so the residual
            // decays at the follower τ = 150 ms: expect ~err·e^(−t/τ) + live wobble. Bounds are
            // that physics with margin — the user-facing story is "instant near-truth landing,
            // residual gone in ~0.3 s" (vs the 4-6 dB half-second fade from scratch this design
            // replaced).
            const size_t at150 = (size_t) juce::jmin ((int) mk.size() - 1, blocksFor (0.150));
            const size_t at300 = (size_t) juce::jmin ((int) mk.size() - 1, blocksFor (0.300));
            expect (std::fabs (mk[at150] - settledT) < 1.3f,
                    "post-context-change switch: makeup " + juce::String (mk[at150], 2)
                    + " dB at +150 ms vs settled " + juce::String (settledT, 2) + " (want < 1.3)");
            expect (std::fabs (mk[at300] - settledT) < 0.7f,
                    "post-context-change switch: makeup " + juce::String (mk[at300], 2)
                    + " dB at +300 ms vs settled " + juce::String (settledT, 2) + " (want < 0.7)");
            // and the residual must finish converging (no long crawl left behind)
            const size_t at800 = (size_t) juce::jmin ((int) mk.size() - 1, blocksFor (0.800));
            expect (std::fabs (mk[at800] - settledT) < 0.4f,
                    "residual after the delta snap did not converge (" + juce::String (mk[at800], 2)
                    + " vs " + juce::String (settledT, 2) + ")");
        }
       #endif
    }
};

static PowerAmpSwitchLevelTest powerAmpSwitchLevelTest;
