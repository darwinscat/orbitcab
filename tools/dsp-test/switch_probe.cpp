// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Offline DIAGNOSTIC for the capture<->tube switch cleanliness (dev tool; not a CI gate).
// Decomposes the audible switch transient into its independent causes by driving CabEngine
// headless at 48 kHz and printing machine-readable TSV sections:
//
//   gainmap    — capture vs tube post-amp gain across input LEVEL (the A/B step is level-
//                dependent on nonlinear stages; the -18 dBFS corner probe alone can't show that).
//   trace      — output envelope (5.3 ms RMS hop, dB rel. the settled TO-mode level) around a
//                live switch: leveler on/off × direction × stimulus (stationary / guitar bursts),
//                each on a FRESH engine (no state contamination between scenarios), real cab IR.
//   stalewake  — the frozen-stage error isolated as an A/B null: engine A switches away and back
//                (stage resumes from stale state), engine B stays in the mode the whole time; both
//                see the SAME input schedule, so post-switch |A-B| is exactly the stale-state
//                transient (leveler off, no cab — post-amp only).
//
// Usage: orbitcab_switch_probe <model.nam> <cab.wav>
#include "core/CabEngine.h"
#include "core/Params.h"
#include "core/LevelProbe.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace cab;

namespace
{
constexpr double kSR = 48000.0;
constexpr int kMaxBlock = 512;
constexpr int kBlk = 64;                 // engine drive granularity
constexpr int kHopBlocks = 4;            // envelope hop = 4*64 = 256 samples ≈ 5.33 ms

//------------------------------------------------------------------ stimuli
// The SHARED levelprobe LCG (one hash, one place: LevelProbe.h).
inline float lcg (long long n) { return cab::levelprobe::white (n); }

struct Stim
{
    // kind 0: stationary white noise at rmsDb.
    // kind 1: guitar-ish strum bursts — LP-filtered noise, 3 ms attack / 180 ms decay, every
    //         350 ms, peak levels cycling {-8,-12,-10,-16} dBFS (hot picking dynamics).
    // kind 2: steady noise whose level STEPS per segment (for stale-wake schedules) — level
    //         comes from levelAt(t) supplied by the caller via seg[] below.
    int kind = 0;
    float rmsDb = -18.0f;
    struct Seg { double untilSec; float rmsDb; };
    std::vector<Seg> seg;                 // kind 2: piecewise levels
    float lpState = 0.0f;

    float sample (long long n)
    {
        const double t = (double) n / kSR;
        if (kind == 0)
            return lcg (n) * juce::Decibels::decibelsToGain (rmsDb) * 1.732f; // uniform: rms = a/sqrt(3)
        if (kind == 2)
        {
            float db = seg.empty() ? rmsDb : seg.back().rmsDb;
            for (const auto& s : seg) if (t < s.untilSec) { db = s.rmsDb; break; }
            return lcg (n) * juce::Decibels::decibelsToGain (db) * 1.732f;
        }
        // kind 1: bursts
        const double period = 0.35;
        const long long burst = (long long) (t / period);
        const double tin = t - (double) burst * period;
        static constexpr float peaks[4] = { -8.0f, -12.0f, -10.0f, -16.0f };
        const float peakDb = peaks[burst & 3];
        const double env = tin < 0.003 ? tin / 0.003 : std::exp (-(tin - 0.003) / 0.18);
        // ~2 kHz 1-pole LP for a rounder, less hissy spectrum
        const float a = 0.23f;
        lpState += a * (lcg (n) - lpState);
        return lpState * juce::Decibels::decibelsToGain (peakDb) * (float) env * 3.0f;
    }
};

//------------------------------------------------------------------ engine helpers
struct Rig
{
    CabEngine e;
    Params p;

    Rig (const juce::MemoryBlock& model, const juce::AudioBuffer<float>* ir, bool leveler)
    {
        p.autoLevel = leveler;
        p.ampOn = true;
        p.aLoaded = ir != nullptr;
        p.slot[0].dryWet01 = 1.0f;
        p.powerAmpMode = PowerAmpMode::capture;
        p.tube.driveDb = 18.0f;
        p.tube.sag = 0.5f; p.tube.presence = 0.5f; p.tube.depth = 0.5f;
        p.tube.load = 0.3f; p.tube.iron = 0.5f; p.tube.bias = 0.7f;

        e.prepare (kSR, kMaxBlock, 2, p);
        if (! e.loadAmpModelBytes (model.getData(), model.getSize()))
            { std::fprintf (stderr, "FATAL: model load failed\n"); std::exit (2); }
        if (ir != nullptr)
        {
            const float* ptrs[2] = { ir->getReadPointer (0),
                                     ir->getReadPointer (ir->getNumChannels() > 1 ? 1 : 0) };
            e.setSlotOriginalIR (0, ptrs, ir->getNumChannels() >= 2 ? 2 : 1, ir->getNumSamples(), kSR);
            e.slotApplyTrim (0, false, 1.0f, false);
            e.seedAutoLevel();   // the real adapter flow: seed the makeup from the IR energy at load
            // pump until the off-thread convolver build is live
            juce::AudioBuffer<float> warm (2, kMaxBlock);
            long long w = 0;
            for (int i = 0; i < 60; ++i)
            {
                for (int n = 0; n < kMaxBlock; ++n) { const float x = 0.05f * lcg (w++); warm.setSample (0, n, x); warm.setSample (1, n, x); }
                e.process (warm.getArrayOfWritePointers(), 2, kMaxBlock, p, false);
                juce::Thread::sleep (8);
            }
        }
    }

