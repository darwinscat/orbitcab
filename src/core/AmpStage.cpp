// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "AmpStage.h"

#include <NAM/dsp.h>        // NAM_SAMPLE (float, via NAM_SAMPLE_FLOAT) + class nam::DSP
#include <NAM/get_dsp.h>    // nam::get_dsp(path|json)

#include "StreamResampler.h"   // cab::StreamResampler (header-only, unit-tested separately)
#include "LevelProbe.h"        // cab::levelprobe — the shared reference-gain stimulus

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace cab
{

namespace
{
    // Output-normalisation base reference. Each .nam tags its measured output loudness (dB); we
    // bring it to this target so swapping tubes doesn't jump the level. A PER-MODEL trim (passed
    // into the load) is added on top. Final level is still governed by output gain + auto-level.
    constexpr float  kNormTargetDb   = -18.0f;
    inline float dbToGain (float db) { return std::pow (10.0f, db * (1.0f / 20.0f)); }

    // All factory poweramp captures are trained at 48 kHz; the rate-matcher (cab::StreamResampler,
    // see StreamResampler.h) converts the host stream to/from this so the model always runs at its
    // native rate (NAM is rate-locked).
    constexpr double kModelSampleRate = 48000.0;
}

//==============================================================================
// One loaded model = up to 2 independent instances (per channel) of the SAME capture, so a stereo
// track gets true-stereo (L/R independent), a mono track uses just instance 0. Swapped as a unit
// via a single atomic pointer.
struct ModelSet
{
    std::unique_ptr<nam::DSP> inst[2];
    float  makeup     = 1.0f;
    double expectedSR = 0.0;
    double loudnessDb = 0.0;
    bool   hasLoudness = false;
    float  refGainDb  = 0.0f;    // measured reference gain (cab::levelprobe stimulus, incl. makeup)
    bool   refGainValid = false; // false = probe failed / non-finite → consumers must not trust it
};

//==============================================================================
struct AmpStage::Impl
{
    std::atomic<ModelSet*> live { nullptr };
    std::atomic<std::uint64_t> blockCount { 0 };

    struct Retired { std::unique_ptr<ModelSet> set; std::uint64_t atBlock; };
    std::vector<Retired> garbage;

    // UI mirrors (message thread reads these).
    std::atomic<double> expectedSR  { 0.0 };
    std::atomic<double> loudnessDb  { 0.0 };
    std::atomic<bool>   hasLoudness { false };
    // Reference-gain mirrors (audio thread reads these — the tube stage's capture level-match).
    std::atomic<float>  refGainDb   { 0.0f };
    std::atomic<bool>   refGainValid { false };

    double hostSR   = 48000.0;
    int    maxBlock = 512;
    bool   resampling = false;          // hostSR != model native rate
    double modelRunSR = kModelSampleRate;   // the rate the NAM instances are Reset to / run at

    // Per-channel resampler state + model-rate scratch (used only when resampling).
    struct Ch
    {
        StreamResampler down, up;       // host->model, model->host
        std::vector<float> modelIn, modelOut;
    } ch[2];
    int maxModelFrames = 1024;

    void retire (ModelSet* old)
    {
        if (old != nullptr)
            garbage.push_back ({ std::unique_ptr<ModelSet> (old),
                                 blockCount.load (std::memory_order_acquire) });
    }

    // Decide the run rate + (re)configure per-channel resamplers/scratch. Call with audio stopped
    // (prepare) or before a model goes live. modelRunSR = the loaded model's native rate.
    void configureRates (double modelSR)
    {
        modelRunSR  = (modelSR > 0.0 ? modelSR : kModelSampleRate);
        resampling  = std::abs (hostSR - modelRunSR) > 0.5;
        maxModelFrames = (int) std::ceil (maxBlock * (modelRunSR / std::max (8000.0, hostSR))) + 16;
        for (auto& c : ch)
        {
            c.down.reset (hostSR,    modelRunSR, maxBlock * 2 + 16);
            c.up  .reset (modelRunSR, hostSR,    maxModelFrames * 2 + 16);
            c.modelIn .assign ((size_t) maxModelFrames, 0.0f);
            c.modelOut.assign ((size_t) maxModelFrames, 0.0f);
        }
    }

    // Host-rate latency the rate-matcher introduces (0 when not resampling). The cubic resampler
    // needs ~3 samples of lookahead per stage to prime; round-trip ≈ down (model→host) + up.
    int latencyHostSamples() const
    {
        if (! resampling) return 0;
        return (int) std::ceil (3.0 * hostSR / modelRunSR) + 3;
    }

    // Build, validate, configure (Reset + loudness/trim) and atomic-swap a model set in.
    bool adopt (std::unique_ptr<ModelSet> set, float extraTrimDb, bool measureRefGain)
    {
        if (set == nullptr || set->inst[0] == nullptr || set->inst[1] == nullptr)
            return false;
        // prototype: mono amp captures only
        if (set->inst[0]->NumInputChannels() != 1 || set->inst[0]->NumOutputChannels() != 1)
            return false;

        const double modelSR = set->inst[0]->GetExpectedSampleRate();

        // Rate contract: the resamplers are configured for `modelRunSR` in prepare() (audio stopped),
        // and we can't safely reconfigure them here (audio may be live → resetting them would race the
        // audio thread). So a model whose native rate differs from the configured run rate would play
        // at the WRONG rate (mispitched). Refuse it instead of misrepresenting it. A model that doesn't
        // report a rate (0) keeps the previous behaviour (run at modelRunSR). Lifting this restriction
        // would need per-model resampler state carried in the swapped ModelSet. (Factory NAMs are 48k.)
        if (modelSR > 0.0 && std::abs (modelSR - modelRunSR) > 0.5)
            return false;

        // Reset the new, not-yet-live instances to the run rate (alloc + prewarm).
        for (auto& m : set->inst)
            m->Reset (modelRunSR, maxModelFrames);

        set->expectedSR = modelSR;
        if (set->inst[0]->HasLoudness())
        {
            set->loudnessDb  = set->inst[0]->GetLoudness();
            set->hasLoudness = true;
            set->makeup = dbToGain ((float) (kNormTargetDb - set->loudnessDb) + extraTrimDb);
        }
        else
        {
            set->loudnessDb = 0.0; set->hasLoudness = false;
            set->makeup = dbToGain (extraTrimDb);
        }

        // --- reference-gain probe (message thread, on the NOT-YET-LIVE instance) ---
        // Measure this model's gain at the shared cab::levelprobe operating point (LP-shaped
        // noise @ -18 dBFS RMS, the seam's typical playing level), INCLUDING the normalization
        // makeup — i.e. exactly what the live capture path will apply. The tube stage reads it
        // (refGainDb mirror) to sit at the capture's level deterministically: no follower, no
        // pump, measured once per load. The probed instance is Reset again afterwards so the
        // first live block starts from clean, prewarmed state — the capture path stays
        // bit-identical to a load without the probe. Skipped for stages whose ref gain nothing
        // consumes (the preamp) — ~0.3 s of inference is not free on the message thread.
        if (measureRefGain)
        {
            const int settle = (int) std::lround (levelprobe::kSettleSec  * modelRunSR);
            const int total  = settle + (int) std::lround (levelprobe::kMeasureSec * modelRunSR);
            std::vector<float> in ((size_t) total), out ((size_t) total);
            levelprobe::fill (in.data(), total, modelRunSR);
            for (int off = 0; off < total; off += maxModelFrames)
            {
                const int n = std::min (total - off, maxModelFrames);
                NAM_SAMPLE* ins [1] = { in.data()  + off };
                NAM_SAMPLE* outs[1] = { out.data() + off };
                set->inst[0]->process (ins, outs, n);
            }
            double si = 0.0, so = 0.0;
            for (int i = settle; i < total; ++i) { si += (double) in[(size_t) i] * in[(size_t) i];
                                                   so += (double) out[(size_t) i] * out[(size_t) i]; }
            const double gain = (std::sqrt (so) * (double) set->makeup) / std::max (1.0e-9, std::sqrt (si));
            const float gainDb = (float) (20.0 * std::log10 (std::max (1.0e-9, gain)));
            set->refGainDb    = gainDb;
            set->refGainValid = std::isfinite (gainDb) && std::fabs (gainDb) < 48.0f;  // sanity bound
            set->inst[0]->Reset (modelRunSR, maxModelFrames);   // clear the probe's state (+ prewarm)
        }

        expectedSR.store (set->expectedSR, std::memory_order_relaxed);
        loudnessDb.store (set->loudnessDb, std::memory_order_relaxed);
        hasLoudness.store (set->hasLoudness, std::memory_order_relaxed);

        const float refDbStaged    = set->refGainDb;      // stage before release() — read after swap
        const bool  refValidStaged = set->refGainValid;
        retire (live.exchange (set.release(), std::memory_order_acq_rel));
        // The ref-gain mirrors are read by the AUDIO thread (the tube's capture level-match), so
        // publish them only after the swap: storing them first would pair the NEW model's gain
        // with the OLD still-live model for a block or two (review finding — a torn combination).
        refGainDb.store (refDbStaged, std::memory_order_relaxed);
        refGainValid.store (refValidStaged, std::memory_order_relaxed);
        return true;
    }

    // RT-safe: run one channel through its model instance, with optional resampling, applying makeup.
    void processChannel (Ch& c, nam::DSP* m, float* io, int n, float g)
    {
        if (! resampling)
        {
            // copy → model scratch (NAM may not allow in==out) → process → back, with makeup.
            std::copy (io, io + n, c.modelIn.data());
            NAM_SAMPLE* in [1] = { c.modelIn.data() };
            NAM_SAMPLE* out[1] = { c.modelOut.data() };
            m->process (in, out, n);
            for (int i = 0; i < n; ++i) io[i] = c.modelOut[i] * g;
            return;
        }

        // host → model (variable count), run NAM, model → host (exactly n).
        c.down.feed (io, n);
        const int mFrames = c.down.produceAvailable (c.modelIn.data(), maxModelFrames);
        if (mFrames > 0)
        {
            NAM_SAMPLE* in [1] = { c.modelIn.data() };
            NAM_SAMPLE* out[1] = { c.modelOut.data() };
            m->process (in, out, mFrames);
            c.up.feed (c.modelOut.data(), mFrames);
        }
        c.up.produceExact (io, n);
        for (int i = 0; i < n; ++i) io[i] *= g;
    }
};

//==============================================================================
AmpStage::AmpStage()  : impl (std::make_unique<Impl>()) {}

AmpStage::~AmpStage()
{
    if (impl != nullptr)
        if (ModelSet* m = impl->live.exchange (nullptr, std::memory_order_acq_rel))
            delete m;
}

void AmpStage::prepare (double sampleRate, int maxBlock)
{
    impl->hostSR   = sampleRate;
    impl->maxBlock = std::max (1, maxBlock);

    // Audio is stopped here, so reconfigure rates + re-Reset the live model in place. Capture the
    // live pointer ONCE — a concurrent message-thread swap mustn't split rate-config from the Reset.
    ModelSet* const m = impl->live.load (std::memory_order_acquire);
    const double modelSR = (m != nullptr && m->expectedSR > 0.0) ? m->expectedSR : kModelSampleRate;
    impl->configureRates (modelSR);
    if (m != nullptr)
        for (auto& inst : m->inst)
            if (inst) inst->Reset (impl->modelRunSR, impl->maxModelFrames);
}

void AmpStage::reset() {}   // transient state cleared by prepare()'s Reset on the next play

//==============================================================================
void AmpStage::process (float* const* io, int numChannels, int numSamples, bool normalize)
{
    auto& d = *impl;
    d.blockCount.fetch_add (1, std::memory_order_acq_rel);

    ModelSet* ms = d.live.load (std::memory_order_acquire);
    if (ms == nullptr || numChannels <= 0 || numSamples <= 0)
        return;                                              // no model → clean passthrough

    const int n = std::min (numSamples, d.maxBlock);
    const float g = normalize ? ms->makeup : 1.0f;

    if (numChannels == 1)
    {
        d.processChannel (d.ch[0], ms->inst[0].get(), io[0], n, g);   // mono track → 1 instance
    }
    else
    {
        // stereo track → 2 independent instances (true stereo). Extra channels (none on a stereo
        // bus) are left untouched.
        d.processChannel (d.ch[0], ms->inst[0].get(), io[0], n, g);
        d.processChannel (d.ch[1], ms->inst[1].get(), io[1], n, g);
    }
}

//==============================================================================
namespace
{
    std::unique_ptr<ModelSet> buildSetFromJson (const nlohmann::json& j)
    {
        auto set = std::make_unique<ModelSet>();
        set->inst[0] = nam::get_dsp (j);            // two independent instances of the same capture
        set->inst[1] = nam::get_dsp (j);
        return set;
    }
}

bool AmpStage::loadModelFromMemory (const void* data, std::size_t size, float trimDb, bool measureRefGain)
{
    std::unique_ptr<ModelSet> set;
    try
    {
        auto j = nlohmann::json::parse (static_cast<const char*> (data),
                                        static_cast<const char*> (data) + size);
        set = buildSetFromJson (j);
    }
    catch (...) { return false; }
    return impl->adopt (std::move (set), trimDb, measureRefGain);
}

void AmpStage::clearModel()
{
    impl->expectedSR.store (0.0, std::memory_order_relaxed);
    impl->loudnessDb.store (0.0, std::memory_order_relaxed);
    impl->hasLoudness.store (false, std::memory_order_relaxed);
    impl->refGainDb.store (0.0f, std::memory_order_relaxed);
    impl->refGainValid.store (false, std::memory_order_relaxed);
    impl->retire (impl->live.exchange (nullptr, std::memory_order_acq_rel));
}

void AmpStage::collectGarbage()
{
    const std::uint64_t now = impl->blockCount.load (std::memory_order_acquire);
    auto& gbg = impl->garbage;
    gbg.erase (std::remove_if (gbg.begin(), gbg.end(),
                               [now] (const Impl::Retired& r) { return now > r.atBlock; }),
               gbg.end());
}

bool   AmpStage::hasModel()         const { return impl->live.load (std::memory_order_acquire) != nullptr; }
double AmpStage::modelSampleRate()  const { return impl->expectedSR.load  (std::memory_order_relaxed); }
double AmpStage::modelLoudness()    const { return impl->loudnessDb.load  (std::memory_order_relaxed); }
bool   AmpStage::modelHasLoudness() const { return impl->hasLoudness.load (std::memory_order_relaxed); }
float  AmpStage::refGainDb()        const { return impl->refGainDb.load   (std::memory_order_relaxed); }
bool   AmpStage::refGainValid()     const { return impl->refGainValid.load (std::memory_order_relaxed) && hasModel(); }
int    AmpStage::latencySamples()   const { return hasModel() ? impl->latencyHostSamples() : 0; }

} // namespace cab
