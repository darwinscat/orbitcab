// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// PowerAmpRouter latency-ALIGNMENT gate. The OFF/dry path must be delayed by EXACTLY the active
// stage's reported latency, so toggling the poweramp power never changes PDC and an off<->active
// crossfade stays time-aligned (the fix that killed the enable/disable gap + jump). This is bit-
// exact by construction: the off path is a pure copy through the alignment ring, so a delayed-dry
// sample must equal the input sample from EXACTLY N samples earlier — no arithmetic, no tolerance.
// Designed to BREAK on any off-by-one in the ring, a stale/wrong tap, a bad ring-wrap at ragged
// block sizes, or the wrong tap being chosen for tube vs capture mode. CI gate.
#include "poweramp/PowerAmpRouter.h"
#include "core/AmpStage.h"
#include "core/CabEngine.h"
#include "core/DryAligner.h"
#include "core/Params.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

using cab::AmpStage;
using cab::PowerAmpMode;
using cab::poweramp::PowerAmpRouter;

namespace
{
    // Distinct-per-sample signal so a delay is detectable sample-exact (no value repeats within the
    // window). Pure LCG → reproducible, independent of juce::Random's shared state.
    std::vector<float> distinctSignal (int n, juce::uint32 seed = 0x1234567u)
    {
        std::vector<float> v ((size_t) n);
        juce::uint32 s = seed;
        for (int i = 0; i < n; ++i)
        {
            s = s * 1664525u + 1013904223u;
            v[(size_t) i] = (float) ((double) s / 4294967296.0 - 0.5);
        }
        return v;
    }

    // Run `in` through the router in fixed `block`-sized chunks (mono duplicated to stereo) on the OFF
    // path (ampOn = false), so ONLY the alignment delay is exercised. Returns channel-0 output.
    std::vector<float> runOff (PowerAmpRouter& r, AmpStage& nam, PowerAmpMode mode,
                               const std::vector<float>& in, int block)
    {
        std::vector<float> out (in.size(), 0.0f);
        cab::TubeParams tp;                       // defaults; the off path never touches the tube
        std::vector<float> L ((size_t) block), R ((size_t) block);
        for (size_t off = 0; off < in.size(); off += (size_t) block)
        {
            const int n = (int) juce::jmin ((size_t) block, in.size() - off);
            for (int i = 0; i < n; ++i) { L[(size_t) i] = in[off + (size_t) i]; R[(size_t) i] = L[(size_t) i]; }
            float* io[2] = { L.data(), R.data() };
            r.process (io, 2, n, /*ampOn*/ false, mode, tp, nam);
            for (int i = 0; i < n; ++i) out[off + (size_t) i] = L[(size_t) i];
        }
        return out;
    }

    // True iff `out[n] == in[n - d]` for every n >= d (bit-exact; the warm-up region n < d is ignored).
    bool isDelayedBy (const std::vector<float>& out, const std::vector<float>& in, int d)
    {
        for (size_t n = (size_t) d; n < in.size(); ++n)
            if (out[n] != in[n - (size_t) d]) return false;
        return true;
    }

    // Run `in` through a DryAligner (1 ch) at fixed delay D in `block`-sized chunks; return the output.
    std::vector<float> runAligner (cab::DryAligner& a, const std::vector<float>& in, int D, int block)
    {
        std::vector<float> out (in.size(), 0.0f), buf ((size_t) block);
        for (size_t off = 0; off < in.size(); off += (size_t) block)
        {
            const int n = (int) juce::jmin ((size_t) block, in.size() - off);
            for (int i = 0; i < n; ++i) buf[(size_t) i] = in[off + (size_t) i];
            const float* io[1] = { buf.data() };
            a.advance (io, 1, n, D);
            for (int i = 0; i < n; ++i) out[off + (size_t) i] = a.delayed (0)[i];
        }
        return out;
    }

    // Best-fit integer delay of `out` vs `in` over [0, maxD] (min mean-squared error). Robust to the
    // tiny (~1e-6) non-idempotence of the full engine chain — pins the delay without bit-exactness.
    int bestFitDelay (const std::vector<float>& out, const std::vector<float>& in, int maxD)
    {
        int best = 0; double bestErr = 1.0e300;
        for (int d = 0; d <= maxD; ++d)
        {
            double e = 0.0; int c = 0;
            for (size_t n = (size_t) maxD; n < in.size(); ++n) { const double df = (double) out[n] - (double) in[n - (size_t) d]; e += df * df; ++c; }
            if (c > 0 && e / (double) c < bestErr) { bestErr = e / (double) c; best = d; }
        }
        return best;
    }