    // Drive `blocks` blocks; append per-block sum-square + makeup.
    void drive (Stim& st, long long& idx, int blocks, bool tube,
                std::vector<double>* ss, std::vector<float>* mk)
    {
        p.powerAmpMode = tube ? PowerAmpMode::tube : PowerAmpMode::capture;
        juce::AudioBuffer<float> buf (2, kBlk);
        for (int b = 0; b < blocks; ++b)
        {
            for (int n = 0; n < kBlk; ++n) { const float x = st.sample (idx++); buf.setSample (0, n, x); buf.setSample (1, n, x); }
            e.process (buf.getArrayOfWritePointers(), 2, kBlk, p, false);
            if (ss != nullptr)
            {
                double a = 0.0;
                for (int n = 0; n < kBlk; ++n) { const double v = buf.getSample (0, n); a += v * v; }
                ss->push_back (a);
            }
            if (mk != nullptr) mk->push_back (e.autoLevelGain());
        }
    }
};

// Collapse per-block sum-squares into a dB envelope at kHopBlocks resolution, rel. `refDb`.
std::vector<float> envDb (const std::vector<double>& ss, double refDb)
{
    std::vector<float> out;
    for (size_t i = 0; i + kHopBlocks <= ss.size(); i += kHopBlocks)
    {
        double a = 0.0;
        for (int k = 0; k < kHopBlocks; ++k) a += ss[i + (size_t) k];
        const double rms = std::sqrt (a / (kHopBlocks * kBlk));
        out.push_back ((float) (20.0 * std::log10 (std::max (1.0e-9, rms)) - refDb));
    }
    return out;
}

} // namespace

