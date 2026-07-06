// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "TubePowerAmp.h"
#include "TubeKernel.h"
#include "SagEnvelope.h"
#include "../core/Params.h"

#include <felitronics/oversampling/PolyphaseOversampler.h>
#include <felitronics/eq/Svf.h>

#include <algorithm>
#include <cmath>
#include <vector>

//==============================================================================
// Block 2 core + Block 3 "feel". An oversampled tube waveshaper, JUCE-free (std + felitronics
// headers only, embedded-safe). All state heap-allocated in prepare(); process() never allocates.
//
// Chain (block 2): Drive pre-gain → upsample ×N → TubeStage (PP/SE) → OS-domain DC-block → down →
//   drive-comp → Output.
// Block 3 "feel" folds in WITHOUT new latency or per-OS cost:
//   • SAG — a feedforward dual-TC envelope on rectified demand (|G·x|, mono-linked shared supply)
//     drives a shrinking B+ rail s=1−droop: `1/s` folds into the Drive input ramp, `s` folds into a
//     post-downsample gain → y = s·shaper(u/s): earlier breakup + lower ceiling = tube squish.
//     (An operating-point bias shift under sag is deferred — a per-block shift breaks block-size
//     determinism, so it needs a per-sample-bias TubeStage overload; TubeVoicing::sagBiasDepth reserves it.)
//   • PRESENCE/DEPTH — two min-phase felitronics::eq::Svf shelves on the output node whose effective
//     dB OPENS with sag/drive (NFB "feedback releases when pushed"), not a static EQ.
// FEEL GATE: with sag=presence=depth=0 every block-3 path is skipped ⇒ byte-identical to block 2.
//==============================================================================
namespace cab::poweramp
{

namespace
{
    constexpr int    kTpp       = 32;       // FIR taps/phase → 31-sample baseband round-trip latency
    constexpr int    kMaxCh     = 2;        // stereo max (engine contract; AmpStage preps 2 likewise)
    constexpr float  kDcBlockHz = 10.0f;    // OS-domain DC blocker corner
    constexpr float  kSagMinRail = 0.2f;    // clamp s = 1−droop away from 0 (never invert / div-blow-up)
    constexpr double kTwoPi     = 6.283185307179586476925287;

    inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }

    // dry/wet blend b so that (1 + b·(Gmax−1)) == Gnom — i.e. the quiet (un-sagged) shelf sits at the
    // nominal knob gain, and sag droop later drives b→1 (the fully-open Gmax). A linear-amplitude blend
    // of a min-phase shelf stays a real, stable, per-sample filter — no per-block coefficient jump.
    inline float blendForNominal (float nomDb, float maxDb) noexcept
    {
        const float gn = dbToGain (nomDb), gm = dbToGain (maxDb);
        return (gm > 1.0001f) ? std::clamp ((gn - 1.0f) / (gm - 1.0f), 0.0f, 1.0f) : 1.0f;
    }
}

struct TubePowerAmp::Impl
{
    using Svf = felitronics::eq::Svf;

    double sampleRate = 0.0;
    int    maxBlock   = 0;
    int    os         = 4;                          // oversampling factor (4 shipping; test may set 32)

    felitronics::oversampling::PolyphaseOversampler ovs;
    std::vector<float> osBuf[kMaxCh];               // maxBlock*os per channel (caller-owned OS scratch)
    float*             osPtr[kMaxCh] { nullptr, nullptr };

    TubeStage stage;                                // the pure PP/SE transfer (TubeKernel.h)

    double dcR = 0.0;                                // OS-domain DC-block one-pole coeff
    double dcx1[kMaxCh] { 0.0, 0.0 }, dcy1[kMaxCh] { 0.0, 0.0 };

