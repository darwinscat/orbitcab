// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for cab::AutoLeveler — the wet->dry match is pure-numeric,
// so its convergence / clamping / silence-gate / rate limits are directly checkable.
//
// The rate-limit battery below is ADVERSARIAL: it exists to break the gain dynamics.
//   • the anti-freeze regression encodes the bug this design replaced (a SmoothedValue
//     re-armed every block against a slew-clamped target compounded into a block-size-
//     dependent crawl: 0.4 dB/s @ 64-sample blocks instead of the designed 9 dB/s);
//   • the block-size / sample-rate invariance checks pin the "invariant dynamics" contract;
//   • the slope probes assert the hard dB/s ceilings SAMPLE BY SAMPLE (no pump, ever);
//   • the snap tests pin the deterministic-retarget contract (jump target, fast glide,
//     followers re-seeded so the ratio STICKS — no pull-back toward the old spectrum).
// Never loosen these to go green.
#include <juce_audio_basics/juce_audio_basics.h>

#include "core/AutoLeveler.h"
#include <cmath>
#include <vector>

using namespace cab;

namespace
{
    // Drive the follower with a fixed dry/mix mean-square for many blocks and return the
    // settled gain (getNextGain advances the applied gain one sample per call).
    float settle (double dryMS, double mixMS, bool enabled, int blocks)
    {
        AutoLeveler a;
        a.prepare (48000.0);
        for (int i = 0; i < blocks; ++i)
        {
            a.processBlock (dryMS, mixMS, enabled, 512);
            for (int s = 0; s < 512; ++s) a.getNextGain();
        }
        return a.currentGain();
    }

    // Run `seconds` of a constant (dry, mix) scenario at the given block size / rate and
    // return the applied-gain trajectory in dB, sampled once per block end.
    struct Traj { std::vector<float> db; int blk; double sr; };
    Traj run (double dryMS, double mixMS, int blk, double sr, double seconds,
              float startGain = 1.0f)
    {
        AutoLeveler a;
        a.prepare (sr);
        if (startGain != 1.0f) a.seed (startGain);
        Traj t; t.blk = blk; t.sr = sr;
        const int blocks = (int) std::llround (seconds * sr / blk);
        for (int i = 0; i < blocks; ++i)
        {
            a.processBlock (dryMS, mixMS, true, blk);
            for (int s = 0; s < blk; ++s) a.getNextGain();
            t.db.push_back (juce::Decibels::gainToDecibels (a.currentGain(), -120.0f));
        }
        return t;
    }

    float atTime (const Traj& t, double sec)
    {
        const int i = juce::jlimit (0, (int) t.db.size() - 1,
                                    (int) std::llround (sec * t.sr / t.blk) - 1);
        return t.db[(size_t) i];
    }
}

struct AutoLevelerTest : juce::UnitTest
{
    AutoLevelerTest() : juce::UnitTest ("AutoLeveler") {}

