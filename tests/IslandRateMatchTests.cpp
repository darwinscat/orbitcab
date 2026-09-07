// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// The front section rate-matches ONCE. This suite is the gate on all three halves of that claim:
//
//  1. AT 48 kHz NOTHING CONVERTS — and nothing else changed either: with a capture armed and the
//     whole front section transparent, the engine returns its input BIT-FOR-BIT.
//  2. AT 44.1 kHz WITH NO CAPTURE ARMED nothing converts either. This is the configuration OrbitCab
//     BOOTS in (`ampOn`/`preampOn` default false, so applyPreamp/applyPoweramp never load a model),
//     and it is the reason the island is engaged on the armed set rather than on the host rate: an
//     always-on island would have put a −4.17 dB droop at 17.64 kHz on a plain cab-loader rig.
//  3. AT 44.1 kHz WITH A CAPTURE ARMED the chain makes exactly ONE round trip. Measured on the real
//     engine as a per-sample complex gain (the transparent path is linear, so a cos run and a sin run
//     combine into one), against the closed-form Catmull-Rom cascade. The row it has to hit is the
//     "one round trip" row of felitronics-core's docs/STREAM-RESAMPLER-COST.md, NOT the ×2 row that
//     two independent NAM stages produced: −4.17 dB coherent and −0.61 dB best phase at 17.64 kHz,
//     against −9.03 / −5.20 before. The BEST-phase figure is the one that matters — at −5.20 dB the
//     top octave is down at every interpolation phase, i.e. a tone loss rather than a roughness.
//
// The stage latencies are 0 throughout on purpose: both NAM stages are prepared AT the model rate,
// so their own rate-matchers never engage. The whole chain's PDC is the island's round trip,
// `lround(2 + 2*hostSR/modelRunSR)` — the geometry, checked here against the audio itself.
#include "core/CabEngine.h"
#include "core/Params.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <complex>
#include <vector>

namespace
{
    // Distinct-per-sample and never zero, so an exact comparison cannot pass by accident and
    // `x + 0.0f` can never turn a −0.0f into a +0.0f behind our backs.
    std::vector<float> distinctSignal (int n, juce::uint32 seed = 0x51ed270bu)
    {
        std::vector<float> v ((size_t) n);
        juce::uint32 s = seed;
        for (int i = 0; i < n; ++i)
        {
            s = s * 1664525u + 1013904223u;
            const float x = (float) ((double) s / 4294967296.0 - 0.5);
            v[(size_t) i] = (std::abs (x) > 1.0e-6f) ? x : 0.25f;
        }
        return v;
    }

    // Everything the engine can do, switched OFF: no cab in either slot, no EQ, no gate, no spring,
    // both NAM stages powered down, no auto-level, unity trims. What is left is the front section's
    // wiring — which is exactly what this suite measures.
    cab::Params transparentParams()
    {
        cab::Params p;
        p.aLoaded = p.bLoaded = false;      // empty slots => the mix is the post-amp dry, untouched
        p.autoLevel   = false;
        p.inputGainDb = 0.0f;
        p.outputGainDb = 0.0f;
        p.preampVolumeDb = 0.0f;
        p.preampOn = false;                 // ARMED but not powered: the island engages, the model never runs
        p.ampOn    = false;
        p.powerAmpMode = cab::PowerAmpMode::capture;
        p.eq.on = false;
        p.gate.on = false;
        p.reverb.type = 0;
        p.monoAmp = false;
        return p;
    }

    // A short decaying noise burst: enough of a tank to make the detour real, no bundled file needed.
    std::vector<float> syntheticIr (int n, juce::uint32 seed = 0x9876u)
    {
        std::vector<float> ir ((size_t) n);
        juce::uint32 s = seed;
        for (int i = 0; i < n; ++i)
        {
            s = s * 1664525u + 1013904223u;
            ir[(size_t) i] = (float) (((double) s / 4294967296.0 - 0.5)
                                      * std::exp (-4.0 * (double) i / (double) n) * 0.5);
        }
        ir[0] = 1.0f;
        return ir;
    }