    // --- block 3 "feel" state (all allocated/prepared in prepare()) ---
    SagEnvelope        sag;                          // pure dual-TC sag model (SagEnvelope.h)
    Svf                svfPresence, svfDepth, svfMid; // min-phase HF/LF shelves + a static per-voicing MID bell (one instance = all channels)
    Svf                svfLoadRes, svfLoadRise;       // block 4 virtual load: LF impedance-resonance Bell + HF inductive-rise shelf (input pre-EQ)
    std::vector<float> sScratch;                     // per-sample rail s = 1−droop (maxBlock)
    int                tubeIdx = 0;                  // voicing index for the per-tube sag/NFB constants

    // Targets from the latest setParams(); smoothed per block in process().
    float gTarget = 1.0f, outTarget = 1.0f, topoTarget = 0.0f;   // topo: 0 = PP, 1 = SE
    float kTarget = 2.0f, vbTarget = 0.30f, bSeTarget = 0.18f, leakTarget = 0.0f;
    float sagTarget = 0.0f, presTarget = 0.0f, depthTarget = 0.0f, loadTarget = 0.0f, ironTarget = 0.0f, biasTarget = 0.0f;
    float autoComp = 1.0f;

    // Smoothed running coefficients.
    float gCur = 1.0f, outCur = 1.0f, topoCur = 0.0f;
    float kCur = 2.0f, vbCur = 0.30f, bSeCur = 0.18f, leakCur = 0.0f;
    float sagCur = 0.0f, presCur = 0.0f, depthCur = 0.0f, loadCur = 0.0f, ironCur = 0.0f, biasCur = 0.0f;
    float otLp[kMaxCh] = {}, otHf[kMaxCh] = {};   // block-4 output-transformer OS-domain 1-pole states (LF split + HF rolloff)
    float comp = 1.0f;
    float gApplied = 1.0f, postApplied = 1.0f;       // per-sample ramp anchors
    bool  primed = false;

    void prepare (double sr, int mb, int osFactor)
    {
        sampleRate = sr;
        maxBlock   = std::max (1, mb);
        os         = std::clamp (osFactor, 2, 32);
        ovs.prepare (os, kMaxCh, kTpp);
        for (int ch = 0; ch < kMaxCh; ++ch)
        {
            osBuf[ch].assign ((std::size_t) (maxBlock * os), 0.0f);
            osPtr[ch] = osBuf[ch].data();
        }
        const double fsOs = sr * (double) os;
        dcR = fsOs > 0.0 ? std::exp (-kTwoPi * (double) kDcBlockHz / fsOs) : 0.0;
        sag.prepare (sr);
        svfPresence.prepare (sr, kMaxCh);
        svfDepth.prepare (sr, kMaxCh);
        svfMid.prepare (sr, kMaxCh);
        svfLoadRes.prepare (sr, kMaxCh);
        svfLoadRise.prepare (sr, kMaxCh);
        sScratch.assign ((std::size_t) maxBlock, 1.0f);
        reset();
        primed = false;
    }

    void reset()
    {
        ovs.reset();
        for (int ch = 0; ch < kMaxCh; ++ch) { dcx1[ch] = dcy1[ch] = 0.0; }
        sag.reset();
        svfPresence.reset();
        svfDepth.reset();
        svfMid.reset();
        svfLoadRes.reset();
        svfLoadRise.reset();
        for (int c = 0; c < kMaxCh; ++c) { otLp[c] = 0.0f; otHf[c] = 0.0f; }
    }