    void runTest() override
    {
        beginTest ("converges to sqrt(dryMS / mixMS)");
        expectWithinAbsoluteError<float> (settle (1.0, 0.25, true, 3000), 2.0f, 0.05f);   // sqrt(4)=2

        beginTest ("clamps to the +36 dB ceiling");
        expectWithinAbsoluteError<float> (settle (1.0, 1.0e-9, true, 5000), 63.10f, 1.0f);

        beginTest ("clamps to the -24 dB floor");
        expectWithinAbsoluteError<float> (settle (1.0, 1.0e6, true, 5000), 0.0631f, 0.01f);

        beginTest ("silence-gated: sub-floor dry energy holds unity");
        expectWithinAbsoluteError<float> (settle (1.0e-8, 1.0e-12, true, 3000), 1.0f, 0.01f);

        beginTest ("disabled aims for unity even with a loud mismatch");
        expectWithinAbsoluteError<float> (settle (1.0, 0.25, false, 3000), 1.0f, 0.02f);

        //------------------------------------------------------------------ rate-limit battery
        // Scenario shared below: a −12.7 dB correct makeup (dry=1, mix=10^(12.7/10)) from unity.
        const double mixFor127 = std::pow (10.0, 12.7 / 10.0);

        beginTest ("ANTI-FREEZE regression: 64-sample blocks converge at the designed rate");
        {
            // The replaced design crawled at ~0.4 dB/s here (−3.2 dB after 8 s). The follower
            // (150 ms) + the 9 dB/s slew must land −12.7 dB in ≤ 12.7/9 + margin ≈ 2.0 s.
            const auto t = run (1.0, mixFor127, 64, 48000.0, 2.5);
            expectWithinAbsoluteError<float> (atTime (t, 2.2), -12.7f, 0.35f,
                "64-sample blocks: leveler frozen (the SmoothedValue double-limiter bug)");
        }

        beginTest ("block-size invariance of the gain trajectory");
        {
            const auto ref = run (1.0, mixFor127, 512, 48000.0, 2.5);
            for (int blk : { 16, 64, 173, 2048 })
            {
                const auto t = run (1.0, mixFor127, blk, 48000.0, 2.5);
                for (double sec : { 0.5, 1.0, 1.5, 2.2 })
                    expectWithinAbsoluteError<float> (atTime (t, sec), atTime (ref, sec), 0.25f,
                        "blk=" + juce::String (blk) + " diverges from blk=512 @ " + juce::String (sec) + " s");
            }
        }

        beginTest ("sample-rate invariance of the gain trajectory (44.1/96/192 kHz)");
        {
            const auto ref = run (1.0, mixFor127, 512, 48000.0, 2.5);
            for (double sr : { 44100.0, 96000.0, 192000.0 })
            {
                const auto t = run (1.0, mixFor127, 512, sr, 2.5);
                for (double sec : { 0.5, 1.0, 2.2 })
                    expectWithinAbsoluteError<float> (atTime (t, sec), atTime (ref, sec), 0.30f,
                        "sr=" + juce::String (sr) + " diverges @ " + juce::String (sec) + " s");
            }
        }

        beginTest ("hard 9 dB/s ceiling, sample by sample, under a pathological target");
        {
            // Alternate the raw target ±20 dB every block — the applied gain must never move
            // faster than the normal per-sample step, and must stay bounded.
            // The raw block energies flip ±40 dB every 64 samples. The 150 ms followers must
            // integrate that to the mean-energy ratio (mix ≈ 50 ⇒ makeup ≈ −17 dB) and the
            // applied gain must (a) never move faster than 9 dB/s per sample on the way, and
            // (b) sit STILL there — no residual block-rate wobble from the flipping target.
            AutoLeveler a;
            a.prepare (48000.0);
            const float maxStep = (float) (9.0 / 48000.0) * 1.02f + 3.0e-6f;   // + float dB-conversion noise
            float prevDb = juce::Decibels::gainToDecibels (a.currentGain(), -120.0f);
            float lateMin = 1.0e9f, lateMax = -1.0e9f;
            const int blocks = 6000;                                           // 8 s — lands + dwells
            for (int i = 0; i < blocks; ++i)
            {
                const bool loud = (i & 1) != 0;
                a.processBlock (1.0, loud ? 100.0 : 0.01, true, 64);
                for (int s = 0; s < 64; ++s)
                {
                    const float db = juce::Decibels::gainToDecibels (a.getNextGain(), -120.0f);
                    expect (std::fabs (db - prevDb) <= maxStep, "per-sample slope exceeded 9 dB/s");
                    prevDb = db;
                }
                if (i >= blocks - 1500)   // the last 2 s: settled
                    { lateMin = juce::jmin (lateMin, prevDb); lateMax = juce::jmax (lateMax, prevDb); }
            }
            const float meanRatioDb = 10.0f * std::log10 (1.0 / ((100.0f + 0.01f) * 0.5f));
            expectWithinAbsoluteError<float> (prevDb, meanRatioDb, 0.6f,
                "did not converge to the mean-energy ratio");
            expect (lateMax - lateMin < 0.25f, "settled gain wobbles "
                                               + juce::String (lateMax - lateMin, 3) + " dB at the block rate");
        }

        beginTest ("snapRatioTo: fast bounded glide that lands and STICKS");
        {
            AutoLeveler a;
            a.prepare (48000.0);
            // converge at unity ratio first
            for (int i = 0; i < 2000; ++i) { a.processBlock (1.0, 1.0, true, 64); for (int s = 0; s < 64; ++s) a.getNextGain(); }
            expectWithinAbsoluteError<float> (a.currentGainDb(), 0.0f, 0.1f);

            // snap to −12 dB (the "other route's" converged makeup); the signal now REALLY has
            // that ratio (mix = dry·10^(12/10)) — i.e. the cache was correct.
            a.snapRatioTo (juce::Decibels::decibelsToGain (-12.0f));
            const float fastStep = (float) (40.0 / 48000.0) * 1.02f + 3.0e-6f;
            float prevDb = a.currentGainDb();
            int   landedAt = -1;
            const double mixFor12 = std::pow (10.0, 12.0 / 10.0);
            for (int i = 0; i < 1500; ++i)   // 2 s
            {
                a.processBlock (1.0, mixFor12, true, 64);
                for (int s = 0; s < 64; ++s)
                {
                    a.getNextGain();
                    const float db = a.currentGainDb();
                    expect (std::fabs (db - prevDb) <= fastStep, "snap glide exceeded 40 dB/s");
                    expect (db <= prevDb + 3.0e-6f, "snap glide must be monotonic here");
                    prevDb = db;
                }
                if (landedAt < 0 && std::fabs (a.currentGainDb() + 12.0f) < 0.05f)
                    landedAt = i;
            }
            expect (landedAt >= 0 && landedAt * 64 / 48000.0 < 0.35,
                    "snap did not land within the fast window");
            expectWithinAbsoluteError<float> (a.currentGainDb(), -12.0f, 0.15f,
                    "followers pulled the snapped makeup back toward the old spectrum");
        }

        beginTest ("snap during silence holds, then sticks when signal returns");
        {
            AutoLeveler a;
            a.prepare (48000.0);
            for (int i = 0; i < 2000; ++i) { a.processBlock (1.0, 1.0, true, 64); for (int s = 0; s < 64; ++s) a.getNextGain(); }
            a.snapRatioTo (juce::Decibels::decibelsToGain (-6.0f));
            for (int i = 0; i < 500; ++i)  { a.processBlock (1.0e-9, 1.0e-9, true, 64); for (int s = 0; s < 64; ++s) a.getNextGain(); }   // silence
            expectWithinAbsoluteError<float> (a.currentGainDb(), -6.0f, 0.15f, "silent snap didn't glide to target");
            const double mixFor6 = std::pow (10.0, 6.0 / 10.0);
            for (int i = 0; i < 2000; ++i) { a.processBlock (1.0, mixFor6, true, 64); for (int s = 0; s < 64; ++s) a.getNextGain(); }
            expectWithinAbsoluteError<float> (a.currentGainDb(), -6.0f, 0.25f, "makeup drifted after signal returned");
            expect (std::isfinite (a.currentGain()), "non-finite gain after silence");
        }

        beginTest ("enable/disable toggles glide fast (deterministic retarget)");
        {
            AutoLeveler a;
            a.prepare (48000.0);
            const double mixFor12 = std::pow (10.0, 12.0 / 10.0);
            for (int i = 0; i < 4000; ++i) { a.processBlock (1.0, mixFor12, true, 64); for (int s = 0; s < 64; ++s) a.getNextGain(); }
            expectWithinAbsoluteError<float> (a.currentGainDb(), -12.0f, 0.2f);
            // disable → unity within the snap window (not 12/9 ≈ 1.3 s)
            int n = 0;
            while (std::fabs (a.currentGainDb()) > 0.1f && n < 1500) { a.processBlock (1.0, mixFor12, false, 64); for (int s = 0; s < 64; ++s) a.getNextGain(); ++n; }
            expect (n * 64 / 48000.0 < 0.45, "disable took " + juce::String (n * 64 / 48000.0, 2) + " s to reach unity");
            // re-enable → back to −12 within the snap window
            n = 0;
            while (std::fabs (a.currentGainDb() + 12.0f) > 0.15f && n < 1500) { a.processBlock (1.0, mixFor12, true, 64); for (int s = 0; s < 64; ++s) a.getNextGain(); ++n; }
            expect (n * 64 / 48000.0 < 0.45, "re-enable took " + juce::String (n * 64 / 48000.0, 2) + " s to return");
        }
    }
};

static AutoLevelerTest autoLevelerTest;