    juce::MemoryBlock testModelBytes()
    {
        juce::MemoryBlock mb;
       #ifdef ORBITCAB_RES_DIR
        juce::File (ORBITCAB_RES_DIR).getChildFile ("preamps/V4KRAK-red-12h.namz").loadFileAsData (mb);
       #endif
        return mb;
    }

    // Push `in` through the engine in fixed blocks (mono duplicated to stereo); return channel 0.
    std::vector<float> render (cab::CabEngine& e, const cab::Params& p,
                               const std::vector<float>& in, int block)
    {
        std::vector<float> out (in.size(), 0.0f);
        std::vector<float> L ((size_t) block), R ((size_t) block);
        for (size_t off = 0; off < in.size(); off += (size_t) block)
        {
            const int n = (int) juce::jmin ((size_t) block, in.size() - off);
            for (int i = 0; i < n; ++i) { L[(size_t) i] = in[off + (size_t) i]; R[(size_t) i] = L[(size_t) i]; }
            float* io[2] { L.data(), R.data() };
            e.process (io, 2, n, p, /*nonRealtime*/ false);
            for (int i = 0; i < n; ++i) out[off + (size_t) i] = L[(size_t) i];
        }
        return out;
    }

    std::vector<float> tone (int n, double f, double sr, bool sine)
    {
        std::vector<float> v ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double a = 2.0 * juce::MathConstants<double>::pi * f * (double) i / sr;
            v[(size_t) i] = (float) (sine ? std::sin (a) : std::cos (a));
        }
        return v;
    }

    struct Axes { double coherentDb = 0.0, worstDb = 0.0, bestDb = 0.0; };

    // The transparent path is LINEAR, so a cos run and a sin run combine into the exact response to a
    // complex exponential — i.e. a per-sample complex gain, with no bucketing and no spectral grid.
    // The trap this avoids is the one felitronics-core's P32 tripped over three times: the quantity is
    // periodic (147 output samples at 44.1/48), and any oracle that averages over another period
    // averages away the very thing being measured. Here nothing is averaged but the carrier itself,
    // deliberately, and the two phase extremes are read from the same per-sample sequence.
    Axes measureAxes (double sr, double f, bool armModel, int block)
    {
        const auto bytes = testModelBytes();
        const int  N = 44100;                       // 1 s: many whole periods of the 147-sample cycle
        const auto p = transparentParams();

        auto run = [&] (bool sine)
        {
            cab::CabEngine e;
            e.prepare (sr, block, 2, p);
            if (armModel && bytes.getSize() > 0)
                e.loadPreampModelBytes (bytes.getData(), bytes.getSize());
            return render (e, p, tone (N, f, sr, sine), block);
        };
        const auto yc = run (false), ys = run (true);

        // The ideal is the input delayed by the GEOMETRY of the conversion (fractional on purpose —
        // rounding it here would smear the carrier's phase into the magnitude).
        const double d = armModel ? 2.0 + 2.0 * sr / 48000.0 : 0.0;
        const int    skip = 8820;                   // past the priming transient (0.2 s)
        const int    span = 147 * 200;              // a whole number of composite periods
        std::complex<double> sum { 0.0, 0.0 };
        double lo = 1.0e30, hi = 0.0;
        for (int n = skip; n < skip + span && n < N; ++n)
        {
            const double a = 2.0 * juce::MathConstants<double>::pi * f * ((double) n - d) / sr;
            const std::complex<double> ideal = std::polar (1.0, a);
            const std::complex<double> g = std::complex<double> (yc[(size_t) n], ys[(size_t) n]) / ideal;
            sum += g;
            lo = std::min (lo, std::abs (g));
            hi = std::max (hi, std::abs (g));
        }
        const double m = std::abs (sum) / (double) span;
        auto db = [] (double x) { return 20.0 * std::log10 (std::max (1.0e-30, x)); };
        return { db (m), db (lo), db (hi) };
    }

    // First index where |out| rises above a floor — the integer part of the delay, which the carrier
    // phase alone cannot resolve (an instrument of a DIFFERENT nature, not a second implementation
    // of the same one).
    int onset (const std::vector<float>& v, float floorAbs)
    {
        for (size_t i = 0; i < v.size(); ++i)
            if (std::abs (v[i]) > floorAbs) return (int) i;
        return -1;
    }
}