    void setParams (const cab::TubeParams& p)
    {
        tubeIdx = std::clamp (p.tubeType, 0, 3);
        const TubeVoicing& v = kTubeVoicings[(std::size_t) tubeIdx];
        // Sanitize the dB params at the single entry point: a non-finite driveDb/outputDb (a bad preset,
        // or any non-APVTS caller — TubeParams is a public POD) would make gCur/postTarget NaN, and
        // std::clamp(NaN) passes NaN straight through into the OS/DC state → permanent poison.
        const float driveDb  = std::isfinite (p.driveDb)  ? p.driveDb  : 0.0f;
        const float outputDb = std::isfinite (p.outputDb) ? p.outputDb : 0.0f;
        autoComp   = std::isfinite (p.autoComp) ? std::clamp (p.autoComp, 0.0f, 1.0f) : 1.0f;
        gTarget    = dbToGain (driveDb) * v.driveScale;
        outTarget  = dbToGain (outputDb);
        topoTarget = p.singleEnded ? 1.0f : 0.0f;
        kTarget    = v.k; vbTarget = v.vbPP; bSeTarget = v.bSE; leakTarget = v.evenLeak;
        // block-3 feel amounts — same isfinite+clamp discipline (new IIR/envelope state must not be poisoned).
        sagTarget   = std::isfinite (p.sag)      ? std::clamp (p.sag,      0.0f, 1.0f) : 0.0f;
        presTarget  = std::isfinite (p.presence) ? std::clamp (p.presence, 0.0f, 1.0f) : 0.0f;
        depthTarget = std::isfinite (p.depth)    ? std::clamp (p.depth,    0.0f, 1.0f) : 0.0f;
        loadTarget  = std::isfinite (p.load)     ? std::clamp (p.load,     0.0f, 1.0f) : 0.0f;
        ironTarget  = std::isfinite (p.iron)     ? std::clamp (p.iron,     0.0f, 1.0f) : 0.0f;
        biasTarget  = std::isfinite (p.bias)     ? std::clamp (p.bias,     0.0f, 1.0f) : 0.0f;
    }

    void process (float* const* io, int numChannels, int numSamples)
    {
        const int nCh = std::min (numChannels, kMaxCh);
        if (nCh <= 0 || numSamples <= 0 || maxBlock <= 0)   // maxBlock<=0 ⇒ process() called before prepare():
            return;                                          // clean no-op (also kills the off+=0 infinite loop)

        // Chunk to maxBlock so a caller passing numSamples > maxBlock is FULLY processed instead of
        // silently leaving the tail dry. State carries across chunks via the members → seamless.
        float* sub[kMaxCh];
        for (int off = 0; off < numSamples; off += maxBlock)
        {
            const int n = std::min (numSamples - off, maxBlock);
            for (int ch = 0; ch < nCh; ++ch) sub[ch] = io[ch] + off;
            processChunk (sub, nCh, n);
        }
    }

