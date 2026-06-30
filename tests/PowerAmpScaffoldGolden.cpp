// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa. Part of OrbitCab — see LICENSE.
//
// Headless golden test: cab::poweramp::TubePowerAmp scaffold (block 1) is a STRICT pass-through.
// JUCE-free — proves the new poweramp stage is bit-exact identity and zero-latency BEFORE any
// DSP lands, so later blocks (sag / waveshaper / NFB / load) measure their deltas against a known-
// clean baseline. CI gate (returns non-zero on any failure). Mirrors CoreConvolverGolden style.

#include "poweramp/TubePowerAmp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using cab::poweramp::TubePowerAmp;

namespace
{
int g_checks = 0, g_fail = 0;
void check (bool ok, const char* msg)
{
    ++g_checks;
    if (! ok) { ++g_fail; std::printf ("  [FAIL] %s\n", msg); }
    else        std::printf ("  [ok]   %s\n", msg);
}

struct Lcg { unsigned long long s; float next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (float) ((s >> 40) & 0xFFFFFF) / 8388608.0f - 1.0f; } };

// Run `total` samples through the stage in a hostile variable-block sweep (all <= maxBlock).
double runSweep (TubePowerAmp& dut, std::vector<float>& L, std::vector<float>& R,
                 const std::vector<float>& L0, const std::vector<float>& R0, int total)
{
    static const int blk[] = { 1, 17, 64, 128, 333, 512 };
    int pos = 0, bi = 0;
    while (pos < total) { const int n = std::min (blk[bi % 6], total - pos); float* io[2] { L.data() + pos, R.data() + pos }; dut.process (io, 2, n); pos += n; ++bi; }
    double mx = 0.0;
    for (int n = 0; n < total; ++n) { mx = std::max (mx, (double) std::fabs (L[(std::size_t) n] - L0[(std::size_t) n])); mx = std::max (mx, (double) std::fabs (R[(std::size_t) n] - R0[(std::size_t) n])); }
    return mx;
}
}

int main()
{
    std::printf ("OrbitCab TubePowerAmp scaffold golden (pass-through identity)\n");
    const double sr = 48000.0;
    const int maxBlock = 512;
    const int total = 8192;

    TubePowerAmp dut;
    dut.prepare (sr, maxBlock);
    check (dut.latencySamples() == 0, "scaffold latency == 0");

    std::vector<float> L0 ((std::size_t) total), R0 ((std::size_t) total);
    { Lcg a { 12345 }, b { 67890 }; for (int n = 0; n < total; ++n) { L0[(std::size_t) n] = 0.7f * a.next(); R0[(std::size_t) n] = 0.7f * b.next(); } }

    // --- pass-through is bit-exact under a variable-block sweep ---
    {
        std::vector<float> L = L0, R = R0;
        check (runSweep (dut, L, R, L0, R0, total) == 0.0, "pass-through is bit-exact (max-abs diff == 0)");
    }

    // --- still identity after reset() ---
    {
        dut.reset();
        std::vector<float> L = L0, R = R0;
        check (runSweep (dut, L, R, L0, R0, total) == 0.0, "pass-through after reset() is bit-exact");
    }

    // --- mono (1 channel) is also bit-exact ---
    {
        TubePowerAmp mono; mono.prepare (sr, maxBlock);
        std::vector<float> M = L0; float* io[1] { M.data() }; mono.process (io, 1, total);
        double d = 0.0; for (int n = 0; n < total; ++n) d = std::max (d, (double) std::fabs (M[(std::size_t) n] - L0[(std::size_t) n]));
        check (d == 0.0, "mono pass-through is bit-exact");
    }

    std::printf ("%d checks, %d failures\n", g_checks, g_fail);
    std::printf (g_fail ? "GOLDEN FAILED\n" : "GOLDEN PASSED\n");
    return g_fail ? 1 : 0;
}
