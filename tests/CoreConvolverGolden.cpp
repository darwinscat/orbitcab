// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa. Part of OrbitCab — see LICENSE.
//
// Headless golden test: cab::CoreConvolver (felitronics::convolution) vs a DIRECT-convolution
// reference, at host rate. Validates the JUCE-free convolution as a sound-preserving cab backend:
//   • impulse -> the IR itself (load + gain irSr/hostSr = 1 at host rate);
//   • mono IR broadcasts to L+R (JUCE Stereo::yes parity);
//   • noise == direct convolution under a hostile variable-block sweep (null RMS < -100 dBFS);
//   • a true-stereo IR convolves each channel with its own IR.
// JUCE-free — direct convolution is the ground truth; the live JUCE-vs-core ear A/B is the plugin.

#include "core/CoreConvolver.h"

#include <cmath>
#include <cstdio>
#include <vector>

using cab::CoreConvolver;

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

std::vector<float> directConv (const std::vector<float>& in, const std::vector<float>& ir)
{
    const int N = (int) in.size(), M = (int) ir.size();
    std::vector<float> out ((std::size_t) N, 0.0f);
    for (int n = 0; n < N; ++n) { double a = 0.0; for (int k = 0; k < M; ++k) { const int i = n - k; if (i >= 0 && i < N) a += (double) in[(std::size_t) i] * ir[(std::size_t) k]; } out[(std::size_t) n] = (float) a; }
    return out;
}
double nullRmsDb (const std::vector<float>& a, const std::vector<float>& b, int from, int to)
{
    double e = 0.0; int n = 0;
    for (int i = from; i < to; ++i) { const double d = (double) a[(std::size_t) i] - b[(std::size_t) i]; e += d * d; ++n; }
    return 20.0 * std::log10 (std::max (1e-12, n ? std::sqrt (e / n) : 0.0));
}
double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b, int from, int to)
{
    double m = 0.0; for (int i = from; i < to; ++i) m = std::max (m, std::fabs ((double) a[(std::size_t) i] - b[(std::size_t) i])); return m;
}

// process `total` samples through the convolver in a hostile variable block sweep (all <= maxBlock).
void runVariableBlocks (CoreConvolver<>& c, std::vector<float>& L, std::vector<float>& R, int total)
{
    static const int blk[] = { 1, 17, 64, 128, 333, 512 };
    int pos = 0, bi = 0;
    while (pos < total) { const int n = std::min (blk[bi % 6], total - pos); float* io[2] { L.data() + pos, R.data() + pos }; c.process (io, 2, n); pos += n; ++bi; }
}
}

int main()
{
    std::printf ("OrbitCab CoreConvolver golden (vs direct conv, host rate)\n");
    const double sr = 48000.0;
    const int irLen = 512;
    const double maxIrSec = 0.05;     // small -> short cold-start fade
    const int warm = 4096;            // > coldXfade -> measure past the cold fade
    const int testLen = 8192;
    const int total = warm + testLen;

    std::vector<float> ir ((std::size_t) irLen);
    { Lcg r { 12345 }; for (int n = 0; n < irLen; ++n) { const float env = std::exp (-3.0f * (float) n / irLen); ir[(std::size_t) n] = env * (0.5f * r.next() + (n == 0 ? 1.0f : 0.0f)); } }

    // --- Test 1: impulse -> IR (mono, gain 1) + mono broadcast to R ---
    {
        CoreConvolver<> c; c.prepare (sr, 512, 2, maxIrSec);
        const float* irp[1] { ir.data() }; c.loadIR (irp, 1, irLen, sr);
        std::vector<float> L ((std::size_t) total, 0.0f), R ((std::size_t) total, 0.0f);
        L[(std::size_t) warm] = 1.0f; R[(std::size_t) warm] = 1.0f;
        runVariableBlocks (c, L, R, total);
        double mL = 0.0, mR = 0.0;
        for (int k = 0; k < irLen; ++k) { mL = std::max (mL, std::fabs ((double) L[(std::size_t) (warm + k)] - ir[(std::size_t) k])); mR = std::max (mR, std::fabs ((double) R[(std::size_t) (warm + k)] - ir[(std::size_t) k])); }
        check (mL < 2e-4, "impulse -> IR (mono, gain=1) max-abs < 2e-4");
        check (mR < 2e-4, "mono IR broadcasts to R (JUCE Stereo::yes parity)");
    }

    // --- Test 2: noise == direct conv (mono IR, independent L/R input) ---
    {
        CoreConvolver<> c; c.prepare (sr, 512, 2, maxIrSec);
        const float* irp[1] { ir.data() }; c.loadIR (irp, 1, irLen, sr);
        std::vector<float> L ((std::size_t) total, 0.0f), R ((std::size_t) total, 0.0f), Lin ((std::size_t) total, 0.0f), Rin ((std::size_t) total, 0.0f);
        Lcg rl { 999 }, rr { 55555 };
        for (int n = warm; n < total; ++n) { L[(std::size_t) n] = Lin[(std::size_t) n] = 0.5f * rl.next(); R[(std::size_t) n] = Rin[(std::size_t) n] = 0.5f * rr.next(); }
        runVariableBlocks (c, L, R, total);
        const auto refL = directConv (Lin, ir), refR = directConv (Rin, ir);
        const int from = warm + irLen, to = total;
        check (maxAbsDiff (L, refL, from, to) < 2e-4, "noise L == direct conv, max-abs < 2e-4");
        check (nullRmsDb (L, refL, from, to) < -100.0, "noise L null RMS < -100 dBFS");
        check (nullRmsDb (R, refR, from, to) < -100.0, "noise R null RMS < -100 dBFS");
    }

    // --- Test 3: true-stereo IR (L != R) ---
    {
        std::vector<float> irL = ir, irR ((std::size_t) irLen);
        { Lcg r { 777 }; for (int n = 0; n < irLen; ++n) { const float env = std::exp (-2.0f * (float) n / irLen); irR[(std::size_t) n] = env * (0.4f * r.next() + (n == 0 ? 0.8f : 0.0f)); } }
        CoreConvolver<> c; c.prepare (sr, 512, 2, maxIrSec);
        const float* irp[2] { irL.data(), irR.data() }; c.loadIR (irp, 2, irLen, sr);
        std::vector<float> L ((std::size_t) total, 0.0f), R ((std::size_t) total, 0.0f), Lin ((std::size_t) total, 0.0f), Rin ((std::size_t) total, 0.0f);
        Lcg rl { 222 }, rr { 333 };
        for (int n = warm; n < total; ++n) { L[(std::size_t) n] = Lin[(std::size_t) n] = 0.5f * rl.next(); R[(std::size_t) n] = Rin[(std::size_t) n] = 0.5f * rr.next(); }
        runVariableBlocks (c, L, R, total);
        const auto refL = directConv (Lin, irL), refR = directConv (Rin, irR);
        const int from = warm + irLen, to = total;
        check (nullRmsDb (L, refL, from, to) < -100.0, "stereo IR: L conv irL, null < -100 dBFS");
        check (nullRmsDb (R, refR, from, to) < -100.0, "stereo IR: R conv irR, null < -100 dBFS");
    }

    std::printf ("%d checks, %d failures\n", g_checks, g_fail);
    std::printf (g_fail ? "GOLDEN FAILED\n" : "GOLDEN PASSED\n");
    return g_fail ? 1 : 0;
}