struct IslandRateMatchTest : juce::UnitTest
{
    IslandRateMatchTest() : juce::UnitTest ("Front-section rate match (the model-rate island)") {}

    void runTest() override
    {
        const auto bytes = testModelBytes();
        const auto p = transparentParams();

        beginTest ("48 kHz host: no island exists, and a capture ARMED changes nothing bit-for-bit");
        {
            cab::CabEngine e;
            e.prepare (48000.0, 512, 2, p);
            expect (! e.islandPossible(), "at the model's own rate there is nothing to convert");
            expectEquals (e.frontRateMatchLatencySamples (true), 0);
            expectEquals (e.frontRateMatchLatencySamples (false), 0);
            if (bytes.getSize() > 0)
            {
                expect (e.loadPreampModelBytes (bytes.getData(), bytes.getSize()), "a 48k capture loads");
                expect (! e.islandEngagedFor (true), "an armed capture must NOT engage an island at 48 kHz");
                expectEquals (e.frontRateMatchLatencySamples (true), 0);
                expectEquals (e.preampLatencySamples(), 0);
            }
            const auto in = distinctSignal (8192);
            for (int blk : { 1, 7, 64, 511, 512 })
            {
                cab::CabEngine f;
                f.prepare (48000.0, 512, 2, p);
                if (bytes.getSize() > 0) f.loadPreampModelBytes (bytes.getData(), bytes.getSize());
                const auto out = render (f, p, in, blk);
                bool same = true;
                for (size_t i = 0; i < in.size(); ++i) same = same && (out[i] == in[i]);
                expect (same, "48 kHz transparent path is bit-exact at block " + juce::String (blk));
            }
        }

        beginTest ("44.1 kHz with NO capture armed: still not one sample converted");
        {
            cab::CabEngine e;
            e.prepare (44100.0, 512, 2, p);
            expect (e.islandPossible(), "44.1 kHz is off the model's rate, so an island is possible");
            expect (! e.islandEngagedFor (true),  "no model armed => no island");
            expect (! e.islandEngagedFor (false), "no model armed => no island in tube mode either");
            expectEquals (e.frontRateMatchLatencySamples (true), 0);
            const auto in = distinctSignal (8192);
            for (int blk : { 1, 7, 64, 511, 512 })
            {
                cab::CabEngine f;
                f.prepare (44100.0, 512, 2, p);
                const auto out = render (f, p, in, blk);
                bool same = true;
                for (size_t i = 0; i < in.size(); ++i) same = same && (out[i] == in[i]);
                expect (same, "no-capture 44.1 kHz path is bit-exact at block " + juce::String (blk));
            }
        }

        beginTest ("44.1 kHz with a capture armed: the PDC is the geometry, and the audio agrees");
        {
            if (bytes.getSize() > 0)
            {
                cab::CabEngine e;
                e.prepare (44100.0, 512, 2, p);
                expect (e.loadPreampModelBytes (bytes.getData(), bytes.getSize()), "capture loads");
                expect (e.islandEngagedFor (true), "an armed capture engages the island");
                // 2 host samples on the way down + 2 model samples on the way back = 3.8375 -> 4.
                expectEquals (e.frontRateMatchLatencySamples (true), 4);
                expectEquals (e.preampLatencySamples(), 0);   // the stage runs AT the model rate
                expectEquals (e.ampLatencySamples(), 0);

                // An impulse resolves the INTEGER part of the delay, which a carrier phase cannot.
                std::vector<float> imp (4096, 0.0f); imp[64] = 1.0f;
                const auto out = render (e, p, imp, 128);
                const int k = onset (out, 1.0e-4f);
                expect (k >= 64 && k <= 64 + 5,
                        "impulse onset lands within the round trip, at " + juce::String (k - 64));
            }
        }

        beginTest ("44.1 kHz: the island never pads, never drops, never repeats — DC through it is DC");
        {
            // The one thing a two-stage conversion can do silently is come up SHORT: produceExact()
            // pads with zeros when the up leg has not been handed enough model frames, and a dropped or
            // repeated sample would look like nothing in a spectrum. Catmull-Rom weights are a partition
            // of unity at every phase, so a constant input must come out as the SAME constant, exactly —
            // and any pad, drop, repeat or stale read breaks that at the sample where it happens.
            // Ragged blocks on purpose: the model-frame count per host block alternates 557/558 at 512,
            // and nothing downstream may assume either.
            if (bytes.getSize() > 0)
            {
                for (int blk : { 1, 7, 64, 128, 300, 511, 512 })
                {
                    cab::CabEngine e;
                    e.prepare (44100.0, 512, 2, p);
                    expect (e.loadPreampModelBytes (bytes.getData(), bytes.getSize()), "capture loads");
                    const std::vector<float> dc (30000, 1.0f);
                    const auto out = render (e, p, dc, blk);
                    float worst = 0.0f; int worstAt = -1;
                    for (size_t i = 1000; i < out.size(); ++i)      // past the priming ramp
                        if (std::abs (out[i] - 1.0f) > worst) { worst = std::abs (out[i] - 1.0f); worstAt = (int) i; }
                    expect (worst < 1.0e-5f,
                            "block " + juce::String (blk) + ": worst DC deviation " + juce::String (worst, 8)
                            + " at sample " + juce::String (worstAt));
                }
            }
        }

        beginTest ("44.1 kHz: a poweramp A/B must not touch the gate — no hole, no lost hysteresis");
        {
            // The island's engagement depends on the poweramp MODE (in tube mode there is no second NAM
            // stage to save a trip on), so a capture<->tube A/B flips it — and the rate-designed stages
            // follow. A gate that re-prepared there would restart CLOSED and then judge with the OPEN
            // threshold, so a note sustaining INSIDE the hysteresis window would never reopen: measured
            // on the bare gate, a 17-40 sample hole at -90 dB and then silence until the next attack.
            // This pins that the A/B costs neither.
            if (bytes.getSize() > 0)
            {
                cab::Params g = transparentParams();
                g.ampOn = true;                       // armed capture, powered, capture mode
                g.gate.on = true;
                g.gate.thresholdDb = -46.0f;
                cab::CabEngine e;
                e.prepare (44100.0, 512, 2, g);
                expect (e.loadAmpModelBytes (bytes.getData(), bytes.getSize()), "capture loads");
                expect (e.islandEngagedFor (true),  "capture mode + armed capture => island");
                expect (! e.islandEngagedFor (false), "tube mode + no preamp => no island");

                // A note that ARRIVES loud (so the gate opens) and then decays to a level between the
                // close and open thresholds — held open only by the Schmitt hysteresis, which is exactly
                // the state a reset destroys.
                const int N = 44100;
                std::vector<float> in ((size_t) N);
                for (int i = 0; i < N; ++i)
                {
                    const float amp = (i < 8192) ? 0.25f : 0.0035f;   // pick, then a held decay
                    in[(size_t) i] = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 196.0 * (double) i / 44100.0);
                }

                std::vector<float> L (512), R (512);
                float gateBefore = 1.0f, worstAfter = 1.0f;
                for (int off = 0, blk = 0; off + 512 <= N; off += 512, ++blk)
                {
                    if (blk == 40) { gateBefore = e.gateGain(); g.powerAmpMode = cab::PowerAmpMode::tube; }
                    for (int i = 0; i < 512; ++i) { L[(size_t) i] = in[(size_t) (off + i)]; R[(size_t) i] = L[(size_t) i]; }
                    float* io[2] { L.data(), R.data() };
                    e.process (io, 2, 512, g, false);
                    if (blk > 40) worstAfter = juce::jmin (worstAfter, e.gateGain());
                }
                expect (gateBefore > 0.9f, "precondition: the gate is OPEN before the A/B (" + juce::String (gateBefore, 4) + ")");
                expect (worstAfter > 0.5f,
                        "the A/B must not shut the gate — worst gain after it was " + juce::String (worstAfter, 6));
            }
        }

