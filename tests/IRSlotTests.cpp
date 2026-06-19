// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for cab::IRSlot — the TRIM / HEAD math is synchronous (the trimmed
// length is computed before the async convolver swap), so it's deterministically testable.
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "core/IRSlot.h"

#include <cmath>
#include <vector>

using namespace cab;

struct IRSlotTest : juce::UnitTest
{
    IRSlotTest() : juce::UnitTest ("IRSlot") {}

    void runTest() override
    {
        const double sr = 48000.0;
        const int    block = 512;

        beginTest ("trim fraction maps to length (no head)");
        {
            IRSlot s; s.prepare (sr, block, 1);
            std::vector<float> orig ((size_t) 1000, 0.5f);
            const float* p[1] = { orig.data() };
            s.setOriginalIR (p, 1, 1000, sr);

            const double full = s.applyTrim (false, 1.0f, false);
            expectWithinAbsoluteError<double> (full, 1000.0 / sr, 1.0 / sr);
            const double quarter = s.applyTrim (true, 0.25f, false);
            expectWithinAbsoluteError<double> (quarter, 250.0 / sr, 2.0 / sr);
        }

        beginTest ("trim clamps to >= ~1 ms");
        {
            IRSlot s; s.prepare (sr, block, 1);
            std::vector<float> orig ((size_t) 1000, 0.5f);
            const float* p[1] = { orig.data() };
            s.setOriginalIR (p, 1, 1000, sr);

            const double tiny   = s.applyTrim (true, 0.0f, false);     // frac 0 -> clamp up
            const int    minLen = juce::jmax (16, (int) (0.001 * sr)); // ~1 ms = 48 @ 48k
            expectWithinAbsoluteError<double> (tiny, (double) minLen / sr, 2.0 / sr);
        }

        beginTest ("coalesce: an identical reload keeps the same length");
        {
            IRSlot s; s.prepare (sr, block, 1);
            std::vector<float> orig ((size_t) 1000, 0.5f);
            const float* p[1] = { orig.data() };
            s.setOriginalIR (p, 1, 1000, sr);

            const double a1 = s.applyTrim (true, 0.5f, false);
            const double a2 = s.applyTrim (true, 0.5f, false);
            expectWithinAbsoluteError<double> (a1, a2, 1.0e-9);
        }

        beginTest ("head trim removes the detected leading silence");
        {
            IRSlot s; s.prepare (sr, block, 1);
            const int lead = 100, body = 900;
            std::vector<float> orig ((size_t) (lead + body), 0.0f);
            for (int i = 0; i < body; ++i)
            {
                const float t = (float) i / (float) sr;
                orig[(size_t) (lead + i)] = std::cos (juce::MathConstants<float>::twoPi * 800.0f * t)
                                            * std::exp (-t * 30.0f);
            }
            const float* p[1] = { orig.data() };
            s.setOriginalIR (p, 1, lead + body, sr);

            const double full   = s.applyTrim (false, 1.0f, false);
            const double headed  = s.applyTrim (false, 1.0f, true);
            expect (headed < full, "head trim did not shorten the IR");

            const int preRoll = (int) (0.0002 * sr);     // detector backs off ~0.2 ms
            expectWithinAbsoluteError<double> (full - headed, (double) (lead - preRoll) / sr, 5.0 / sr);
        }
    }
};

static IRSlotTest irSlotTest;