    void processChunk (float* const* io, int nCh, int n)
    {
        if (n <= 0) return;   // guards the 1.0f/n ramps below: a 0-length chunk is a div-by-zero

        // --- per-block coefficient smoothing (~25 ms one-pole at block rate; first block snaps) ---
        const float a = (! primed) ? 1.0f
                      : (sampleRate > 0.0 ? (float) (1.0 - std::exp (- (double) n / (0.025 * sampleRate))) : 1.0f);
        gCur    += a * (gTarget    - gCur);
        outCur  += a * (outTarget  - outCur);
        topoCur += a * (topoTarget - topoCur);
        kCur    += a * (kTarget    - kCur);
        vbCur   += a * (vbTarget   - vbCur);
        bSeCur  += a * (bSeTarget  - bSeCur);
        leakCur += a * (leakTarget - leakCur);
        sagCur   += a * (sagTarget   - sagCur);
        loadCur  += a * (loadTarget  - loadCur);
        ironCur  += a * (ironTarget  - ironCur);
        biasCur  += a * (biasTarget  - biasCur);
        presCur  += a * (presTarget  - presCur);
        depthCur += a * (depthTarget - depthCur);

        const TubeVoicing& v = kTubeVoicings[(std::size_t) tubeIdx];

        // --- block-3 "feel" per-block setup + the FEEL GATE (all-off ⇒ block-2 path, byte-identical) ---
        const bool sagOn = sagCur > 1.0e-6f;
        sag.setParams (sagCur, v.sagFastMs, v.sagRecoveryMs, v.sagMaxDroop);

        // PRESENCE/DEPTH: the Svf shelves hold their FULLY-OPEN gain (set per block from the settled
        // knob → block-size-independent); the dynamic "NFB opens when pushed" is the PER-SAMPLE dry/wet
        // blend in the post-downsample loop, driven by the per-sample sag droop. Opening per-sample (not
        // per-block off a block-boundary droop sample) is exactly what keeps the output identical across
        // host buffer sizes. presBase/depthBase place the quiet state at the nominal knob gain.
        const float presNomDb  = presCur  * v.presenceMaxDb;
        const float depthNomDb = depthCur * v.depthMaxDb;
        const float presMaxDb  = presNomDb  * (1.0f + v.nfbOpen);
        const float depthMaxDb = depthNomDb * (1.0f + v.nfbOpen);
        const bool  presOn  = presCur  > 1.0e-4f;
        const bool  depthOn = depthCur > 1.0e-4f;
        const float presBase  = blendForNominal (presNomDb,  presMaxDb);
        const float depthBase = blendForNominal (depthNomDb, depthMaxDb);
        if (presOn)  svfPresence.setParams (felitronics::eq::FilterType::HighShelf, (double) v.presenceHz, 0.70710678, (double) presMaxDb);
        if (depthOn) svfDepth.setParams   (felitronics::eq::FilterType::LowShelf,  (double) v.depthHz,    0.70710678, (double) depthMaxDb);

        // static per-voicing MID bell — the amp fingerprint (always on when the tube runs; not knob-gated)
        const bool midOn = std::fabs (v.midDb) > 1.0e-3f;
        if (midOn) svfMid.setParams (felitronics::eq::FilterType::Bell, (double) v.midHz, (double) v.midQ, (double) v.midDb);

        stage.configure (kCur, bSeCur, vbCur, leakCur, topoCur);

        // drive-compensation: numeric small-signal slope of the FULL composite (incl. pre-gain gCur)
        const float sl = stage.slopeAtZero (gCur);
        comp = std::pow (std::max (1.0e-6f, std::fabs (sl)), -autoComp);

        const float postTarget = comp * outCur;
        if (! primed) { gApplied = gCur; postApplied = postTarget; primed = true; }

        // --- VIRTUAL LOAD (block 4): reactive-speaker impedance pre-EQ on the RAW input, so the Drive
        // pre-gain + sag DETECTOR below both see the load-coloured signal (frequency-dependent break-up,
        // and sag pulled correctly by the resonance-boosted lows). Min-phase (felitronics Svf), 0 latency;
        // both gains 0 dB ⇒ skipped ⇒ byte-identical to block 3. Static per-voicing (the amp's own load). ---
        const float loadResDb  = loadCur * v.loadResDb;    // the Load knob scales the per-voicing impedance shape
        const float loadRiseDb = loadCur * v.loadRiseDb;
        const bool  loadOn = loadCur > 1.0e-4f && (std::fabs (v.loadResDb) > 1.0e-3f || std::fabs (v.loadRiseDb) > 1.0e-3f);
        if (loadOn)
        {
            svfLoadRes .setParams (felitronics::eq::FilterType::Bell,      (double) v.loadResHz,  (double) v.loadResQ, (double) loadResDb);
            svfLoadRise.setParams (felitronics::eq::FilterType::HighShelf, (double) v.loadRiseHz, 0.70710678,          (double) loadRiseDb);
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < nCh; ++ch)
                    io[ch][i] = svfLoadRise.processSample (ch, svfLoadRes.processSample (ch, io[ch][i]));
        }