    // Run `in` through a CabEngine in `block`-sized chunks with params `p`; return channel-0 output.
    std::vector<float> runEngine (cab::CabEngine& e, const cab::Params& p, const std::vector<float>& in, int block)
    {
        std::vector<float> out (in.size(), 0.0f), L ((size_t) block), R ((size_t) block);
        for (size_t off = 0; off < in.size(); off += (size_t) block)
        {
            const int n = (int) juce::jmin ((size_t) block, in.size() - off);
            for (int i = 0; i < n; ++i) { L[(size_t) i] = in[off + (size_t) i]; R[(size_t) i] = L[(size_t) i]; }
            float* io[2] = { L.data(), R.data() };
            e.process (io, 2, n, p, false);
            for (int i = 0; i < n; ++i) out[off + (size_t) i] = L[(size_t) i];
        }
        return out;
    }

    // The factory 48k preamp used by the router test, as an embedded resource file (any 48k .nam works).
    juce::MemoryBlock loadTestModelBytes()
    {
        juce::MemoryBlock mb;
       #ifdef ORBITCAB_RES_DIR
        juce::File (ORBITCAB_RES_DIR).getChildFile ("preamps/V4KRAK-red-12h.namz").loadFileAsData (mb);
       #endif
        return mb;
    }
}

struct PowerAmpRouterAlignTest : juce::UnitTest
{
    PowerAmpRouterAlignTest() : juce::UnitTest ("PowerAmpRouter latency alignment") {}

