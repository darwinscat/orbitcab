// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for cab::AmpEq — the amp tone stack (teq::EqEngine wrapper) that runs
// between the preamp and poweramp NAM stages. Proves: master-off is a bit-exact passthrough;
// a Bass boost lifts the lows while leaving the highs alone; the HPF attenuates sub-bass but
// not the mids; stereo channels are processed identically; and nothing produces NaN/inf.
#include <juce_audio_basics/juce_audio_basics.h>

#include "core/AmpEq.h"
#include "core/Params.h"

#include <cmath>
#include <vector>

using namespace cab;

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kSr = 48000.0;

    std::vector<float> sine (int n, double rate, double f, float amp = 0.5f)
    {
        std::vector<float> v ((size_t) n);
        for (int i = 0; i < n; ++i) v[(size_t) i] = amp * (float) std::sin (2.0 * kPi * f * i / rate);
        return v;
    }

    // RMS over the tail [start, end) — skip the head so biquad transients have settled.
    double rms (const std::vector<float>& v, size_t start)
    {
        double e = 0.0; size_t n = 0;
        for (size_t i = start; i < v.size(); ++i) { e += (double) v[i] * v[i]; ++n; }
        return n ? std::sqrt (e / (double) n) : 0.0;
    }

    bool anyBad (const std::vector<float>& v)
    { for (float x : v) if (std::isnan (x) || std::isinf (x)) return true; return false; }

    // Run a mono signal through AmpEq in `block`-sized chunks, in place; return the output.
    std::vector<float> runMono (const EqParams& eq, std::vector<float> sig, int block = 128)
    {
        AmpEq fx;
        fx.prepare (kSr, block, 1);
        for (size_t i = 0; i < sig.size(); i += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, sig.size() - i);
            float* ch[1] = { sig.data() + i };
            fx.process (ch, 1, n, eq);
        }
        return sig;
    }
}

struct AmpEqTest : juce::UnitTest
{
    AmpEqTest() : juce::UnitTest ("AmpEq") {}

    void runTest() override
    {
        beginTest ("master off → bit-exact passthrough");
        {
            EqParams eq;                 // on=false; plus a boost that MUST be ignored while off
            eq.bassDb = 6.0f; eq.trebleDb = -4.0f; eq.hpfOn = true; eq.hpfHz = 150.0f;
            const auto in  = sine (8192, kSr, 220.0);
            const auto out = runMono (eq, in);
            bool exact = true;
            for (size_t i = 0; i < in.size(); ++i) if (out[i] != in[i]) { exact = false; break; }
            expect (exact, "output must be untouched when eq.on is false");
        }

        beginTest ("Bass +6 lifts the lows, leaves the highs");
        {
            EqParams eq; eq.on = true; eq.bassDb = 6.0f;
            const auto lowIn  = sine (16384, kSr, 50.0);     // well below the 100 Hz shelf corner
            const auto highIn = sine (16384, kSr, 5000.0);   // well above it
            const double lowRatio  = rms (runMono (eq, lowIn),  8192) / rms (lowIn,  8192);
            const double highRatio = rms (runMono (eq, highIn), 8192) / rms (highIn, 8192);
            expect (lowRatio  > 1.3,  "50 Hz should be clearly boosted by a +6 dB low shelf");
            expect (highRatio > 0.9 && highRatio < 1.1, "5 kHz should be ~unchanged by the bass knob");
        }

        beginTest ("HPF attenuates sub-bass, spares the mids");
        {
            EqParams eq; eq.on = true; eq.hpfOn = true; eq.hpfHz = 120.0f;
            const auto subIn = sine (16384, kSr, 40.0);      // ~1.6 oct below corner → ~19 dB down
            const auto midIn = sine (16384, kSr, 1000.0);    // well in the passband
            const double subRatio = rms (runMono (eq, subIn), 8192) / rms (subIn, 8192);
            const double midRatio = rms (runMono (eq, midIn), 8192) / rms (midIn, 8192);
            expect (subRatio < 0.5,  "40 Hz should be strongly attenuated by a 120 Hz HPF");
            expect (midRatio > 0.9 && midRatio < 1.1, "1 kHz should pass the HPF ~unchanged");
        }

        beginTest ("stereo: both channels processed identically");
        {
            EqParams eq; eq.on = true; eq.bassDb = 4.0f; eq.presenceDb = 3.0f; eq.lpfOn = true; eq.lpfHz = 8000.0f;
            const int n = 8192, block = 128;
            auto base = sine (n, kSr, 300.0);
            std::vector<float> L = base, R = base;
            AmpEq fx; fx.prepare (kSr, block, 2);
            for (size_t i = 0; i < (size_t) n; i += (size_t) block)
            {
                const int m = (int) std::min ((size_t) block, (size_t) n - i);
                float* ch[2] = { L.data() + i, R.data() + i };
                fx.process (ch, 2, m, eq);
            }
            bool equal = true;
            for (int i = 0; i < n; ++i) if (L[(size_t) i] != R[(size_t) i]) { equal = false; break; }
            expect (equal, "identical L/R input must yield identical L/R output");
            expect (! anyBad (L) && ! anyBad (R), "no NaN/inf");
        }

        beginTest ("no NaN/inf with everything engaged");
        {
            EqParams eq; eq.on = true;
            eq.bassDb = 9.0f; eq.midDb = -6.0f; eq.trebleDb = 5.0f; eq.presenceDb = 4.0f;
            eq.hpfOn = true; eq.hpfHz = 90.0f; eq.lpfOn = true; eq.lpfHz = 9000.0f;
            const auto out = runMono (eq, sine (16384, kSr, 440.0));
            expect (! anyBad (out), "full EQ must stay finite");
        }
    }
};

static AmpEqTest ampEqTest;
