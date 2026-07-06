// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.
//
// Headless golden test: cab::Convolver (felitronics::convolution) vs a DIRECT-convolution
// reference, at host rate. Validates the JUCE-free convolution as a sound-preserving cab backend
// AND the load-time reference-unity IR normalization contract:
//   • impulse -> the normalized IR itself; mono IR broadcasts to L+R (JUCE Stereo::yes parity);
//   • noise == direct convolution with the identically-scaled IR under a hostile variable-block
//     sweep (null RMS < -100 dBFS) — engine math is exact, the normalization is ONE known gain;
//   • the applied gain matches an INDEPENDENT implementation of the reference measure
//     (double-precision linear-grid DFT integral) within 0.15 dB — pins the formula;
//   • a unit delta normalizes to ~0 dB (flat |H| ⇒ already reference-unity) and passes through;
//   • the same IR at x0.125 file scale lands on the SAME output (loudness is the loader's job);
//   • a silent IR is left alone (gain 1 — the -60 dB floor guard, no garbage amplification);
//   • a true-stereo IR keeps its L/R relative level (ONE common gain, imaging untouched).
// JUCE-free — direct convolution is the ground truth; the live JUCE-vs-core ear A/B is the plugin.

#include "core/Convolver.h"

#include <juce_core/juce_core.h>   // juce::Thread::sleep — pump juce::dsp::Convolution's async loader

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using cab::Convolver;

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
std::vector<float> scaled (const std::vector<float>& ir, float g)
{
    std::vector<float> out = ir; for (float& v : out) v *= g; return out;
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
double rms (const std::vector<float>& a, int from, int to)
{
    double e = 0.0; int n = 0; for (int i = from; i < to; ++i) { e += (double) a[(std::size_t) i] * a[(std::size_t) i]; ++n; }
    return n ? std::sqrt (e / n) : 0.0;
}

// INDEPENDENT reference-gain implementation (the formula's ground truth): double-precision direct
// DFT of the IR on a fine LINEAR frequency grid, weighted by the one-pole reference power spectrum
// w(f) = 1/(1+(f/kIrRefShapeHz)^2), DC excluded. Deliberately not the shipping code path (that one
// uses the felitronics real FFT on a pow2 grid): agreement within tolerance pins the formula, not
// the implementation.
double refNormGainDb (const std::vector<float>& ir, double sr)
{
    const int bins = 4096;
    double num = 0.0, den = 0.0;
    for (int k = 1; k <= bins; ++k)
    {
        const double f = 0.5 * sr * (double) k / (double) bins;
        std::complex<double> acc (0.0, 0.0);
        const std::complex<double> step = std::exp (std::complex<double> (0.0, -2.0 * 3.14159265358979323846 * f / sr));
        std::complex<double> w (1.0, 0.0);
        for (float v : ir) { acc += (double) v * w; w *= step; }
        const double wt = 1.0 / (1.0 + (f / Convolver::kIrRefShapeHz) * (f / Convolver::kIrRefShapeHz));
        num += wt * std::norm (acc);
        den += wt;
    }
    return -10.0 * std::log10 (std::max (1e-12, num / den));   // the gain the loader should APPLY
}

// process `total` samples through the convolver in a hostile variable block sweep (all <= maxBlock).
void runVariableBlocks (Convolver& c, std::vector<float>& L, std::vector<float>& R, int total)
{
    // juce::dsp::Convolution loads the IR ASYNCHRONOUSLY on its own background thread and swaps it in
    // during process() with a 50 ms crossfade. In a bare console app that races (the buffer is
    // processed in microseconds, faster than the ~10 ms loader poll). So first drive the loader to
    // completion — pump silence + sleep until the requested IR is the live engine (isBusy() false) —
    // then flush the first-load crossfade on silence, leaving the fully-installed IR with zeroed
    // internal state before the measured signal is fed. (The plugin + OrbitCab_Tests pump the same way.)
    {
        std::vector<float> z0 (512, 0.0f), z1 (512, 0.0f);
        auto pump = [&] { float* io[2] { z0.data(), z1.data() }; c.process (io, 2, 512); };
        for (int i = 0; i < 800 && c.isBusy(); ++i) { pump(); juce::Thread::sleep (3); }   // await install (~2.4 s cap)
        for (int s = 0; s < 6000; s += 512) pump();                                          // flush >50 ms crossfade @48k
    }
    static const int blk[] = { 1, 17, 64, 128, 333, 512 };
    int pos = 0, bi = 0;
    while (pos < total) { const int n = std::min (blk[bi % 6], total - pos); float* io[2] { L.data() + pos, R.data() + pos }; c.process (io, 2, n); pos += n; ++bi; }
}
}

