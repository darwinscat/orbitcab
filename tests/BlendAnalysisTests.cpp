// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for cab::blend — the offline A/B blend analysis behind auto-polarity
// (felitronics::measurement::xcorrAlign) and the MIX interference tint (offline curves).
// Deterministic: IRs are LCG noise bursts with an exponential decay, no files, no engine.
#include <juce_core/juce_core.h>

#include "core/BlendAnalysis.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace cab;

struct BlendAnalysisTest : juce::UnitTest
{
    BlendAnalysisTest() : juce::UnitTest ("BlendAnalysis") {}

    // Deterministic exp-decaying noise burst — a stand-in cab IR. `delay` prepends silence
    // (a residual pre-delay); `gain` scales (polarity via a negative gain).
    static std::vector<float> burst (std::uint32_t seed, int len, int delay, float gain)
    {
        std::vector<float> v ((size_t) (len + delay), 0.0f);
        std::uint32_t s = seed;
        for (int i = 0; i < len; ++i)
        {
            s = s * 1664525u + 1013904223u;                            // LCG (deterministic across platforms)
            const float white = ((float) (s >> 8) / 8388608.0f) - 1.0f;   // [-1, 1)
            v[(size_t) (delay + i)] = gain * white * std::exp (-6.0f * (float) i / (float) len);
        }
        return v;
    }

    void runTest() override
    {
        const double sr = 48000.0;
        const int    len = 4096;

        beginTest ("suggestPolarityInvert: inverted, slightly delayed copy -> invert");
        {
            const auto a = burst (1234u, len, 0, 1.0f);
            const auto b = burst (1234u, len, 37, -1.0f);              // same IR, flipped + 37 samples late
            const auto r = blend::suggestPolarityInvert ({ a }, { b }, sr);
            expect (r.has_value(), "confident suggestion expected for a related pair");
            if (r.has_value()) expect (*r, "the flip must be detected");
        }

        beginTest ("suggestPolarityInvert: identical copy -> no flip");
        {
            const auto a = burst (1234u, len, 0, 1.0f);
            const auto r = blend::suggestPolarityInvert ({ a }, { a }, sr);
            expect (r.has_value() && ! *r, "identical IRs are in phase");
        }

        beginTest ("suggestPolarityInvert: onset delta beyond the search range -> no suggestion");
        {
            const auto a = burst (1234u, len, 0, 1.0f);
            const auto b = burst (1234u, len, 480, -1.0f);             // 10 ms late > kMaxLagSeconds (5 ms)
            expect (! blend::suggestPolarityInvert ({ a }, { b }, sr).has_value(),
                    "xcorrAlign must refuse (corr = 0) — never guess out of range");
        }

        beginTest ("suggestPolarityInvert: degenerate inputs -> no suggestion");
        {
            const auto a = burst (1234u, len, 0, 1.0f);
            expect (! blend::suggestPolarityInvert ({}, { a }, sr).has_value());
            expect (! blend::suggestPolarityInvert ({ a }, { std::vector<float> (16, 0.1f) }, sr).has_value());
            expect (! blend::suggestPolarityInvert ({ a }, { a }, 0.0).has_value());
        }

        beginTest ("effectiveMixAB: mute-solo override mirrors the engine");
        {
            expectWithinAbsoluteError (blend::effectiveMixAB (0.3f, false, false, true),  0.3f, 1.0e-6f);
            expectWithinAbsoluteError (blend::effectiveMixAB (0.3f, true,  false, true),  1.0f, 1.0e-6f);   // A muted -> full B
            expectWithinAbsoluteError (blend::effectiveMixAB (0.3f, false, true,  true),  0.0f, 1.0e-6f);   // B muted -> full A
            expectWithinAbsoluteError (blend::effectiveMixAB (0.3f, false, false, false), 0.0f, 1.0e-6f);   // B absent -> full A
        }

        // A shared 50/50 fixture: identical delta IRs — every frequency coherent.
        SlotParams flatA, flatB;                                       // defaults: no filters, no phase, wet 1.0

        beginTest ("interferenceCurve: B = A at 50/50 -> +3 dB coherent gain everywhere");
        {
            std::vector<float> delta (512, 0.0f); delta[0] = 1.0f;
            const auto curve = blend::interferenceCurve ({ delta }, { delta }, flatA, flatB, 0.5f, true, sr);
            expectEquals ((int) curve.size(), blend::kPoints);
            for (const double v : curve)
                expectWithinAbsoluteError (v, 3.01, 0.15);             // 2 coherent halves vs incoherent power sum
        }

        beginTest ("interferenceCurve: B = -A cancels; the phase param rescues it");
        {
            std::vector<float> delta (512, 0.0f); delta[0] = 1.0f;
            auto inv = delta; for (auto& v : inv) v = -v;

            const auto comb = blend::interferenceCurve ({ delta }, { inv }, flatA, flatB, 0.5f, true, sr);
            expectEquals ((int) comb.size(), blend::kPoints);
            for (const double v : comb)
                expect (v < -20.0, "a perfect anti-phase 50/50 must show deep cancellation");

            SlotParams flipped = flatB; flipped.phase = true;          // the auto-polarity fix, applied
            const auto fixed = blend::interferenceCurve ({ delta }, { inv }, flatA, flipped, 0.5f, true, sr);
            for (const double v : fixed)
                expectWithinAbsoluteError (v, 3.01, 0.15);
        }

        beginTest ("interferenceCurve: single-sided blends -> empty (no tint)");
        {
            std::vector<float> delta (512, 0.0f); delta[0] = 1.0f;
            expect (blend::interferenceCurve ({ delta }, { delta }, flatA, flatB, 0.0f, true,  sr).empty());   // full A
            expect (blend::interferenceCurve ({ delta }, { delta }, flatA, flatB, 1.0f, true,  sr).empty());   // full B
            expect (blend::interferenceCurve ({ delta }, { delta }, flatA, flatB, 0.5f, false, sr).empty());   // B absent
            SlotParams mutedB = flatB; mutedB.mute = true;
            expect (blend::interferenceCurve ({ delta }, { delta }, flatA, mutedB, 0.5f, true, sr).empty());   // B muted -> solo A
            expect (blend::interferenceCurve ({}, { delta }, flatA, flatB, 0.5f, true, sr).empty());           // no taps
        }

        beginTest ("interferenceCurve: a slot HPF thins the blend's low-band interference");
        {
            // Same IR on both sides (coherent +3 dB); HPF on B removes its lows -> in the low
            // band only A remains audible-in-power, so the interference relaxes toward 0 dB
            // there while the top stays ~+3 dB. Trend check, not exact dB.
            const auto ir = burst (777u, len, 0, 1.0f);
            SlotParams hpfB = flatB; hpfB.hpfOn = true; hpfB.hpfHz = 400.0f;
            const auto curve = blend::interferenceCurve ({ ir }, { ir }, flatA, hpfB, 0.5f, true, sr);
            expectEquals ((int) curve.size(), blend::kPoints);
            expect (curve.front() < 1.5, "low band: B is filtered out -> interference must relax");
            expectWithinAbsoluteError (curve.back(), 3.01, 0.5);       // top band: still fully coherent
        }
    }
};

static BlendAnalysisTest blendAnalysisTest;
