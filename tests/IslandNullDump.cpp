// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// THE 48 kHz NULL TEST, as a raw dump so two BUILDS can be compared byte for byte.
//
// Any restructuring of the front section has to prove it did not move a single sample at the rate
// where nothing is converted. An in-tree assertion cannot prove that on its own — it only knows this
// build. So this renders the engine with EVERYTHING hot (both NAM captures, the tone stack with all
// six bands live, the gate crossing its threshold, the spring tank, a cab IR, auto-level, a mono
// stretch, ragged blocks, power toggles and a capture<->tube switch mid-stream) and writes the raw
// float32 output. Build it on the revision before the change and on the revision after, and `cmp`.
//
// Usage: orbitcab_island_null_dump <out.raw> [sampleRate]
//        orbitcab_island_null_dump delay [sampleRate]     — print the front section's measured delay
#include "core/CabEngine.h"
#include "core/Params.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

namespace
{
    juce::MemoryBlock modelBytes (const char* rel)
    {
        juce::MemoryBlock mb;
       #ifdef ORBITCAB_RES_DIR
        juce::File (ORBITCAB_RES_DIR).getChildFile (rel).loadFileAsData (mb);
       #endif
        return mb;
    }

    // A guitar-ish stimulus: decaying plucks over a noise bed, so the gate opens and closes, the
    // spring is excited, and the leveler has something non-stationary to chase.
    std::vector<float> stimulus (int n, double sr)
    {
        std::vector<float> v ((size_t) n, 0.0f);
        juce::uint32 s = 0x2f6e2b1u;
        double env = 0.0;
        for (int i = 0; i < n; ++i)
        {
            if (i % (int) (sr * 0.37) == 0) env = 1.0;
            env *= 0.99985;
            s = s * 1664525u + 1013904223u;
            const double noise = (double) s / 4294967296.0 - 0.5;
            const double t = (double) i / sr;
            const double body = std::sin (2.0 * juce::MathConstants<double>::pi * 196.0 * t)
                              + 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1970.0 * t)
                              + 0.2 * std::sin (2.0 * juce::MathConstants<double>::pi * 6100.0 * t);
            v[(size_t) i] = (float) (env * 0.3 * body + 0.0015 * noise);
        }
        return v;
    }

    // A short synthetic "cab" and "spring" so the dump needs no bundled audio files.
    std::vector<float> syntheticIr (int n, double decay, juce::uint32 seed)
    {
        std::vector<float> ir ((size_t) n);
        juce::uint32 s = seed;
        for (int i = 0; i < n; ++i)
        {
            s = s * 1664525u + 1013904223u;
            const double r = (double) s / 4294967296.0 - 0.5;
            ir[(size_t) i] = (float) (r * std::exp (-decay * (double) i / (double) n) * 0.5);
        }
        ir[0] = 1.0f;
        return ir;
    }
}

// The front section's own delay, measured two ways of DIFFERENT natures on a transparent chain with
// a capture merely ARMED: an impulse onset (resolves the whole samples, blind to the fraction) and the
// carrier phase of a low tone (resolves the fraction, blind to whole periods). Neither alone is enough.
static int measureDelay (double sr)
{
    const int maxBlock = 512;
    cab::Params p;
    p.aLoaded = p.bLoaded = false;
    p.autoLevel = false; p.inputGainDb = 0.0f; p.outputGainDb = 0.0f; p.preampVolumeDb = 0.0f;
    p.preampOn = false; p.ampOn = false; p.powerAmpMode = cab::PowerAmpMode::capture;
    p.eq.on = false; p.gate.on = false; p.reverb.type = 0; p.monoAmp = false;

    const auto pre = modelBytes ("preamps/V4KRAK-red-12h.namz");
    auto make = [&] (cab::CabEngine& e)
    {
        e.prepare (sr, maxBlock, 2, p);
        if (pre.getSize() > 0)
        {
            e.loadPreampModelBytes (pre.getData(), pre.getSize());
            e.loadAmpModelBytes    (pre.getData(), pre.getSize());   // BOTH armed: the shipped rig
        }
    };
    auto push = [&] (cab::CabEngine& e, const std::vector<float>& in)
    {
        std::vector<float> out (in.size(), 0.0f), L ((size_t) maxBlock), R ((size_t) maxBlock);
        for (size_t off = 0; off < in.size(); off += (size_t) maxBlock)
        {
            const int n = (int) juce::jmin ((size_t) maxBlock, in.size() - off);
            for (int i = 0; i < n; ++i) { L[(size_t) i] = in[off + (size_t) i]; R[(size_t) i] = L[(size_t) i]; }
            float* io[2] { L.data(), R.data() };
            e.process (io, 2, n, p, false);
            for (int i = 0; i < n; ++i) out[off + (size_t) i] = L[(size_t) i];
        }
        return out;
    };

    const int N = 44100;
    std::vector<float> imp ((size_t) N, 0.0f); imp[1000] = 1.0f;
    cab::CabEngine ei; make (ei);
    const auto oi = push (ei, imp);
    int onsetK = -1;
    for (int i = 0; i < N; ++i) if (std::abs (oi[(size_t) i]) > 1.0e-5f) { onsetK = i; break; }

    const double f = 100.0;
    std::vector<float> tc ((size_t) N), ts ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        const double a = 2.0 * juce::MathConstants<double>::pi * f * (double) i / sr;
        tc[(size_t) i] = (float) std::cos (a); ts[(size_t) i] = (float) std::sin (a);
    }
    cab::CabEngine ec, es; make (ec); make (es);
    const auto yc = push (ec, tc), ys = push (es, ts);
    const double w = 2.0 * juce::MathConstants<double>::pi * f / sr;
    double reAcc = 0.0, imAcc = 0.0;
    for (int n = 8820; n < 8820 + 147 * 200 && n < N; ++n)
    {
        const double c = std::cos (w * (double) n), s2 = std::sin (w * (double) n);
        const double yr = yc[(size_t) n], yi = ys[(size_t) n];        // y = yc + i*ys
        // divide by e^{i w n}: (yr + i yi) * (c - i s2)
        reAcc += yr * c + yi * s2;
        imAcc += yi * c - yr * s2;
    }
    const double ang = std::atan2 (imAcc, reAcc);
    cab::CabEngine er; make (er);
    std::printf ("sr=%.0f  impulse onset = %d samples  |  carrier-phase delay @100 Hz = %.4f samples"
                 "  |  stages report preamp=%d amp=%d\n",
                 sr, onsetK - 1000, -ang / w, er.preampLatencySamples(), er.ampLatencySamples());
    return 0;
}

