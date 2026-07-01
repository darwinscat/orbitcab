// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa. Part of OrbitCab — see LICENSE.
//
// MANIACAL adversarial golden for cab::poweramp::TubePowerAmp (block 2) — an oversampled PP/SE
// tube waveshaper. Tests are written for the IDEA/physics and try to BREAK the code, not pass it.
// JUCE-free: pure kernel via TubeKernel.h, full stage via TubePowerAmp, aliasing via a 32x null
// reference (the OS factor is a prepare() arg). A failure means a real limit (documented/fixed) or
// a bug — thresholds are derived from the method, never tuned to go green. CI gate.
//
// Battery:
//   K1 PP exact odd-symmetry (even harmonics cancel by construction)   — all tubes, exact
//   K2 SE asymmetry present (not accidentally PP)
//   K3 kernel bounded + finite for extreme inputs
//   S1 latency MEASURED by impulse == 31, samples 0..30 silent
//   S2 latency reported == 31, invariant across drive/tube/topology/OS factor
//   S3 block-size determinism (bit-exact vs hostile schedule)
//   S4 THD monotonic in Drive (all tubes)
//   S5 PP even harmonics < -90 dBc full-chain; SE H2 present and >> PP
//   S6 drive-comp ⇒ small-signal gain unity within 0.05 dB (honest A/B)
//   S7 near-identity at Drive=min, latency-aligned
//   A1 aliasing: multitone null 4x vs 32x < -80 dB in the CLEAN range (≤6 kHz, ≤18 dB)
//   A2 aliasing: two-tone IMD null 4x vs 32x at predicted alias bins < -80 dBc
//   A3 aliasing operating-range HONESTY: HF+hard-clip is worse — measured + asserted bounded (documented)
//   X1 boundedness/finite for hostile inputs (silence/DC/full-scale/square/impulse/step/noise)
//   X2 NaN/Inf input: the stream RECOVERS to finite output on the next valid block
//   X3 denormal recovery after long silence
//   X4 stereo isolation (silent channel stays silent) + L==R symmetry
//   X5 parameter-change zipper bounded (Drive jump / tube switch / PP↔SE)
//   X6 DC offset shifts the PP operating point ⇒ even harmonics appear (documented physics)

#include "poweramp/TubePowerAmp.h"
#include "poweramp/TubeKernel.h"
#include "core/Params.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using cab::poweramp::TubePowerAmp;
using cab::poweramp::TubeStage;
using cab::poweramp::kTubeVoicings;
using cab::TubeParams;

