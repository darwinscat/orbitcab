// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for the NAM amp stage. The risky new piece is the streaming rate-match
// resampler (cab::StreamResampler) — tested directly for identity, up/down conversion, no drift,
// and no allocation-induced glitches. AmpStage itself is tested for clean passthrough with no model.
#include <juce_audio_basics/juce_audio_basics.h>

#include "core/StreamResampler.h"
#include "core/AmpStage.h"

#include <cmath>
#include <vector>

using namespace cab;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // Run a signal through a resampler in `block`-sized chunks; return the concatenated output.
    std::vector<float> runResampler (double inRate, double outRate, const std::vector<float>& in, int block)
    {
        StreamResampler r;
        r.reset (inRate, outRate, block * 2 + 16);
        std::vector<float> out, tmp ((size_t) (block * 4 + 16));
        out.reserve ((size_t) ((double) in.size() * outRate / inRate) + 32);
        for (size_t i = 0; i < in.size(); i += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, in.size() - i);
            r.feed (in.data() + i, n);
            const int got = r.produceAvailable (tmp.data(), (int) tmp.size());
            out.insert (out.end(), tmp.begin(), tmp.begin() + got);
        }
        return out;
    }

    bool anyBad (const std::vector<float>& v)
    { for (float x : v) if (std::isnan (x) || std::isinf (x)) return true; return false; }

    float peak (const std::vector<float>& v)
    { float p = 0; for (float x : v) p = std::max (p, std::fabs (x)); return p; }

    std::vector<float> sine (int n, double rate, double f, float amp = 0.5f)
    {
        std::vector<float> v ((size_t) n);
        for (int i = 0; i < n; ++i) v[(size_t) i] = amp * (float) std::sin (2.0 * kPi * f * i / rate);
        return v;
    }
}

struct AmpStageTest : juce::UnitTest
{
    AmpStageTest() : juce::UnitTest ("AmpStage") {}

    void runTest() override
    {
        beginTest ("resampler identity at ratio 1 (clean 2-sample delay)");
        {
            auto in = sine (4000, 48000.0, 600.0);
            auto out = runResampler (48000.0, 48000.0, in, 512);
            expect (! anyBad (out));
            int matched = 0, checked = 0;
            for (size_t k = 2; k + 2 < out.size() && k < in.size() && k < 2000; ++k, ++checked)
                if (std::abs (out[k] - in[k - 2]) < 1.0e-4f) ++matched;          // catmull@t=0 → 2-sample delay
            expect (checked > 1000 && matched > checked - 4);
        }

        beginTest ("upsample 44100 -> 48000 (no NaN, level kept, count ~ ratio)");
        {
            auto in = sine (44100, 44100.0, 100.0);
            auto out = runResampler (44100.0, 48000.0, in, 512);
            expect (! anyBad (out));
            expectWithinAbsoluteError<float> (peak (out), 0.5f, 0.04f);
            expectWithinAbsoluteError<float> ((float) out.size() / (float) in.size(), 48000.0f / 44100.0f, 0.01f);
        }

        beginTest ("downsample 96000 -> 48000 (no NaN, level kept, count ~ half)");
        {
            auto in = sine (96000, 96000.0, 100.0);
            auto out = runResampler (96000.0, 48000.0, in, 512);
            expect (! anyBad (out));
            expectWithinAbsoluteError<float> (peak (out), 0.5f, 0.04f);
            expectWithinAbsoluteError<float> ((float) out.size() / (float) in.size(), 0.5f, 0.01f);
        }

        beginTest ("no drift on DC over many blocks");
        {
            std::vector<float> in (48000, 1.0f);
            auto out = runResampler (44100.0, 48000.0, in, 512);
            expect (! anyBad (out));
            double tail = 0; int c = 0;
            for (size_t k = (out.size() > 256 ? out.size() - 256 : 0); k < out.size(); ++k) { tail += out[k]; ++c; }
            expectWithinAbsoluteError<float> (c ? (float) (tail / c) : 0.0f, 1.0f, 0.02f);
        }

        beginTest ("AmpStage passthrough + zero latency with no model");
        {
            AmpStage amp;
            amp.prepare (48000.0, 512);
            auto L = sine (512, 48000.0, 220.0, 0.3f), R = L;
            const auto L0 = L, R0 = R;
            float* io[2] = { L.data(), R.data() };
            amp.process (io, 2, 512, true);
            expect (L == L0 && R == R0);          // no model loaded → signal untouched
            expect (amp.latencySamples() == 0);
        }
    }
};

static AmpStageTest ampStageTest;
