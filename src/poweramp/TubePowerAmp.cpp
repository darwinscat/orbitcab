// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "TubePowerAmp.h"
#include "TubeKernel.h"
#include "../core/Params.h"

#include <felitronics/oversampling/PolyphaseOversampler.h>

#include <algorithm>
#include <cmath>
#include <vector>

//==============================================================================
// Block 2 — the nonlinear core. An oversampled tube waveshaper, JUCE-free (std + felitronics
// headers only) so it stays bit-light, extractable to felitronics-core, and embeddable on an
// M7/Daisy-class target. All state is heap-allocated in prepare(); process() never allocates.
//
// Chain: Drive pre-gain → upsample ×N → TubeStage (PP/SE, see TubeKernel.h) → OS-domain DC-block
// → downsample → drive-comp → Output. Drive is an input pre-gain (keeps each tube's curve
// identity); drive-comp normalises the small-signal gain from the composite numeric slope so a
// clean setting stays clean and A/B is honest. The transfer math lives in TubeKernel.h so it can
// be unit-tested directly; the oversampling factor is a prepare() arg so a test can null 4x vs 32x.
//==============================================================================
namespace cab::poweramp
{

namespace
{
    constexpr int    kTpp       = 32;       // FIR taps/phase → 31-sample baseband round-trip latency
    constexpr int    kMaxCh     = 2;        // stereo max (engine contract; AmpStage preps 2 likewise)
    constexpr float  kDcBlockHz = 10.0f;    // OS-domain DC blocker corner
    constexpr double kTwoPi     = 6.283185307179586476925287;

    inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
}

struct TubePowerAmp::Impl
{
    double sampleRate = 0.0;
    int    maxBlock   = 0;
    int    os         = 4;                          // oversampling factor (4 shipping; test may set 32)

    felitronics::oversampling::PolyphaseOversampler ovs;
    std::vector<float> osBuf[kMaxCh];               // maxBlock*os per channel (caller-owned OS scratch)
    float*             osPtr[kMaxCh] { nullptr, nullptr };

    TubeStage stage;                                // the pure PP/SE transfer (TubeKernel.h)

    double dcR = 0.0;                                // OS-domain DC-block one-pole coeff
    double dcx1[kMaxCh] { 0.0, 0.0 }, dcy1[kMaxCh] { 0.0, 0.0 };

    // Targets from the latest setParams(); smoothed per block in process().
    float gTarget = 1.0f, outTarget = 1.0f, topoTarget = 0.0f;   // topo: 0 = PP, 1 = SE
    float kTarget = 2.0f, vbTarget = 0.30f, bSeTarget = 0.18f, leakTarget = 0.0f;
    float autoComp = 1.0f;

    // Smoothed running coefficients.
    float gCur = 1.0f, outCur = 1.0f, topoCur = 0.0f;
    float kCur = 2.0f, vbCur = 0.30f, bSeCur = 0.18f, leakCur = 0.0f;
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
        reset();
        primed = false;
    }

    void reset()
    {
        ovs.reset();
        for (int ch = 0; ch < kMaxCh; ++ch) { dcx1[ch] = dcy1[ch] = 0.0; }
    }

    void setParams (const cab::TubeParams& p)
    {
        const TubeVoicing& v = kTubeVoicings[(std::size_t) std::clamp (p.tubeType, 0, 3)];
        autoComp   = std::clamp (p.autoComp, 0.0f, 1.0f);
        gTarget    = dbToGain (p.driveDb) * v.driveScale;
        outTarget  = dbToGain (p.outputDb);
        topoTarget = p.singleEnded ? 1.0f : 0.0f;
        kTarget    = v.k; vbTarget = v.vbPP; bSeTarget = v.bSE; leakTarget = v.evenLeak;
    }

    void process (float* const* io, int numChannels, int numSamples)
    {
        const int nCh = std::min (numChannels, kMaxCh);
        if (nCh <= 0 || numSamples <= 0 || maxBlock <= 0)   // maxBlock<=0 ⇒ process() called before prepare():
            return;                                          // clean no-op (also kills the off+=0 infinite loop)

        // Chunk to maxBlock so a caller passing numSamples > maxBlock is FULLY processed instead of
        // silently leaving the tail dry. Hosts never exceed maxBlock; a standalone/extracted reuse
        // might. State carries across chunks via the members, so the result is seamless.
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
                              // (silent inf on ARM/IEEE, a trap on x86 with FP exceptions unmasked)

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

        stage.configure (kCur, bSeCur, vbCur, leakCur, topoCur);

        // drive-compensation from the composite numeric small-signal slope (incl. pre-gain gCur)
        const float sl = stage.slopeAtZero (gCur);
        comp = std::pow (std::max (1.0e-6f, std::fabs (sl)), -autoComp);

        const float postTarget = comp * outCur;
        if (! primed) { gApplied = gCur; postApplied = postTarget; primed = true; }

        // --- input Drive pre-gain (per-sample ramp from last applied → this block's smoothed value) ---
        {
            const float g0 = gApplied, g1 = gCur, inv = 1.0f / (float) n;
            for (int i = 0; i < n; ++i)
            {
                const float g = g0 + (g1 - g0) * ((float) (i + 1) * inv);
                for (int ch = 0; ch < nCh; ++ch)
                {
                    const float v = io[ch][i];                            // sanitize AT THE GATE: a NaN/Inf would
                    const float s = (std::isfinite (v) ? v : 0.0f) * g;   // poison the OS/DC state; the clamp also
                    io[ch][i] = std::clamp (s, -1.0e6f, 1.0e6f);          // catches huge·gain → +Inf overflow (X7)
                }
            }
            gApplied = g1;
        }

        // --- upsample → PP/SE waveshape + DC-block (OS domain) → downsample ---
        ovs.upsample (io, nCh, n, osPtr);
        for (int ch = 0; ch < nCh; ++ch)
        {
            float* b = osPtr[ch];
            double x1 = dcx1[ch], y1 = dcy1[ch];
            const int m = n * os;
            for (int j = 0; j < m; ++j)
            {
                const float  w  = stage.at (b[j]);
                const double dc = (double) w - x1 + dcR * y1;
                x1 = (double) w; y1 = dc;
                b[j] = (float) dc;
            }
            // flush non-finite / denormal state so a transient can't poison or CPU-spike the stream
            dcx1[ch] = (std::isfinite (x1) && std::fabs (x1) > 1e-30) ? x1 : 0.0;
            dcy1[ch] = (std::isfinite (y1) && std::fabs (y1) > 1e-30) ? y1 : 0.0;
        }
        ovs.downsample (osPtr, nCh, n, io);

        // --- drive-comp + Output gain (per-sample ramp) ---
        {
            const float p0 = postApplied, p1 = postTarget, inv = 1.0f / (float) n;
            for (int i = 0; i < n; ++i)
            {
                const float g = p0 + (p1 - p0) * ((float) (i + 1) * inv);
                for (int ch = 0; ch < nCh; ++ch) io[ch][i] *= g;
            }
            postApplied = p1;
        }
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