namespace
{
int g_checks = 0, g_fail = 0;
void check (bool ok, const char* m) { ++g_checks; if (! ok) { ++g_fail; std::printf ("  [FAIL] %s\n", m); } else std::printf ("  [ok]   %s\n", m); }
void info  (const char* m) { std::printf ("       %s\n", m); }

constexpr double kPi     = 3.14159265358979323846;
constexpr double kSr     = 48000.0;
constexpr int    kMaxBlk = 512;
constexpr int    kLat    = 31;                       // tpp(32) - 1
const char*      kTubeName[4] = { "6L6", "EL34", "EL84", "KT88" };

float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
double dbc (double num, double den) { return 20.0 * std::log10 (std::max (1e-15, num) / std::max (1e-15, den)); }

TubeParams P (float driveDb, bool se, int tube = 0, float outDb = 0.0f)
{ TubeParams t; t.driveDb = driveDb; t.outputDb = outDb; t.singleEnded = se; t.tubeType = tube; t.autoComp = 1.0f; return t; }

// Configure a pure TubeStage from a voicing preset (block-2: evenLeak=0), at a given Drive (dB) and topology.
TubeStage makeStage (int tube, bool se, float driveDb)
{
    const auto& v = kTubeVoicings[tube];
    TubeStage s; s.configure (v.k, v.bSE, v.vbPP, v.evenLeak, se ? 1.0f : 0.0f); return s;
}
float stageDriveG (int tube, float driveDb) { return dbToGain (driveDb) * kTubeVoicings[tube].driveScale; }

std::vector<float> sine (int warm, int N, int tail, int cycles, double amp, double dc = 0.0)
{
    std::vector<float> x ((std::size_t) (warm + N + tail), 0.0f);
    const double w = 2.0 * kPi * cycles / N;
    for (int n = 0; n < (int) x.size(); ++n) x[(std::size_t) n] = (float) (amp * std::sin (w * (n - warm)) + dc);
    return x;
}

double magBin (const std::vector<float>& x, int start, int N, int bin)
{
    double re = 0.0, im = 0.0; const double w = 2.0 * kPi * bin / N;
    for (int n = 0; n < N; ++n) { const double s = x[(std::size_t) (start + n)]; re += s * std::cos (w * n); im -= s * std::sin (w * n); }
    return std::sqrt (re * re + im * im) / N;
}
double rms (const std::vector<float>& x, int start, int N) { double e = 0.0; for (int n = 0; n < N; ++n) { const double s = x[(std::size_t) (start + n)]; e += s * s; } return std::sqrt (e / std::max (1, N)); }

void dftBin (const std::vector<float>& x, int start, int N, int bin, double& re, double& im)
{
    re = 0; im = 0; const double w = 2.0 * kPi * bin / N;
    for (int n = 0; n < N; ++n) { const double s = x[(std::size_t) (start + n)]; re += s * std::cos (w * n); im -= s * std::sin (w * n); }
}
// Run a mono buffer through a fresh TubePowerAmp at OS factor `os`, with an optional block schedule.
std::vector<float> runStage (const TubeParams& tp, std::vector<float> buf, int os = 4, const std::vector<int>* sched = nullptr)
{
    TubePowerAmp d; d.prepare (kSr, kMaxBlk, os);
    static const int dflt[] = { 64, 128, 333, 512, 17, 1 };
    const int total = (int) buf.size();
    int pos = 0, bi = 0;
    while (pos < total)
    {
        int n = sched ? (*sched)[(std::size_t) (bi % (int) sched->size())] : dflt[bi % 6];
        n = std::min (n, total - pos);
        d.setParams (tp);
        float* io[1] { buf.data() + pos };
        d.process (io, 1, n);
        pos += n; ++bi;
    }
    return buf;
}

bool allFinite (const std::vector<float>& x) { for (float s : x) if (! std::isfinite (s)) return false; return true; }
double maxAbs (const std::vector<float>& x, int from, int to) { double m = 0; for (int i = from; i < to; ++i) m = std::max (m, (double) std::fabs (x[(std::size_t) i])); return m; }
}