        beginTest ("44.1 kHz: the spring detour survives idle/resume without padding the return");
        {
            // The detour's two legs balance over an UNBROKEN sequence of blocks; a subsequence (an idle
            // spring) is a random walk that eventually pads the return with a literal 0.0f and leaves a
            // permanent extra sample of wet lag. With a unit-impulse tank and a DC input, the wet IS the
            // dry, so any pad, drop or repeat shows as an exact zero or a level step in the sum.
            if (bytes.getSize() > 0)
            {
                cab::Params r = transparentParams();
                r.reverb.type = 1; r.reverb.mix01 = 1.0f; r.reverb.scale01 = 1.0f;
                cab::CabEngine e;
                e.prepare (44100.0, 512, 2, r);
                expect (e.loadPreampModelBytes (bytes.getData(), bytes.getSize()), "capture loads");
                std::vector<float> ir (1, 1.0f);                 // a unit impulse: the tank is a wire
                const float* irPlanes[1] { ir.data() };
                e.loadReverbIR (irPlanes, 1, 1, 44100.0);
                for (int i = 0; i < 500; ++i) e.pumpConvolverReloads();

                std::vector<float> L (512), R (512);
                int  zerosAfterPriming = 0;
                bool sawWet = false;
                for (int blk = 0; blk < 400; ++blk)
                {
                    r.reverb.mix01 = ((blk / 7) % 2 == 0) ? 1.0f : 0.0f;   // idle / resume, over and over
                    for (int i = 0; i < 512; ++i) { L[(size_t) i] = 1.0f; R[(size_t) i] = 1.0f; }
                    float* io[2] { L.data(), R.data() };
                    e.process (io, 2, 512, r, false);
                    if (blk > 20 && r.reverb.mix01 > 0.5f)
                    {
                        for (int i = 0; i < 512; ++i)
                        {
                            if (L[(size_t) i] == 0.0f) ++zerosAfterPriming;
                            if (L[(size_t) i] > 1.5f)  sawWet = true;      // dry 1 + wet ~1
                        }
                    }
                }
                expect (sawWet, "precondition: the wet return is actually reaching the sum");
                expectEquals (zerosAfterPriming, 0);
            }
        }