        // --- input Drive pre-gain ramp (+ SAG rail-shrink 1/s when active) ---
        {
            const float g0 = gApplied, g1 = gCur, inv = 1.0f / (float) n;
            for (int i = 0; i < n; ++i)
            {
                const float g = g0 + (g1 - g0) * ((float) (i + 1) * inv);
                if (sagOn)
                {
                    // mono-linked demand (shared supply) → droop → rail s; push u/s into the shaper.
                    float w[kMaxCh], demand = 0.0f;
                    for (int ch = 0; ch < nCh; ++ch)
                    {
                        const float vch = io[ch][i];
                        w[ch] = (std::isfinite (vch) ? vch : 0.0f) * g;
                        // cap the demand: an extreme FINITE input (|vch|·g > FLT_MAX → +Inf) must not
                        // push the sag envelope to Inf→NaN (which flushDenormals wouldn't clear → stuck rail).
                        demand = std::max (demand, std::min (std::fabs (w[ch]), 1.0e6f));
                    }
                    const float s = std::max (kSagMinRail, 1.0f - sag.process (demand));
                    sScratch[(std::size_t) i] = s;
                    const float invS = 1.0f / s;
                    for (int ch = 0; ch < nCh; ++ch) io[ch][i] = std::clamp (w[ch] * invS, -1.0e6f, 1.0e6f);
                }
                else
                {
                    for (int ch = 0; ch < nCh; ++ch)
                    {
                        const float vch = io[ch][i];                              // sanitize AT THE GATE: a NaN/Inf would
                        const float s2  = (std::isfinite (vch) ? vch : 0.0f) * g;  // poison the OS/DC state; clamp also
                        io[ch][i] = std::clamp (s2, -1.0e6f, 1.0e6f);              // catches huge·gain → +Inf overflow
                    }
                }
            }
            gApplied = g1;
        }

        // --- OUTPUT TRANSFORMER (block 4 stage 2) + dynamic BIAS (stage 3) — both act in the OS domain ---
        const double fsOs   = sampleRate * (double) os;
        const bool   ironOn = ironCur > 1.0e-4f && v.otHfHz > 0.0f;
        const float  gLf    = ironOn ? (float) (1.0 - std::exp (-kTwoPi * (double) v.otLfHz / fsOs)) : 0.0f;   // LF split 1-pole
        const float  gHf    = ironOn ? (float) (1.0 - std::exp (-kTwoPi * (double) v.otHfHz / fsOs)) : 0.0f;   // HF leakage 1-pole
        const float  kSat   = std::max (0.1f, v.otSatK);
        const float  otWet  = ironCur;                          // Iron knob scales the LF-sat blend + the HF-rolloff blend
        // dynamic bias: the PP operating point drifts toward class-B under sag (crossover bloom); needs Sag + Bias.
        const bool   biasOn = biasCur > 1.0e-4f && sagOn && v.sagBiasDepth > 0.0f;
        const float  invMaxDroopB = v.sagMaxDroop > 0.0f ? 1.0f / v.sagMaxDroop : 0.0f;
        const float  biasScale    = biasCur * v.sagBiasDepth * vbCur;   // vbEff = vbCur − biasScale·droopN

        // --- upsample → PP/SE waveshape (+ per-sample bias) + DC-block + output transformer (OS domain) → downsample ---
        ovs.upsample (io, nCh, n, osPtr);
        for (int ch = 0; ch < nCh; ++ch)
        {
            float* b = osPtr[ch];
            double x1 = dcx1[ch], y1 = dcy1[ch];
            float  lp = otLp[ch], hf = otHf[ch];
            const int m = n * os;
            for (int j = 0; j < m; ++j)
            {
                float w;
                if (biasOn)   // per-sample class-B drift, ZOH from the host-rate droop (block-size-deterministic)
                {
                    const float droopN = std::clamp ((1.0f - sScratch[(std::size_t) (j / os)]) * invMaxDroopB, 0.0f, 1.0f);
                    w = stage.at (b[j], -biasScale * droopN);
                }
                else
                    w = stage.at (b[j]);
                const double dc = (double) w - x1 + dcR * y1;       // OS-domain DC-block
                x1 = (double) w; y1 = dc;
                float s = (float) dc;
                if (ironOn)                                          // OUTPUT TRANSFORMER
                {
                    lp += gLf * (s - lp);                            //   low band (transformer flux)
                    const float sat = std::tanh (kSat * lp) / kSat;  //   soft-clip the lows (unity small-signal)
                    s += otWet * (sat - lp);                         //   grind / compress the low notes
                    hf += gHf * (s - hf);                            //   HF leakage low-pass
                    s += otWet * (hf - s);                           //   roll off the fizzy top
                }
                b[j] = s;
            }
            // flush non-finite / denormal state so a transient can't poison or CPU-spike the stream
            dcx1[ch] = (std::isfinite (x1) && std::fabs (x1) > 1e-30) ? x1 : 0.0;
            dcy1[ch] = (std::isfinite (y1) && std::fabs (y1) > 1e-30) ? y1 : 0.0;
            otLp[ch] = (std::isfinite (lp) && std::fabs (lp) > 1e-30f) ? lp : 0.0f;
            otHf[ch] = (std::isfinite (hf) && std::fabs (hf) > 1e-30f) ? hf : 0.0f;
        }
        ovs.downsample (osPtr, nCh, n, io);

