// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

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

        // The MONO-CPU contract that CabEngine's frontCh path relies on: with a real model loaded,
        // process(numChannels=1) must run EXACTLY ONE nam::DSP instance and NEVER touch ch1 — that is
        // the whole ½-CPU win (fatal-on-old-laptop if it silently runs both). A distinct ch1 sentinel
        // that stays bit-exact proves the 2nd instance did not run; ch1 changing = the mono lane count
        // regressed (e.g. someone passed numCh instead of frontCh). numChannels=2 must run both.
        beginTest ("NAM lane contract: mono runs one instance (ch1 untouched); stereo runs both");
        {
           #ifdef ORBITCAB_RES_DIR
            const juce::File nf = juce::File (ORBITCAB_RES_DIR).getChildFile ("preamps/V4KRAK-red-12h.namz");
            expect (nf.existsAsFile(), "test resource .nam must exist: " + nf.getFullPathName());
            juce::MemoryBlock mb; if (nf.existsAsFile()) nf.loadFileAsData (mb);
            if (mb.getSize() > 0)
            {
                AmpStage amp; amp.prepare (48000.0, 512);
                expect (amp.loadModelFromMemory (mb.getData(), mb.getSize()), "48k factory .nam loads (live synchronously)");

                {   // MONO: numChannels=1 → one instance, ch1 must be left exactly as-is
                    auto a = sine (512, 48000.0, 220.0, 0.3f);
                    auto b = sine (512, 48000.0, 330.0, 0.4f);   // ch1 SENTINEL (a NAM would visibly alter it)
                    const auto a0 = a, b0 = b;
                    float* io[2] = { a.data(), b.data() };
                    amp.process (io, 1, 512, /*normalize*/ true);
                    expect (b == b0, "mono (numChannels=1) wrote ch1 — a 2nd NAM ran; the ½-CPU contract is broken");
                    expect (a != a0, "mono lane did not process ch0 — the model is not running");
                }
                {   // STEREO: numChannels=2 → both instances run, ch1 processed
                    auto a = sine (512, 48000.0, 220.0, 0.3f);
                    auto b = sine (512, 48000.0, 330.0, 0.4f);
                    const auto b0 = b;
                    float* io[2] = { a.data(), b.data() };
                    amp.process (io, 2, 512, /*normalize*/ true);
                    expect (b != b0, "stereo (numChannels=2) must process ch1 via the 2nd instance");
                }
            }
           #endif
        }
    }
};

static AmpStageTest ampStageTest;