    void runTest() override
    {
        const double sr = 96000.0; const int prepBlock = 512;

        beginTest ("DryAligner: exact delay at arbitrary taps across ragged block sizes (bit-exact)");
        {
            const auto in = distinctSignal (7000);
            for (int D : { 0, 1, 5, 31, 100, 255 })
                for (int blk : { 1, 7, 64, 128, 300, 512 })   // incl. blocks LARGER than the 256 ring capacity
                {
                    cab::DryAligner a; a.prepare (1, blk, 256);
                    const auto out = runAligner (a, in, D, blk);
                    expect (isDelayedBy (out, in, D),
                            "exact delay D=" + juce::String (D) + " at block size " + juce::String (blk));
                }
        }

        beginTest ("DryAligner: a tap beyond capacity clamps (no overrun / crash)");
        {
            const auto in = distinctSignal (3000);
            cab::DryAligner a; a.prepare (1, prepBlock, 256);
            const auto out = runAligner (a, in, 1000, 64);      // capacity 256 → clamps to 255
            expect (isDelayedBy (out, in, 255), "over-capacity tap clamps to capacity-1");
        }

        beginTest ("tube-mode OFF delays the dry by EXACTLY the tube latency (bit-exact)");
        {
            PowerAmpRouter r; AmpStage nam;
            r.prepare (sr, prepBlock, 2); nam.prepare (sr, prepBlock);
            const int Lt = r.tubeLatencySamples();
            expect (Lt > 0, "tube must report real (oversampling) latency");
            const auto in  = distinctSignal (8000);
            const auto out = runOff (r, nam, PowerAmpMode::tube, in, 64);
            expect (isDelayedBy (out, in, Lt),     "off = dry delayed by exactly the tube latency");
            expect (! isDelayedBy (out, in, Lt - 1), "delay is not L-1 (off-by-one guard)");
            expect (! isDelayedBy (out, in, Lt + 1), "delay is not L+1 (off-by-one guard)");
        }

        beginTest ("capture-mode OFF with no model is a bit-identical passthrough (tap 0)");
        {
            PowerAmpRouter r; AmpStage nam;                 // no model loaded → latency 0
            r.prepare (sr, prepBlock, 2); nam.prepare (sr, prepBlock);
            expect (nam.latencySamples() == 0);
            const auto in  = distinctSignal (4000);
            const auto out = runOff (r, nam, PowerAmpMode::capture, in, 100);
            expect (isDelayedBy (out, in, 0), "a 0-latency capture off path must be exact identity");
        }

        beginTest ("tube-mode OFF delay holds across ragged block sizes (ring-wrap stress)");
        {
            PowerAmpRouter r; AmpStage nam;
            r.prepare (sr, prepBlock, 2); nam.prepare (sr, prepBlock);
            const int Lt = r.tubeLatencySamples();
            const auto in = distinctSignal (6000);
            for (int blk : { 1, 3, 7, 32, 63, 128, 200 })
            {
                r.reset();                                  // cold ring per run
                const auto out = runOff (r, nam, PowerAmpMode::tube, in, blk);
                expect (isDelayedBy (out, in, Lt), "exact delay must survive block size " + juce::String (blk));
            }
        }

        beginTest ("capture-mode OFF with a real 48k model at 96k delays by the reported rate-match latency");
        {
           #ifdef ORBITCAB_RES_DIR
            const juce::File nf = juce::File (ORBITCAB_RES_DIR).getChildFile ("preamps/V4KRAK-red-12h.namz");
            expect (nf.existsAsFile(), "test resource .nam must exist: " + nf.getFullPathName());
            if (nf.existsAsFile())
            {
                juce::MemoryBlock mb; nf.loadFileAsData (mb);
                PowerAmpRouter r; AmpStage nam;
                r.prepare (sr, prepBlock, 2);
                nam.prepare (sr, prepBlock);                // sets the 48k default run-rate + resampling@96k
                const bool ok = nam.loadModelFromMemory (mb.getData(), mb.getSize());   // 48k model → accepted
                expect (ok, "a 48k factory .nam must load");
                if (ok)
                {
                    const int Ln = nam.latencySamples();
                    expectEquals (Ln, (int) std::ceil (3.0 * sr / 48000.0) + 3);   // the rate-match formula = 9 @ 96k
                    const auto in  = distinctSignal (8000);
                    const auto out = runOff (r, nam, PowerAmpMode::capture, in, 64);
                    expect (isDelayedBy (out, in, Ln), "capture off = dry delayed by EXACTLY the reported latency");
                }
            }
           #endif
        }

        // Full-engine guards: with a rate-matching model ARMED, a bypassed/off NAM stage must still emit
        // the dry delayed to that stage's PDC (so the reported latency and the signal agree, and toggling
        // the power never shifts either). Reverting to a hard bypass (no delay) makes bestFitDelay == 0.
        beginTest ("CabEngine PREAMP bypass is latency-aligned to the armed model at 96 kHz");
        {
           #ifdef ORBITCAB_RES_DIR
            const auto bytes = loadTestModelBytes();
            expect (bytes.getSize() > 0, "embedded test .nam present");
            if (bytes.getSize() > 0)
            {
                cab::Params p; p.autoLevel = false; p.slot[0].dryWet01 = 0.0f;   // pure post-amp dry
                p.preampOn = false; p.ampOn = false;                            // preamp bypassed, poweramp off
                cab::CabEngine e; e.prepare (sr, prepBlock, 2, p);
                expect (e.loadPreampModelBytes (bytes.getData(), bytes.getSize()), "preamp model loads");
                const int L = e.preampLatencySamples();
                expectEquals (L, (int) std::ceil (3.0 * sr / 48000.0) + 3);      // 9 @ 96k
                const auto in  = distinctSignal (12000);
                const auto out = runEngine (e, p, in, 128);
                expectEquals (bestFitDelay (out, in, 24), L,
                              "preamp-OFF dry must be delayed by the armed model's rate-match latency");
            }
           #endif
        }

        beginTest ("CabEngine CAPTURE (poweramp) bypass is latency-aligned to the armed model at 96 kHz");
        {
           #ifdef ORBITCAB_RES_DIR
            const auto bytes = loadTestModelBytes();
            if (bytes.getSize() > 0)
            {
                cab::Params p; p.autoLevel = false; p.slot[0].dryWet01 = 0.0f;
                p.preampOn = false; p.ampOn = false; p.powerAmpMode = cab::PowerAmpMode::capture;
                cab::CabEngine e; e.prepare (sr, prepBlock, 2, p);
                expect (e.loadAmpModelBytes (bytes.getData(), bytes.getSize()), "capture model loads");
                const int L = e.ampLatencySamples();
                expectEquals (L, (int) std::ceil (3.0 * sr / 48000.0) + 3);
                const auto in  = distinctSignal (12000);
                const auto out = runEngine (e, p, in, 128);
                expectEquals (bestFitDelay (out, in, 24), L,
                              "poweramp-OFF dry must be delayed by the armed capture's rate-match latency");
            }
           #endif
        }
    }
};

static PowerAmpRouterAlignTest powerAmpRouterAlignTest;