int main (int argc, char** argv)
{
    if (argc < 2) { juce::Logger::outputDebugString ("usage: dump <out.raw> [sr]"); return 2; }
    if (juce::String (argv[1]) == "delay")
        return measureDelay (argc > 2 ? juce::String (argv[2]).getDoubleValue() : 44100.0);
    const juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    const double sr = (argc > 2) ? juce::String (argv[2]).getDoubleValue() : 48000.0;
    const int maxBlock = 512;

    cab::Params p;
    p.inputGainDb = 1.5f;  p.outputGainDb = -2.0f;  p.preampVolumeDb = 3.0f;
    p.autoLevel = true;
    p.preampOn = true;     p.ampOn = true;          p.powerAmpMode = cab::PowerAmpMode::capture;
    p.eq.on = true; p.eq.bassDb = 4.0f; p.eq.midDb = -5.0f; p.eq.trebleDb = 3.5f; p.eq.presenceDb = 2.0f;
    p.eq.hpfOn = true; p.eq.hpfHz = 90.0f; p.eq.lpfOn = true; p.eq.lpfHz = 9000.0f;
    p.gate.on = true; p.gate.thresholdDb = -46.0f;
    p.reverb.type = 1; p.reverb.mix01 = 0.4f; p.reverb.scale01 = 0.15f;
    p.aLoaded = true;  p.slot[0].dryWet01 = 0.8f;
    p.monoAmp = false;

    cab::CabEngine e;
    e.prepare (sr, maxBlock, 2, p);

    const auto pre = modelBytes ("preamps/V4KRAK-red-12h.namz");
    if (pre.getSize() > 0)
    {
        e.loadPreampModelBytes (pre.getData(), pre.getSize());
        e.loadAmpModelBytes    (pre.getData(), pre.getSize());
    }

    const auto cabIr = syntheticIr (2048, 6.0, 0x1234u);
    const float* cabPlanes[1] { cabIr.data() };
    e.setSlotOriginalIR (0, cabPlanes, 1, (int) cabIr.size(), sr);
    e.slotApplyTrim (0, false, 0.0f, false);

    const auto spring = syntheticIr (12000, 4.0, 0x9876u);
    const float* springPlanes[1] { spring.data() };
    e.loadReverbIR (springPlanes, 1, (int) spring.size(), sr);
    for (int i = 0; i < 200; ++i) e.pumpConvolverReloads();   // land any coalesced swap

    const int  n = (int) (sr * 6.0);
    const auto in = stimulus (n, sr);
    std::vector<float> outL ((size_t) n, 0.0f), outR ((size_t) n, 0.0f);
    std::vector<float> L ((size_t) maxBlock), R ((size_t) maxBlock);

    // Ragged blocks so no boundary is ever aligned to the resampler's own period, plus scheduled
    // events at fixed SAMPLE positions (not block positions) so the two builds see the same stream.
    const int blocks[] = { 1, 7, 64, 128, 300, 511, 512, 33 };
    int bi = 0;
    for (int off = 0; off < n; )
    {
        const int blk = juce::jmin (blocks[bi++ % 8], n - off);
        const double tSec = (double) off / sr;
        p.preampOn     = ! (tSec > 2.0 && tSec < 2.5);                       // power toggle mid-stream
        p.ampOn        = ! (tSec > 3.0 && tSec < 3.4);
        p.powerAmpMode = (tSec > 4.0 && tSec < 5.0) ? cab::PowerAmpMode::tube
                                                    : cab::PowerAmpMode::capture;
        p.monoAmp      = (tSec > 5.2);                                       // mono-fold stretch
        for (int i = 0; i < blk; ++i) { L[(size_t) i] = in[(size_t) (off + i)]; R[(size_t) i] = 0.7f * L[(size_t) i]; }
        float* io[2] { L.data(), R.data() };
        e.process (io, 2, blk, p, /*nonRealtime*/ false);
        for (int i = 0; i < blk; ++i) { outL[(size_t) (off + i)] = L[(size_t) i]; outR[(size_t) (off + i)] = R[(size_t) i]; }
        off += blk;
    }

    out.deleteFile();
    juce::FileOutputStream os (out);
    if (! os.openedOk()) return 3;
    for (int i = 0; i < n; ++i) { os.write (&outL[(size_t) i], sizeof (float)); os.write (&outR[(size_t) i], sizeof (float)); }
    os.flush();
    std::printf ("wrote %d stereo frames at %.0f Hz to %s\n", n, sr, out.getFullPathName().toRawUTF8());
    return 0;
}