int main()
{
    std::printf ("OrbitCab CoreConvolver golden (vs direct conv + reference-unity normalization)\n");
    const double sr = 48000.0;
    const int irLen = 512;
    const double maxIrSec = 0.05;     // small -> short cold-start fade
    const int warm = 4096;            // > coldXfade -> measure past the cold fade
    const int testLen = 8192;
    const int total = warm + testLen;

    std::vector<float> ir ((std::size_t) irLen);
    { Lcg r { 12345 }; for (int n = 0; n < irLen; ++n) { const float env = std::exp (-3.0f * (float) n / irLen); ir[(std::size_t) n] = env * (0.5f * r.next() + (n == 0 ? 1.0f : 0.0f)); } }

    // --- Test 1: impulse -> normalized IR (mono) + mono broadcast to R ---
    float gMono = 0.0f;
    {
        Convolver c; c.prepare (sr, 512, 2, maxIrSec);
        const float* irp[1] { ir.data() }; c.loadIR (irp, 1, irLen, sr);
        gMono = c.irNormalizationGain();
        std::vector<float> L ((std::size_t) total, 0.0f), R ((std::size_t) total, 0.0f);
        L[(std::size_t) warm] = 1.0f; R[(std::size_t) warm] = 1.0f;
        runVariableBlocks (c, L, R, total);
        double mL = 0.0, mR = 0.0;
        for (int k = 0; k < irLen; ++k) { const double want = (double) ir[(std::size_t) k] * gMono; mL = std::max (mL, std::fabs ((double) L[(std::size_t) (warm + k)] - want)); mR = std::max (mR, std::fabs ((double) R[(std::size_t) (warm + k)] - want)); }
        check (mL < 2e-4, "impulse -> normalized IR (mono) max-abs < 2e-4");
        check (mR < 2e-4, "mono IR broadcasts to R (JUCE Stereo::yes parity)");
        check (std::isfinite (gMono) && gMono > 0.0f, "normalization gain is finite and positive");
    }

    // --- Test 2: the applied gain matches the independent formula reference ---
    {
        const double refDb = refNormGainDb (ir, sr);
        const double gotDb = 20.0 * std::log10 (std::max (1e-12, (double) gMono));
        std::printf ("         norm gain: applied %+7.3f dB, independent reference %+7.3f dB\n", gotDb, refDb);
        check (std::fabs (gotDb - refDb) < 0.15, "normalization gain == independent DFT reference (< 0.15 dB)");
    }

    // --- Test 3: noise == direct conv with the identically-scaled IR (mono IR, indep. L/R input) ---
    {
        Convolver c; c.prepare (sr, 512, 2, maxIrSec);
        const float* irp[1] { ir.data() }; c.loadIR (irp, 1, irLen, sr);
        const auto irg = scaled (ir, c.irNormalizationGain());
        std::vector<float> L ((std::size_t) total, 0.0f), R ((std::size_t) total, 0.0f), Lin ((std::size_t) total, 0.0f), Rin ((std::size_t) total, 0.0f);
        Lcg rl { 999 }, rr { 55555 };
        for (int n = warm; n < total; ++n) { L[(std::size_t) n] = Lin[(std::size_t) n] = 0.5f * rl.next(); R[(std::size_t) n] = Rin[(std::size_t) n] = 0.5f * rr.next(); }
        runVariableBlocks (c, L, R, total);
        const auto refL = directConv (Lin, irg), refR = directConv (Rin, irg);
        const int from = warm + irLen, to = total;
        check (maxAbsDiff (L, refL, from, to) < 2e-4, "noise L == direct conv (normalized IR), max-abs < 2e-4");
        check (nullRmsDb (L, refL, from, to) < -100.0, "noise L null RMS < -100 dBFS");
        check (nullRmsDb (R, refR, from, to) < -100.0, "noise R null RMS < -100 dBFS");
    }

    // --- Test 4: file-scale invariance — ir and ir*0.125 land on the SAME output ---
    {
        Convolver c1, c2; c1.prepare (sr, 512, 2, maxIrSec); c2.prepare (sr, 512, 2, maxIrSec);
        const auto irQuiet = scaled (ir, 0.125f);
        const float* p1[1] { ir.data() };      c1.loadIR (p1, 1, irLen, sr);
        const float* p2[1] { irQuiet.data() }; c2.loadIR (p2, 1, irLen, sr);
        std::vector<float> L1 ((std::size_t) total, 0.0f), R1 ((std::size_t) total, 0.0f), L2, R2;
        Lcg r { 4242 };
        for (int n = warm; n < total; ++n) L1[(std::size_t) n] = 0.5f * r.next();
        R1 = L1; L2 = L1; R2 = L1;
        runVariableBlocks (c1, L1, R1, total);
        runVariableBlocks (c2, L2, R2, total);
        check (nullRmsDb (L1, L2, warm + irLen, total) < -80.0, "ir vs ir*0.125: outputs null < -80 dBFS (loudness is the loader's)");
    }

    // --- Test 5: a unit delta is already reference-unity — gain ~0 dB, clean passthrough ---
    {
        std::vector<float> delta ((std::size_t) irLen, 0.0f); delta[0] = 1.0f;
        Convolver c; c.prepare (sr, 512, 2, maxIrSec);
        const float* p[1] { delta.data() }; c.loadIR (p, 1, irLen, sr);
        check (std::fabs (c.irNormalizationGainDb()) < 0.01f, "unit delta normalizes to ~0 dB (flat |H|)");
        std::vector<float> L ((std::size_t) total, 0.0f), R ((std::size_t) total, 0.0f), Lin ((std::size_t) total, 0.0f);
        Lcg r { 31337 };
        for (int n = warm; n < total; ++n) { L[(std::size_t) n] = Lin[(std::size_t) n] = 0.5f * r.next(); }
        R = L;
        runVariableBlocks (c, L, R, total);
        check (maxAbsDiff (L, Lin, warm + irLen, total) < 2e-4, "delta IR == passthrough, max-abs < 2e-4");
    }

    // --- Test 6: a silent IR is left alone (floor guard) — no garbage amplification ---
    {
        std::vector<float> silent ((std::size_t) irLen, 0.0f);
        Convolver c; c.prepare (sr, 512, 2, maxIrSec);
        const float* p[1] { silent.data() }; c.loadIR (p, 1, irLen, sr);
        check (std::fabs (c.irNormalizationGainDb()) < 1e-6f, "silent IR: gain held at 1 (the -60 dB floor guard)");
        std::vector<float> L ((std::size_t) total, 0.0f), R ((std::size_t) total, 0.0f);
        Lcg r { 777777 };
        for (int n = warm; n < total; ++n) L[(std::size_t) n] = R[(std::size_t) n] = 0.5f * r.next();
        runVariableBlocks (c, L, R, total);
        check (rms (L, warm + irLen, total) < 1e-9, "silent IR convolves to silence (finite, no blast)");
    }

    // --- Test 7: true-stereo IR — engine parity per channel + L/R relative level preserved ---
    {
        std::vector<float> irL = ir, irR ((std::size_t) irLen);
        for (int n = 0; n < irLen; ++n) irR[(std::size_t) n] = 0.5f * irL[(std::size_t) n];   // R = L at -6 dB
        Convolver c; c.prepare (sr, 512, 2, maxIrSec);
        const float* irp[2] { irL.data(), irR.data() }; c.loadIR (irp, 2, irLen, sr);
        const float g = c.irNormalizationGain();
        std::vector<float> L ((std::size_t) total, 0.0f), R ((std::size_t) total, 0.0f), In ((std::size_t) total, 0.0f);
        Lcg r { 2468 };
        for (int n = warm; n < total; ++n) { L[(std::size_t) n] = R[(std::size_t) n] = In[(std::size_t) n] = 0.5f * r.next(); }
        runVariableBlocks (c, L, R, total);
        const auto refL = directConv (In, scaled (irL, g)), refR = directConv (In, scaled (irR, g));
        const int from = warm + irLen, to = total;
        check (nullRmsDb (L, refL, from, to) < -100.0, "stereo IR: L conv irL (one common gain), null < -100 dBFS");
        check (nullRmsDb (R, refR, from, to) < -100.0, "stereo IR: R conv irR (one common gain), null < -100 dBFS");
        const double ratio = rms (R, from, to) / std::max (1e-12, rms (L, from, to));
        check (std::fabs (ratio - 0.5) < 0.01, "stereo imaging preserved: R/L RMS ratio stays 0.5 (-6 dB)");
    }

    std::printf ("%d checks, %d failures\n", g_checks, g_fail);
    std::printf (g_fail ? "GOLDEN FAILED\n" : "GOLDEN PASSED\n");
    return g_fail ? 1 : 0;
}