        // static per-voicing MID band (voicing tone — always on when the tube runs; not sag/knob-gated)
        if (midOn)
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < nCh; ++ch)
                    io[ch][i] = svfMid.processSample (ch, io[ch][i]);

        // --- block 3: SAG rail output (·s) + PRESENCE/DEPTH shelves (the NFB "output node") ---
        // Skipped entirely when the feel layer is off → the output equals the block-2 result exactly.
        if (sagOn || presOn || depthOn)
        {
            const float invMaxDroop = v.sagMaxDroop > 0.0f ? 1.0f / v.sagMaxDroop : 0.0f;
            for (int i = 0; i < n; ++i)
            {
                // per-sample droop → per-sample shelf "open" blend (block-size-independent). droopN ∈ [0,1]
                // maps quiet→open; the Svf state still advances every sample so its memory stays coherent.
                const float droopN     = sagOn ? std::clamp ((1.0f - sScratch[(std::size_t) i]) * invMaxDroop, 0.0f, 1.0f) : 0.0f;
                const float presBlend  = presBase  + (1.0f - presBase)  * droopN;
                const float depthBlend = depthBase + (1.0f - depthBase) * droopN;
                for (int ch = 0; ch < nCh; ++ch)
                {
                    float x = io[ch][i];
                    if (sagOn) x *= sScratch[(std::size_t) i];                                     // rail-shrink output ·s
                    if (presOn)  { const float sh = svfPresence.processSample (ch, x); x += presBlend  * (sh - x); }
                    if (depthOn) { const float sh = svfDepth.processSample   (ch, x); x += depthBlend * (sh - x); }
                    io[ch][i] = x;
                }
            }
        }

        // --- drive-comp + Output gain (per-sample ramp) ---
        {
            const float p0 = postApplied, p1 = postTarget, inv = 1.0f / (float) n;
            for (int i = 0; i < n; ++i)
            {
                const float g = p0 + (p1 - p0) * ((float) (i + 1) * inv);
                for (int ch = 0; ch < nCh; ++ch) io[ch][i] = std::clamp (io[ch][i] * g, -1.0e6f, 1.0e6f);
            }
            postApplied = p1;
        }

        sag.flushDenormals();
        svfPresence.flushDenormals();
        svfDepth.flushDenormals();
        svfMid.flushDenormals();
        svfLoadRes.flushDenormals();
        svfLoadRise.flushDenormals();
    }

    int latencySamples() const noexcept { return ovs.latencySamples(); }
};

TubePowerAmp::TubePowerAmp() : impl (std::make_unique<Impl>()) {}
TubePowerAmp::~TubePowerAmp() = default;

void TubePowerAmp::prepare (double sampleRate, int maxBlock, int oversampleFactor) { impl->prepare (sampleRate, maxBlock, oversampleFactor); }
void TubePowerAmp::reset() { impl->reset(); }
void TubePowerAmp::setParams (const cab::TubeParams& p) noexcept { impl->setParams (p); }
void TubePowerAmp::process (float* const* io, int numChannels, int numSamples) noexcept { impl->process (io, numChannels, numSamples); }
int  TubePowerAmp::latencySamples() const noexcept { return impl->latencySamples(); }

} // namespace cab::poweramp
