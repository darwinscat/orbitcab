// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for cab::AutoLeveler — the wet->dry match is pure-numeric,
// so its convergence / clamping / silence-gate are trivially checkable.
#include <juce_audio_basics/juce_audio_basics.h>

#include "core/AutoLeveler.h"

using namespace cab;

namespace
{
    // Drive the follower with a fixed dry/mix mean-square for many blocks and return the
    // settled gain (getNextGain advances the smoother one step per call).
    float settle (double dryMS, double mixMS, bool enabled, int blocks)
    {
        AutoLeveler a;
        a.prepare (48000.0, 0.03);
        for (int i = 0; i < blocks; ++i)
        {
            a.processBlock (dryMS, mixMS, enabled, 512);
            a.getNextGain();
        }
        return a.currentGain();
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
        // dryMeanSq never crosses the -60 dBFS floor, so the target is never set -> unity.
        expectWithinAbsoluteError<float> (settle (1.0e-8, 1.0e-12, true, 3000), 1.0f, 0.01f);

        beginTest ("disabled aims for unity even with a loud mismatch");
        expectWithinAbsoluteError<float> (settle (1.0, 0.25, false, 3000), 1.0f, 0.02f);
    }
};

static AutoLevelerTest autoLevelerTest;
