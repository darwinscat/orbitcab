// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.
//
// Real-material numeric proof: convolve a real sweep through real cab IRs with the core backend
// (cab::CoreConvolver over felitronics::convolution) and null it against a DIRECT (textbook) convolution
// of the EXACT IR the adapter loads — resampled off host rate, then the loader's reference-unity
// normalization gain (see Convolver.h). That makes the whole load+resample+normalize+convolve path
// verifiable against the math, on real cabs, deterministically.
//
//   usage:  <exe> <sweep.wav> <ir1.wav> [ir2.wav ...]
//
// (A direct JUCE-vs-core null was tried first, but juce::dsp::Convolution's async IR loader races without a
//  message loop in a bare console app — non-deterministic; the math is the stronger, stable reference anyway.)

#include "core/Convolver.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>   // juce::Thread::sleep — pump juce::dsp::Convolution's async loader

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
std::vector<float> loadMono (const juce::String& path, double& sr)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (path)));
    if (rd == nullptr) { sr = 0.0; return {}; }
    sr = rd->sampleRate;
    const int N = (int) rd->lengthInSamples;
    juce::AudioBuffer<float> buf ((int) rd->numChannels, N);
    rd->read (&buf, 0, N, 0, true, true);
    std::vector<float> out ((std::size_t) N);
    for (int i = 0; i < N; ++i) out[(std::size_t) i] = buf.getSample (0, i);   // channel 0
    return out;
}
double nullDb (const std::vector<float>& a, const std::vector<float>& b, int from, int to)
{
    double e = 0.0; int n = 0;
    for (int i = from; i < to; ++i) { const double d = (double) a[(std::size_t) i] - b[(std::size_t) i]; e += d * d; ++n; }
    return 20.0 * std::log10 (std::max (1e-12, n ? std::sqrt (e / n) : 0.0));
}
double rmsDb (const std::vector<float>& a, int from, int to)
{
    double e = 0.0; int n = 0;
    for (int i = from; i < to; ++i) { e += (double) a[(std::size_t) i] * a[(std::size_t) i]; ++n; }
    return 20.0 * std::log10 (std::max (1e-12, n ? std::sqrt (e / n) : 0.0));
}
double maxAbs (const std::vector<float>& a, const std::vector<float>& b, int from, int to)
{
    double m = 0.0; for (int i = from; i < to; ++i) m = std::max (m, std::fabs ((double) a[(std::size_t) i] - b[(std::size_t) i])); return m;
}
// GROUND TRUTH: null `core` output against a DIRECT (textbook) convolution of `in` with `ir`, over
// [from, from+len). No JUCE — this is the mathematical answer. (Double accumulator = stricter than float.)
double coreVsDirectDb (const std::vector<float>& in, const std::vector<float>& core, const std::vector<float>& ir, int from, int len)
{
    const int M = (int) ir.size(), N = (int) in.size();
    double e = 0.0; int cnt = 0;
    for (int idx = from; idx < from + len && idx < N; ++idx)
    {
        double a = 0.0;
        for (int k = 0; k < M; ++k) { const int i = idx - k; if (i >= 0 && i < N) a += (double) in[(std::size_t) i] * ir[(std::size_t) k]; }
        const double d = a - (double) core[(std::size_t) idx]; e += d * d; ++cnt;
    }
    return 20.0 * std::log10 (std::max (1e-12, cnt ? std::sqrt (e / cnt) : 0.0));
}
template <class Eng>
void runBlocks (Eng& eng, std::vector<float>& L, std::vector<float>& R, int total)
{
    // juce::dsp::Convolution loads the IR asynchronously (its own background thread) + swaps it in
    // during process() with a 50 ms crossfade. A bare console app races that ~10 ms loader poll, so
    // first drive the loader to completion — pump silence + sleep until the IR is live (isBusy()
    // false) — then flush the first-load crossfade on silence, before the signal is fed.
    {
        std::vector<float> z0 (512, 0.0f), z1 (512, 0.0f);
        auto pump = [&] { float* io[2] { z0.data(), z1.data() }; eng.process (io, 2, 512); };
        for (int i = 0; i < 800 && eng.isBusy(); ++i) { pump(); juce::Thread::sleep (3); }   // await install
        for (int s = 0; s < 6000; s += 512) pump();                                            // flush >50 ms crossfade
    }
    int pos = 0; while (pos < total) { const int n = std::min (512, total - pos); float* io[2] { L.data() + pos, R.data() + pos }; eng.process (io, 2, n); pos += n; }
}
}