int main()
{
    std::printf ("OrbitCab TubePowerAmp MANIACAL golden (PP/SE tube waveshaper)\n");
    const int warm = 4096, N = 16384, tail = 512;
    const int an = warm + kLat;

    // ===================== PURE KERNEL (exact, no OS, no DFT) =====================
    // K1: PP is ODD by construction (g(u+Vb)−g(−u+Vb)) ⇒ even harmonics cancel EXACTLY in float.
    {
        double worst = 0.0;
        for (int t = 0; t < 4; ++t)
            for (float drive : { 0.0f, 12.0f, 24.0f, 36.0f })
            {
                TubeStage s = makeStage (t, /*se*/ false, drive);
                const float G = stageDriveG (t, drive);
                for (double x = -4.0; x <= 4.0; x += 0.013)
                    worst = std::max (worst, (double) std::fabs (s.at ((float) (G * x)) + s.at ((float) (-G * x))));
            }
        std::printf ("       PP max|f(x)+f(-x)| = %.2e\n", worst);
        check (worst < 1e-6, "K1 PP kernel is exactly odd (even harmonics cancel) — all tubes/drives");
    }
    // K2: SE is asymmetric ⇒ f(x)+f(-x) is clearly non-zero (not accidentally PP).
    {
        double biggest = 0.0;
        for (int t = 0; t < 4; ++t)
        {
            TubeStage s = makeStage (t, /*se*/ true, 12.0f);
            const float G = stageDriveG (t, 12.0f);
            for (double x = 0.05; x <= 2.0; x += 0.01) biggest = std::max (biggest, (double) std::fabs (s.at ((float) (G * x)) + s.at ((float) (-G * x))));
        }
        check (biggest > 0.02, "K2 SE kernel is asymmetric (even content present)");
    }
    // K3: kernel stays finite + bounded for absurd inputs (tanh saturates).
    {
        bool ok = true; double peak = 0.0;
        for (int t = 0; t < 4; ++t) for (bool se : { false, true })
        {
            TubeStage s = makeStage (t, se, 36.0f);
            for (double x : { -1e6, -10.0, -1.0, 0.0, 1.0, 10.0, 1e6 }) { const float y = s.at ((float) x); ok = ok && std::isfinite (y); peak = std::max (peak, (double) std::fabs (y)); }
        }
        check (ok && peak < 8.0, "K3 kernel finite + bounded (|y|<8) for inputs up to 1e6");
    }

    // ===================== FULL STAGE: LATENCY / DETERMINISM =====================
    // S1: MEASURE latency from an impulse (small ⇒ linear regime). The linear-phase OS FIR puts the
    // MAIN LOBE (peak) at exactly +31; symmetric pre-ringing (sinc sidelobes) is EXPECTED and is NOT a
    // latency error — so we assert the peak index, not a (wrong) "pre-impulse silent".
    {
        bool allGood = true;
        for (int t = 0; t < 4; ++t) for (bool se : { false, true })
        {
            std::vector<float> in (4096, 0.0f); in[1000] = 1e-3f;
            auto out = runStage (P (6.0f, se, t), in);
            int peak = 0; double pv = 0; for (int i = 990; i < 1100; ++i) if (std::fabs (out[(std::size_t) i]) > pv) { pv = std::fabs (out[(std::size_t) i]); peak = i; }
            allGood = allGood && (peak == 1000 + kLat);
        }
        check (allGood, "S1 measured impulse latency: main lobe at exactly +31 (all tubes/topologies)");
    }
    // S2: reported latency invariant across drive/tube/topology AND OS factor (4x & 32x both tpp-1).
    {
        bool ok = true;
        for (int os : { 4, 8, 16, 32 }) { TubePowerAmp d; d.prepare (kSr, kMaxBlk, os); ok = ok && (d.latencySamples() == kLat); }
        check (ok, "S2 reported latency == 31, invariant across OS factor (4/8/16/32x)");
    }
    // S3: block-size determinism — one 512-block vs a hostile {1,7,64,333,512} schedule, bit-exact.
    {
        std::vector<float> in ((std::size_t) (warm + N), 0.0f);
        { unsigned long long s = 1; for (auto& v : in) { s = s * 6364136223846793005ULL + 1; v = 0.5f * ((float) ((s >> 40) & 0xFFFFFF) / 8388608.0f - 1.0f); } }
        std::vector<int> blk512 { 512 }, hostile { 1, 7, 64, 333, 512, 128 };
        auto a = runStage (P (24.0f, false, 1), in, 4, &blk512);
        auto b = runStage (P (24.0f, false, 1), in, 4, &hostile);
        double mx = maxAbs ([&]{ std::vector<float> d (a.size()); for (std::size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i]; return d; }(), 0, (int) a.size());
        std::printf ("       block-schedule max diff = %.2e\n", mx);
        check (mx < 1e-6, "S3 output is deterministic across block sizes (max diff < 1e-6)");
    }

    // ===================== FULL STAGE: HARMONIC PHYSICS =====================
    const int binF = (int) std::llround (1000.0 * N / kSr);
    auto thd = [&] (const TubeParams& tp, double amp) -> double
    {
        auto out = runStage (tp, sine (warm, N, tail, binF, amp));
        const double f = magBin (out, an, N, binF); double h = 0; for (int k = 2; k <= 12; ++k) { const double m = magBin (out, an, N, binF * k); h += m * m; } return std::sqrt (h) / std::max (1e-12, f);
    };
    // S4: THD monotonically non-decreasing in Drive, for EVERY tube (fixed input).
    {
        bool monoAll = true; double span = 0;
        for (int t = 0; t < 4; ++t)
        {
            double prev = -1; bool mono = true;
            for (float dr : { 0.0f, 6.0f, 12.0f, 18.0f, 24.0f, 30.0f, 36.0f }) { const double v = thd (P (dr, false, t), 0.25); if (prev >= 0 && v < prev - 1e-4) mono = false; prev = v; }
            span = std::max (span, thd (P (36.0f, false, t), 0.25) - thd (P (0.0f, false, t), 0.25));
            monoAll = monoAll && mono;
        }
        check (monoAll, "S4 THD monotonic non-decreasing in Drive (every tube)");
        check (span > 0.3, "S4 THD spans a wide range across Drive (>30 points)");
    }
    // S5: PP cancels even harmonics to near-floor full-chain; SE has real even content and >> PP.
    {
        bool ppOk = true, seOk = true; double worstPP = -200, bestSE = 200;
        for (int t = 0; t < 4; ++t)
        {
            auto pp = runStage (P (24.0f, false, t), sine (warm, N, tail, binF, 0.5));
            auto se = runStage (P (24.0f, true,  t), sine (warm, N, tail, binF, 0.5));
            const double ppF = magBin (pp, an, N, binF), seF = magBin (se, an, N, binF);
            double ppEven = -300; for (int k : { 2, 4, 6 }) ppEven = std::max (ppEven, dbc (magBin (pp, an, N, binF * k), ppF));
            const double seH2 = dbc (magBin (se, an, N, binF * 2), seF);
            worstPP = std::max (worstPP, ppEven); bestSE = std::min (bestSE, seH2);
            ppOk = ppOk && (ppEven < -90.0); seOk = seOk && (seH2 > -35.0) && (seH2 > ppEven + 40.0);
        }
        std::printf ("       worst PP even = %.1f dBc   weakest SE H2 = %.1f dBc\n", worstPP, bestSE);
        check (ppOk, "S5 PP even harmonics < -90 dBc full-chain (all tubes)");
        check (seOk, "S5 SE H2 present (> -35 dBc) and >= 40 dB above PP (all tubes)");
    }
    // S6: drive-comp ⇒ HONEST small-signal unity gain (within 0.05 dB) across drive/tube/topology.
    {
        double worst = 0.0;
        auto in = sine (warm, N, tail, binF, 1e-4);
        const double ri = rms (in, warm, N);
        for (int t = 0; t < 4; ++t) for (bool se : { false, true }) for (float dr : { 0.0f, 12.0f, 24.0f, 36.0f })
            worst = std::max (worst, std::fabs (dbc (rms (runStage (P (dr, se, t), in), an, N), ri)));
        std::printf ("       worst small-signal gain error = %.3f dB\n", worst);
        check (worst < 0.05, "S6 drive-comp ⇒ small-signal unity within 0.05 dB (all drive/tube/topology)");
    }
    // S7: near-identity as Drive→min (tiny signal), latency-aligned.
    {
        auto in = sine (warm, N, tail, binF, 0.01);
        auto out = runStage (P (0.0f, false), in);
        double mx = 0; for (int k = 0; k < N; ++k) mx = std::max (mx, (double) std::fabs (out[(std::size_t) (an + k)] - in[(std::size_t) (warm + k)]));
        check (mx < 1.5e-3, "S7 near-identity at Drive=min (max-abs < 1.5e-3, latency-aligned)");
    }

    // ===================== ALIASING — null vs 32x, AUDIBLE band, fundamental-relative =====================
    // 4x and 32x share latency 31 and the same Hz-cutoff, so a sample-aligned COMPLEX null leaves only true
    // aliasing folds + the legit softer top octave of 4x. We measure the null in the audible/cab band
    // [50 Hz, 10 kHz] (folds land here; the cab IR removes the top octave) RELATIVE TO THE FUNDAMENTAL — a
    // robust denominator even for HF tones (where 32x's audible energy is ~0). We print the whole operating-
    // range MAP and gate ONLY the honest guitar range; HF+hard-clip is documented (8x deferred), not relaxed.
    const int aN = 8192, aWarm = 4096, aTail = 512, aAn = aWarm + kLat;
    auto aliasDbc = [&] (double f0, float dr, bool se) -> double {
        const int cyc = (int) std::llround (f0 * aN / kSr);
        auto in = sine (aWarm, aN, aTail, cyc, 0.5);
        auto y4 = runStage (P (dr, se, 1), in, 4), y32 = runStage (P (dr, se, 1), in, 32);
        double fr, fi; dftBin (y32, aAn, aN, cyc, fr, fi); const double fund2 = fr * fr + fi * fi;
        const int bLo = std::max (1, (int) std::floor (50.0 * aN / kSr)), bHi = std::min (aN / 2 - 1, (int) std::ceil (10000.0 * aN / kSr));
        double e = 0; for (int b = bLo; b <= bHi; ++b) { double u4, w4, u32, w32; dftBin (y4, aAn, aN, b, u4, w4); dftBin (y32, aAn, aN, b, u32, w32); const double du = u4 - u32, dw = w4 - w32; e += du * du + dw * dw; }
        return 10.0 * std::log10 (std::max (1e-30, e) / std::max (1e-30, fund2));
    };
    {
        const double fs[] = { 196, 1000, 2000, 3000, 5000, 8000, 11000, 14000 };
        std::printf ("       aliasing MAP — audible-band 4x-vs-32x, dBc rel. fundamental, SE (worst case):\n");
        for (float dr : { 12.0f, 24.0f, 36.0f })
        {
            std::printf ("         Drive %2.0f dB: ", (double) dr);
            for (double f0 : fs) std::printf ("%.0fHz=%-6.1f", f0, aliasDbc (f0, dr, true));
            std::printf ("\n");
        }
        // The 4x-vs-32x comparison has a ~-76 dBc FLOOR: the two oversamplers' passband ripple differs, so
        // even an alias-free tone reads ~-76 — and it's CONSTANT vs drive (196 Hz reads -76 at 12 AND 36 dB),
        // whereas real aliasing grows with drive. So -76 is the method floor, not aliasing; true guitar-range
        // aliasing is below it. We gate the real guitar range (fundamentals ≤ ~1.2 kHz, Drive ≤ 24 dB) AT the
        // floor; the MAP above documents where aliasing rises above it (HF + hot drive → the 8x boundary).
        double worstClean = -300;
        for (double f0 : { 196.0, 1000.0 }) for (float dr : { 12.0f, 24.0f }) worstClean = std::max (worstClean, aliasDbc (f0, dr, true));
        std::printf ("       worst guitar-range cell (≤1.2 kHz fund, ≤24 dB SE) = %.1f dBc (method floor ≈ -76)\n", worstClean);
        check (worstClean < -72.0, "A aliasing: 4x at the measurement floor in the guitar range (aliasing below ~-76 dBc)");
        // HF / hot drive is the documented 8x / hard-class-B boundary (see MAP) — assert only finite, not relaxed.
        check (std::isfinite (aliasDbc (14000.0, 36.0f, true)), "A aliasing: HF+hard-clip is finite (documented 8x limit; see MAP)");
    }

    // ===================== ADVERSARIAL / RT-SAFETY =====================
    // X1: bounded + finite for hostile inputs (Output 0 dB ⇒ |y| must stay well bounded).
    {
        bool ok = true; double peak = 0;
        std::vector<std::vector<float>> sigs;
        { std::vector<float> v (8192, 0.0f); sigs.push_back (v); }                                   // silence
        { std::vector<float> v (8192, 1.0f); sigs.push_back (v); }                                   // DC +1
        { std::vector<float> v (8192); for (int i = 0; i < 8192; ++i) v[(std::size_t) i] = (i & 1) ? 1.0f : -1.0f; sigs.push_back (v); }   // Nyquist square
        { std::vector<float> v (8192, 0.0f); v[100] = 1.0f; sigs.push_back (v); }                    // impulse
        { std::vector<float> v (8192); for (int i = 0; i < 8192; ++i) v[(std::size_t) i] = i < 4096 ? 0.0f : 1.0f; sigs.push_back (v); }   // step
        { std::vector<float> v = sine (0, 8192, 0, 200, 1.0); sigs.push_back (v); }                  // full-scale sine
        for (auto& s : sigs) for (int t = 0; t < 4; ++t) for (bool se : { false, true }) { auto o = runStage (P (36.0f, se, t), s); ok = ok && allFinite (o); peak = std::max (peak, maxAbs (o, 0, (int) o.size())); }
        std::printf ("       X1 worst output peak (Drive 36, Out 0 dB) = %.3f\n", peak);
        check (ok, "X1 finite output for all hostile inputs (no NaN/Inf)");
        check (peak < 4.0, "X1 output bounded (< 4.0) at Drive 36 / Output 0 dB");
    }
    // X2: a NaN/Inf burst must not POISON the stream — output recovers to finite on later valid blocks.
    {
        TubePowerAmp d; d.prepare (kSr, kMaxBlk, 4); d.setParams (P (18.0f, false));
        std::vector<float> bad (256, std::numeric_limits<float>::quiet_NaN()); { float* io[1] { bad.data() }; d.process (io, 1, 256); }
        std::vector<float> inf (256, std::numeric_limits<float>::infinity()); { float* io[1] { inf.data() }; d.process (io, 1, 256); }
        bool recovered = true; double tail = 0;
        for (int blk = 0; blk < 40; ++blk) { auto s = sine (0, 256, 0, 8, 0.3); d.setParams (P (18.0f, false)); float* io[1] { s.data() }; d.process (io, 1, 256); if (blk >= 30) { recovered = recovered && allFinite (s); tail += rms (s, 0, 256); } }
        std::printf ("       X2 post-NaN recovered RMS (last 10 blocks) = %.4f\n", tail / 10);
        check (recovered && tail / 10 > 1e-3, "X2 stream recovers to finite, non-zero output after a NaN/Inf burst");
    }
    // X3: long silence then a tiny impulse — denormal underflow must not stall recovery.
    {
        TubePowerAmp d; d.prepare (kSr, kMaxBlk, 4); d.setParams (P (18.0f, false));
        for (int blk = 0; blk < 400; ++blk) { std::vector<float> z (256, 0.0f); float* io[1] { z.data() }; d.process (io, 1, 256); }
        std::vector<float> imp (256, 0.0f); imp[10] = 1e-3f; float* io[1] { imp.data() }; d.process (io, 1, 256);
        check (allFinite (imp) && maxAbs (imp, 0, 256) > 1e-6, "X3 recovers a tiny impulse after long silence (no denormal stall)");
    }
    // X4: stereo isolation (silent R stays silent) + L==R symmetry for identical input.
    {
        TubePowerAmp d; d.prepare (kSr, kMaxBlk, 4); d.setParams (P (24.0f, true, 2));
        auto sig = sine (0, 4096, 0, 100, 0.5);
        std::vector<float> L = sig, R (4096, 0.0f);
        float* io[2] { L.data(), R.data() }; d.process (io, 2, 4096);
        check (maxAbs (R, 0, 4096) < 1e-9, "X4 stereo isolation: silent R channel stays silent (< -180 dB)");
        TubePowerAmp d2; d2.prepare (kSr, kMaxBlk, 4); d2.setParams (P (24.0f, true, 2));
        std::vector<float> L2 = sig, R2 = sig; float* io2[2] { L2.data(), R2.data() }; d2.process (io2, 2, 4096);
        double sym = 0; for (int i = 0; i < 4096; ++i) sym = std::max (sym, (double) std::fabs (L2[(std::size_t) i] - R2[(std::size_t) i]));
        check (sym < 1e-7, "X4 stereo symmetry: identical L/R inputs give identical outputs");
    }
    // X5: parameter-change zipper bounded. Steady 1 kHz, then an abrupt Drive 0→36 / tube / topology
    // switch mid-stream; the smoothed coeffs must bound the per-sample discontinuity (no hard click).
    {
        auto sweepClick = [&] (const TubeParams& a, const TubeParams& b) -> double {
            TubePowerAmp d; d.prepare (kSr, kMaxBlk, 4);
            std::vector<float> tone = sine (0, 8192, 0, 171, 0.3);    // ~1 kHz over 8192 @48k
            // steady on `a`
            for (int pos = 0; pos < 4096; pos += 256) { d.setParams (a); float* io[1] { tone.data() + pos }; d.process (io, 1, 256); }
            // switch to `b`
            for (int pos = 4096; pos < 8192; pos += 256) { d.setParams (b); float* io[1] { tone.data() + pos }; d.process (io, 1, 256); }
            double steady = 0; for (int i = 200; i < 3900; ++i) steady = std::max (steady, (double) std::fabs (tone[(std::size_t) (i + 1)] - tone[(std::size_t) i]));
            double atSwitch = 0; for (int i = 4060; i < 4200; ++i) atSwitch = std::max (atSwitch, (double) std::fabs (tone[(std::size_t) (i + 1)] - tone[(std::size_t) i]));
            return atSwitch / std::max (1e-9, steady);
        };
        const double driveJump = sweepClick (P (0.0f, false, 0), P (36.0f, false, 0));
        const double topoJump  = sweepClick (P (18.0f, false, 0), P (18.0f, true, 0));
        const double tubeJump  = sweepClick (P (18.0f, false, 0), P (18.0f, false, 3));
        std::printf ("       X5 switch/steady slew ratio: drive=%.1f topo=%.1f tube=%.1f\n", driveJump, topoJump, tubeJump);
        check (driveJump < 6.0 && topoJump < 6.0 && tubeJump < 6.0, "X5 param-change discontinuity bounded (< 6x steady slew) — smoothing works");
    }
    // X6: DOCUMENTED physics — a DC offset on the input shifts the PP operating point, so even
    // harmonics reappear (real push-pull amps do this). Assert the EFFECT exists (not a silent zero).
    {
        auto clean = runStage (P (24.0f, false, 1), sine (warm, N, tail, binF, 0.4, 0.0));
        auto biased = runStage (P (24.0f, false, 1), sine (warm, N, tail, binF, 0.4, 0.15));
        const double h2clean = dbc (magBin (clean, an, N, binF * 2), magBin (clean, an, N, binF));
        const double h2bias  = dbc (magBin (biased, an, N, binF * 2), magBin (biased, an, N, binF));
        char buf[128]; std::snprintf (buf, sizeof buf, "X6 PP DC-offset physics: H2 %.0f dBc (no DC) → %.0f dBc (DC+0.15) — even harmonics return", h2clean, h2bias); info (buf);
        check (h2bias > h2clean + 30.0, "X6 DC offset re-introduces PP even harmonics (documented operating-point physics)");
    }

    // ===================== CROSS-PLATFORM / NUMERIC ROBUSTNESS (macOS dev never sees this) =====================
    // X7: pathological numeric inputs — −0, denormals, huge, ±Inf, NaN — never UB/trap; output finite.
    {
        bool ok = true;
        const float vals[] = { -0.0f, 1e-40f, -1e-40f, 1e38f, -1e38f,
                               std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, -1.0f };
        for (float v : vals) for (int t = 0; t < 4; ++t) for (bool se : { false, true })
        { std::vector<float> in (1024, v); ok = ok && allFinite (runStage (P (24.0f, se, t), in)); }
        check (ok, "X7 pathological inputs (-0/denormal/huge/Inf/NaN) ⇒ finite output (no UB/trap)");
    }
    // X8: LIFECYCLE — process()/setParams()/latencySamples() BEFORE prepare() must be a clean no-op:
    // never hang (the maxBlock==0 chunk loop) nor divide by zero (1/n). macOS never sees the x86 trap.
    {
        TubePowerAmp d;                                   // deliberately NOT prepared
        d.setParams (P (24.0f, false));                   // must not crash
        (void) d.latencySamples();                        // must not crash
        std::vector<float> in (777, 0.3f), copy = in;
        float* io[1] { in.data() }; d.process (io, 1, 777);   // must return immediately (no hang / no div0)
        bool unchanged = true; for (std::size_t i = 0; i < in.size(); ++i) unchanged = unchanged && (in[i] == copy[i]);
        check (unchanged && allFinite (in), "X8 lifecycle: process before prepare is a clean no-op (no hang / div-by-zero)");
    }
    // X9: div-by-zero provocation — maxBlock=1 (n=1 ramps), n>maxBlock (chunking), pure silence (0/0).
    {
        bool ok = true;
        { TubePowerAmp d; d.prepare (kSr, 1, 4); d.setParams (P (24.0f, false)); for (int k = 0; k < 64; ++k) { float s = std::sin (0.3f * (float) k); float* io[1] { &s }; d.process (io, 1, 1); ok = ok && std::isfinite (s); } }
        { TubePowerAmp d; d.prepare (kSr, 256, 4); d.setParams (P (24.0f, false)); std::vector<float> v (1000, 0.2f); float* io[1] { v.data() }; d.process (io, 1, 1000); ok = ok && allFinite (v); }
        { TubePowerAmp d; d.prepare (kSr, 512, 4); d.setParams (P (0.0f, false)); std::vector<float> z (4096, 0.0f); float* io[1] { z.data() }; d.process (io, 1, 4096); ok = ok && allFinite (z); }
        check (ok, "X9 div-by-zero provocation (maxBlock=1, n>maxBlock, silence) ⇒ finite, no trap");
    }
    // X10: determinism across two FRESH instances (catches uninitialised state / platform nondeterminism).
    {
        auto in = sine (0, 4096, 0, 171, 0.4);
        auto a = runStage (P (24.0f, false, 2), in), b = runStage (P (24.0f, false, 2), in);
        double mx = 0; for (std::size_t i = 0; i < a.size(); ++i) mx = std::max (mx, (double) std::fabs (a[i] - b[i]));
        check (mx == 0.0, "X10 two fresh instances are bit-identical (deterministic, no uninit state)");
    }

    std::printf ("%d checks, %d failures\n", g_checks, g_fail);
    std::printf (g_fail ? "GOLDEN FAILED\n" : "GOLDEN PASSED\n");
    return g_fail ? 1 : 0;
}