//==============================================================================
int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: %s <model.nam> <cab.wav>\n", argv[0]); return 1; }

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    juce::MemoryBlock model;
    if (! cwd.getChildFile (juce::String (argv[1])).loadFileAsData (model) || model.getSize() == 0)
        { std::fprintf (stderr, "no model (size=%d)\n", (int) model.getSize()); return 1; }
    std::fprintf (stderr, "model bytes: %d\n", (int) model.getSize());

    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (cwd.getChildFile (juce::String (argv[2]))));
    if (rd == nullptr) { std::fprintf (stderr, "no cab wav\n"); return 1; }
    juce::AudioBuffer<float> ir ((int) rd->numChannels, (int) rd->lengthInSamples);
    rd->read (&ir, 0, (int) rd->lengthInSamples, 0, true, true);

    //---------------------------------------------------------------- gainmap
    std::printf ("#SECTION gainmap\nlevel_dbfs\tg_cap_db\tg_tube_db\tstep_db\n");
    for (float lvl : { -30.0f, -24.0f, -18.0f, -12.0f, -9.0f, -6.0f })
    {
        float g[2] = { 0.0f, 0.0f };
        for (int mode = 0; mode < 2; ++mode)
        {
            Rig r (model, nullptr, /*leveler*/ false);     // no cab: output == post-amp dry
            Stim st; st.kind = 0; st.rmsDb = lvl;
            long long idx = 1;
            std::vector<double> ss;
            r.drive (st, idx, 700, mode == 1, &ss, nullptr);           // ~930 ms
            const int skip = (int) (0.3 * kSR / kBlk);                 // 300 ms settle
            double num = 0.0; long long cnt = 0;
            for (size_t b = (size_t) skip; b < ss.size(); ++b) { num += ss[b]; cnt += kBlk; }
            const double outDb = 20.0 * std::log10 (std::max (1.0e-9, std::sqrt (num / (double) cnt)));
            g[mode] = (float) (outDb - (double) lvl);
        }
        std::printf ("%.0f\t%.2f\t%.2f\t%.2f\n", lvl, g[0], g[1], g[0] - g[1]);
    }

    //---------------------------------------------------------------- reference-gain probe (shared stimulus)
    // The SHARED cab::levelprobe stimulus through each route (post-amp only: no cab, leveler off).
    // g_tube printed here IS the kTubeRefGainDb constant baked into PowerAmpRouter.cpp, and g_cap
    // must match what AmpStage measures at model load (same stimulus) — the cross-check.
    {
        const int total = (int) (1.2 * kSR);
        std::vector<float> stim ((size_t) total);
        cab::levelprobe::fill (stim.data(), total, kSR);
        std::printf ("#SECTION refprobe\nmode\tgain_db\n");
        for (int mode = 0; mode < 2; ++mode)
        {
            Rig r (model, nullptr, /*leveler*/ false);
            r.p.powerAmpMode = mode ? PowerAmpMode::tube : PowerAmpMode::capture;
            juce::AudioBuffer<float> buf (2, kBlk);
            const int skip = (int) (0.5 * kSR);
            double sumIn = 0.0, sumOut = 0.0;
            for (int off = 0; off + kBlk <= total; off += kBlk)
            {
                for (int n = 0; n < kBlk; ++n) { const float x = stim[(size_t) (off + n)]; buf.setSample (0, n, x); buf.setSample (1, n, x); }
                if (off >= skip)
                    for (int n = 0; n < kBlk; ++n) { const double x = buf.getSample (0, n); sumIn += x * x; }
                r.e.process (buf.getArrayOfWritePointers(), 2, kBlk, r.p, false);
                if (off >= skip)
                    for (int n = 0; n < kBlk; ++n) { const double y = buf.getSample (0, n); sumOut += y * y; }
            }
            std::printf ("%s\t%.2f\n", mode ? "tube" : "capture",
                         20.0 * std::log10 (std::max (1.0e-9, std::sqrt (sumOut / std::max (1.0, sumIn)))));
        }
    }

    //---------------------------------------------------------------- cab gain + settled makeup per mode
    // For each mode: RMS(cab on, leveler off) − RMS(no cab) = the cab's RMS gain for THAT mode's
    // spectrum; plus the leveler's settled makeup with the cab. If the two modes' cab gains differ,
    // the settled makeups must differ by the same amount (mk = −cabGain) — a consistency check that
    // pins where any extra A/B step (beyond the post-amp step) comes from.
    std::printf ("#SECTION cabgain\nmode\tpostamp_db\tpostcab_db\tcabgain_db\tmk_settled_db\n");
    for (int mode = 0; mode < 2; ++mode)
    {
        double lvl[2] = { 0.0, 0.0 };   // [0] = no cab, [1] = cab
        float mkSettled = 1.0f;
        for (int cab = 0; cab < 2; ++cab)
        {
            Rig r (model, cab ? &ir : nullptr, /*leveler*/ false);
            Stim st; st.kind = 0; st.rmsDb = -18.0f;
            long long idx = 1;
            std::vector<double> ss;
            r.drive (st, idx, 1100, mode == 1, &ss, nullptr);          // ~1.47 s
            const int skip = (int) (0.6 * kSR / kBlk);
            double num = 0.0; long long cnt = 0;
            for (size_t b = (size_t) skip; b < ss.size(); ++b) { num += ss[b]; cnt += kBlk; }
            lvl[cab] = 20.0 * std::log10 (std::max (1.0e-9, std::sqrt (num / (double) cnt)));
        }
        {   // settled makeup with the cab + leveler on
            Rig r (model, &ir, /*leveler*/ true);
            Stim st; st.kind = 0; st.rmsDb = -18.0f;
            long long idx = 1;
            std::vector<float> mk;
            r.drive (st, idx, 2200, mode == 1, nullptr, &mk);          // ~2.9 s converge
            mkSettled = mk.back();
        }
        std::printf ("%s\t%.2f\t%.2f\t%.2f\t%.2f\n", mode ? "tube" : "capture",
                     lvl[0], lvl[1], lvl[1] - lvl[0],
                     juce::Decibels::gainToDecibels (mkSettled, -120.0f));
    }

    //---------------------------------------------------------------- switch traces (null form)
    // Engine A: 1.5 s converge in FROM → switch → 1.5 s in TO. Engine REF: the SAME input, but in
    // the TO mode the whole time. delta = envA − envREF: before the switch it is the honest A/B
    // step; after the fade it is EXACTLY the transition artifact (leveler memory + any stage state),
    // valid even on non-stationary bursts. Both engines share leveler on/off, cab, stimulus.
    for (int lev = 0; lev <= 1; ++lev)
    for (int stimKind = 0; stimKind <= 1; ++stimKind)
    for (int toTube = 0; toTube <= 1; ++toTube)
    {
        Rig A (model, &ir, lev == 1), R (model, &ir, lev == 1);
        Stim sa; sa.kind = stimKind; sa.rmsDb = -18.0f;
        Stim sr = sa;
        long long ia = 1, ir2 = 1;
        std::vector<double> ssA, ssR; std::vector<float> mkA, mkR;
        const int pre = (int) (1.5 * kSR / kBlk), post = (int) (1.5 * kSR / kBlk);
        A.drive (sa, ia, pre,  toTube == 0, &ssA, &mkA);   // FROM mode
        A.drive (sa, ia, post, toTube == 1, &ssA, &mkA);   // TO mode
        R.drive (sr, ir2, pre,  toTube == 1, &ssR, &mkR);  // TO mode all along
        R.drive (sr, ir2, post, toTube == 1, &ssR, &mkR);
        const auto envA = envDb (ssA, 0.0), envR = envDb (ssR, 0.0);
        std::printf ("#SECTION trace leveler=%s stim=%s dir=%s\n t_ms\tdelta_db\tsm_db\tmkA_db\tmkR_db\n",
                     lev ? "on" : "off", stimKind ? "bursts" : "noise", toTube ? "c2t" : "t2c");
        const int switchHop = pre / kHopBlocks;
        float sm = 0.0f; bool smInit = false;
        const float aSm = 1.0f - std::exp (-(float) (kHopBlocks * kBlk) / (0.05f * (float) kSR)); // 50 ms EMA
        for (size_t h = 0; h < envA.size() && h < envR.size(); ++h)
        {
            const double tMs = ((double) h - (double) switchHop) * kHopBlocks * kBlk / kSR * 1000.0;
            const float d = envA[h] - envR[h];
            if (! smInit) { sm = d; smInit = true; } else sm += aSm * (d - sm);
            if (tMs < -250.0 || tMs > 1450.0) continue;
            const size_t bi = std::min (mkA.size() - 1, (size_t) (h * kHopBlocks));
            std::printf ("%.1f\t%.2f\t%.2f\t%.2f\t%.2f\n", tMs, d, sm,
                         juce::Decibels::gainToDecibels (mkA[bi], -120.0f),
                         juce::Decibels::gainToDecibels (mkR[bi], -120.0f));
        }
    }

    //---------------------------------------------------------------- stale-wake nulls
    // Schedule: LOUD (-9) 2 s → QUIET (-24) 2.5 s → QUIET (-24) 1.5 s (post-switch window).
    // Engine A: away-mode during the middle segment (stage under test frozen), back for the last.
    // Engine B: stays in the mode the whole time. |A-B| after the last switch = stale-state error.
    for (int wakeTube = 0; wakeTube <= 1; ++wakeTube)     // 1: tube freezes; 0: capture freezes
    {
        auto mkStim = [] { Stim s; s.kind = 2; s.seg = { { 2.0, -9.0f }, { 4.5, -24.0f }, { 9.0, -24.0f } }; return s; };
        const int segA = (int) (2.0 * kSR / kBlk), segB = (int) (2.5 * kSR / kBlk), segC = (int) (1.5 * kSR / kBlk);

        Rig A (model, nullptr, false), B (model, nullptr, false);
        Stim sa = mkStim(), sb = mkStim();
        long long ia = 1, ib = 1;
        std::vector<double> ssA, ssB;
        const bool m = wakeTube == 1;
        A.drive (sa, ia, segA, m,   &ssA, nullptr);   // in-mode, loud
        A.drive (sa, ia, segB, ! m, &ssA, nullptr);   // away (stage frozen w/ loud-state)
        A.drive (sa, ia, segC, m,   &ssA, nullptr);   // back, quiet
        B.drive (sb, ib, segA, m, &ssB, nullptr);
        B.drive (sb, ib, segB, m, &ssB, nullptr);
        B.drive (sb, ib, segC, m, &ssB, nullptr);
        const auto eA = envDb (ssA, 0.0), eB = envDb (ssB, 0.0);
        std::printf ("#SECTION stalewake wake=%s\n t_ms\ta_db\tb_db\tdelta_db\n", m ? "tube" : "capture");
        const int switchHop = (segA + segB) / kHopBlocks;
        for (size_t h = (size_t) std::max (0, switchHop - 20); h < eA.size() && h < eB.size(); ++h)
        {
            const double tMs = ((double) h - (double) switchHop) * kHopBlocks * kBlk / kSR * 1000.0;
            std::printf ("%.1f\t%.2f\t%.2f\t%.2f\n", tMs, eA[h], eB[h], eA[h] - eB[h]);
        }
    }

    return 0;
}