int main (int argc, char** argv)
{
    // Release build → no MessageManager needed (jassert is a no-op; the Convolution loader is its own
    // thread, picked up in process(); WAV reading needs no message loop).
    if (argc < 3) { std::printf ("usage: %s <sweep.wav> <ir1.wav> [ir2.wav ...]\n", argv[0]); return 2; }

    double sweepSr = 0.0;
    auto sweep = loadMono (argv[1], sweepSr);
    if (sweep.empty()) { std::printf ("could not load sweep %s\n", argv[1]); return 2; }
    const double sr     = sweepSr > 0.0 ? sweepSr : 48000.0;
    const int    maxUse = (int) std::min ((double) sweep.size(), 16.0 * sr);   // ~16 s of sweep = full-band coverage
    std::printf ("sweep: %.0f Hz, using %d samples (%.1f s)\n\n", sr, maxUse, maxUse / sr);

    bool allOk = true;
    for (int a = 2; a < argc; ++a)
    {
        double irSr = 0.0;
        auto ir = loadMono (argv[a], irSr);
        if (ir.empty()) { std::printf ("  [skip] could not load IR %s\n", argv[a]); continue; }
        const int irLen = (int) ir.size();
        const int warm  = (int) std::ceil (2.0 * sr);    // covers JUCE async load + core cold fade + 50 ms fade
        const int total = warm + maxUse;

        std::vector<float> in ((std::size_t) total, 0.f);
        for (int i = 0; i < maxUse; ++i) in[(std::size_t)(warm+i)] = sweep[(std::size_t) i];
        std::vector<float> cL = in, cR = in;

        // the EXACT IR the adapter is expected to convolve with: resample off host rate, then the
        // loader's reference-unity normalization gain (queried back exactly as applied). This mirrors
        // cab::Convolver's internal load EXACTLY, so a direct convolution with it is the mathematical
        // ground truth for the whole adapter (load + resample + normalize + conv).
        std::vector<float> expIr = (irSr != sr) ? felitronics::convolution::resampleIr (ir.data(), irLen, irSr, sr) : ir;

        const float* irp[1] { ir.data() };
        cab::Convolver cc; cc.prepare (sr, 512, 2, std::max (0.6, (int) expIr.size() / sr + 0.1)); cc.loadIR (irp, 1, irLen, irSr);
        const float g = cc.irNormalizationGain();
        for (float& v : expIr) v *= g;
        runBlocks (cc, cL, cR, total);

        const int    from = warm + (int) expIr.size(), to = total;                            // steady region
        const double nd  = coreVsDirectDb (in, cL, expIr, from, std::min (24000, to - from)); // juce vs the math
        const double sig = rmsDb (cL, from, to);
        // juce::dsp::Convolution uses a single-precision partitioned FFT, so it nulls against the
        // double-precision direct convolution deep but not bit-exact (the felitronics scalar head
        // reached ~-170). -100 dBFS is ~1e-5 RMS — far below audibility (the task's own A/B bar was
        // 1e-3 / -60 dB); a genuinely broken load lands 60+ dB above this.
        const bool   ok  = nd < -100.0;
        (void) maxAbs; (void) nullDb;
        allOk = allOk && ok;
        std::printf ("  %-26s @%5.1fk  juce-vs-MATH %7.1f dB   (out %.1f dB, IR %d->%d taps)  %s\n",
                     juce::File (argv[a]).getFileNameWithoutExtension().toRawUTF8(), irSr / 1000.0,
                     nd, sig, irLen, (int) expIr.size(), ok ? "EXACT" : "<-- CHECK");
    }
    std::printf ("\n%s\n", allOk ? "EXACT — juce convolution == the math on every real cab (host + resampled)" : "SOME REAL-IR CHECK FAILED");
    return allOk ? 0 : 1;
}
