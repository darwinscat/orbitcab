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
#include "core/DryAlignCapacity.h"
#include "core/Params.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <cstring>
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

    // The window bestFitDelay searches for a path that should carry `latency`: twice that, and never
    // less than 8 — a path delayed by anything from nothing to double is found where it actually is. A
    // FIXED window (it was 24, sized on the old 9-sample figure) goes blind the day the core's number
    // outgrows it and returns a plausible wrong answer ("10") instead of the real one.
    int fitWindow (int latency) { return 2 * latency + 8; }

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
                    // The LENGTH is the core's (9 samples under its old kernel, 96 under the 64-tap one, and it
                    // may move again) — this gate owns only that the dry path carries exactly what is reported.
                    expect (Ln > 0, "precondition: a 48k model at 96 kHz rate-matches, so there is a latency to carry");
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
                expect (L > 0, "precondition: the armed model rate-matches at 96 kHz");
                const auto in  = distinctSignal (12000);
                const auto out = runEngine (e, p, in, 128);
                expectEquals (bestFitDelay (out, in, fitWindow (L)), L,
                              "preamp-OFF dry must be delayed by the armed model's rate-match latency");
            }
           #endif
        }

        // THE WET PATH, which every gate above leaves unmeasured (they bypass the stage, so they check the dry
        // copy against the reported number and never the number against the signal). With the stage ON, an
        // identity capture — a 1-tap Linear .nam — makes the output the input delayed by exactly what the
        // rate-match really costs, and that must BE the reported latency: the host compensates it and the bypass
        // paths copy it. Asserted where the core publishes its latency bound (felitronics-core >= v0.30.0).
        // v0.13.1 reports more than its path costs (6 vs 4 samples at 44.1 kHz, 9 vs 6 at 96, 27 vs 18 at 384)
        // and its resampler crashes above a 4:1 ratio (216 - 352.8 kHz): there this gate could only measure the
        // defect the bump removes, so it is skipped and says so.
        beginTest ("the WET path arrives exactly the reported latency late (identity capture, 44.1k .. 384k)");
        {
            if constexpr (! cab::PublishesLatencyBound<AmpStage>)
            {
                logMessage ("  skipped: this core publishes no latency bound, and its reported latency is not its path's");
                expect (true);
            }
            else
            {
                const char* identity = R"({"version":"0.5.2","architecture":"Linear","config":{"receptive_field":1,"bias":false},"weights":[1.0],"sample_rate":48000})";
                for (double rate : { 44100.0, 88200.0, 96000.0, 192000.0, 216000.0, 352800.0, 384000.0 })
                    for (int block : { 7, 64, 512 })
                        for (bool capture : { false, true })
                        {
                            cab::Params p; p.autoLevel = false; p.slot[0].dryWet01 = 0.0f;
                            p.preampOn = ! capture; p.ampOn = capture;
                            if (capture) p.powerAmpMode = cab::PowerAmpMode::capture;
                            cab::CabEngine e; e.prepare (rate, block, 2, p);
                            const juce::String where = juce::String (rate, 0) + " block " + juce::String (block)
                                                     + (capture ? " capture" : " preamp");
                            const bool loaded = capture ? e.loadAmpModelBytes (identity, std::strlen (identity))
                                                        : e.loadPreampModelBytes (identity, std::strlen (identity));
                            expect (loaded, "identity capture loads at " + where);
                            const int L = capture ? e.ampLatencySamples() : e.preampLatencySamples();
                            expect (L > 0, "precondition (rate-match) at " + where);
                            const auto in  = distinctSignal (16000);
                            const auto out = runEngine (e, p, in, block);
                            expectEquals (bestFitDelay (out, in, fitWindow (L)), L, "wet path == reported latency at " + where);
                        }
            }
        }

        // Every host rate the rate-match meets, both bypass paths: the dry carries EXACTLY the reported
        // latency. This is the gate the 256-sample ring failed — at 352.8 and 384 kHz it clamped the core's
        // 267 and 288 to 255, silently, while the host compensated the full amount. 705.6 and 768 kHz
        // reach past every ring a CONSTANT could have been sized for on either core (48 and 51 on v0.13.1,
        // 502 and 544 on v0.30.0), so a capacity that stops following the core fails here on both.
        beginTest ("CabEngine bypass paths carry exactly the reported latency at every host rate (44.1k .. 768k)");
        {
           #ifdef ORBITCAB_RES_DIR
            const auto bytes = loadTestModelBytes();
            expect (bytes.getSize() > 0, "embedded test .nam present");
            if (bytes.getSize() > 0)
                for (int rate : { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000 })   // integers: compared below
                    for (bool capture : { false, true })
                    {
                        cab::Params p; p.autoLevel = false; p.slot[0].dryWet01 = 0.0f;
                        p.preampOn = false; p.ampOn = false;
                        if (capture) p.powerAmpMode = cab::PowerAmpMode::capture;
                        cab::CabEngine e; e.prepare (rate, prepBlock, 2, p);
                        const juce::String where = juce::String (rate) + (capture ? " capture" : " preamp");
                        const bool loaded = capture ? e.loadAmpModelBytes (bytes.getData(), bytes.getSize())
                                                    : e.loadPreampModelBytes (bytes.getData(), bytes.getSize());
                        expect (loaded, "model loads at " + where);
                        const int L = capture ? e.ampLatencySamples() : e.preampLatencySamples();
                        // Precondition: at the model's own 48 kHz nothing rate-matches; anywhere else it does.
                        expect (rate == 48000 ? L == 0 : L > 0, "precondition (rate-match) at " + where);
                        const auto in  = distinctSignal (16000);
                        const auto out = runEngine (e, p, in, 128);
                        expectEquals (bestFitDelay (out, in, fitWindow (L)), L, "dry == reported latency at " + where);
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
                expect (L > 0, "precondition: the armed capture rate-matches at 96 kHz");
                const auto in  = distinctSignal (12000);
                const auto out = runEngine (e, p, in, 128);
                expectEquals (bestFitDelay (out, in, fitWindow (L)), L,
                              "poweramp-OFF dry must be delayed by the armed capture's rate-match latency");
            }
           #endif
        }
    }
};

static PowerAmpRouterAlignTest powerAmpRouterAlignTest;