        beginTest ("44.1 kHz with a capture armed: ONE round trip, not two — all three axes");
        {
            if (bytes.getSize() > 0)
            {
                // The closed-form Catmull-Rom cascade, one round trip 44.1 <-> 48 kHz, from
                // felitronics-core docs/STREAM-RESAMPLER-COST.md. The shipped two-stage chain used to
                // read −9.03 / −13.16 / −5.20 at 17 640 Hz; anything near those means the second round
                // trip is still in the path.
                struct Row { double f, coherent, worst, best; };
                const Row rows[] = {
                    { 10000.0, -0.64,  -1.16, -0.11 },
                    { 15000.0, -2.59,  -5.14, -0.41 },
                    { 17640.0, -4.17,  -9.27, -0.61 },
                    { 19000.0, -4.98, -12.20, -0.69 },
                    { 20000.0, -5.48, -14.79, -0.74 },
                };
                for (const auto& r : rows)
                {
                    const Axes a = measureAxes (44100.0, r.f, /*armModel*/ true, 512);
                    logMessage (juce::String (r.f, 0) + " Hz: coherent " + juce::String (a.coherentDb, 2)
                                + " / worst " + juce::String (a.worstDb, 2)
                                + " / best "  + juce::String (a.bestDb, 2) + " dB");
                    expect (std::abs (a.coherentDb - r.coherent) < 0.1,
                            "coherent carrier at " + juce::String (r.f, 0) + " Hz: measured "
                            + juce::String (a.coherentDb, 3) + ", one-round-trip row " + juce::String (r.coherent, 2));
                    expect (std::abs (a.worstDb - r.worst) < 0.1,
                            "worst phase at " + juce::String (r.f, 0) + " Hz: measured "
                            + juce::String (a.worstDb, 3) + ", one-round-trip row " + juce::String (r.worst, 2));
                    expect (std::abs (a.bestDb - r.best) < 0.1,
                            "best phase at " + juce::String (r.f, 0) + " Hz: measured "
                            + juce::String (a.bestDb, 3) + ", one-round-trip row " + juce::String (r.best, 2));
                }
            }
        }
    }
};

static IslandRateMatchTest islandRateMatchTest;
