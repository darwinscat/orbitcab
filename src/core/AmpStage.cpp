// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "AmpStage.h"

#include <NAM/dsp.h>        // NAM_SAMPLE (float, via NAM_SAMPLE_FLOAT) + class nam::DSP
#include <NAM/get_dsp.h>    // nam::get_dsp(path|json)

#include "StreamResampler.h"   // cab::StreamResampler (header-only, unit-tested separately)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    bool adopt (std::unique_ptr<ModelSet> set, float extraTrimDb)
    {
        if (set == nullptr || set->inst[0] == nullptr || set->inst[1] == nullptr)
            return false;
        // prototype: mono amp captures only
        if (set->inst[0]->NumInputChannels() != 1 || set->inst[0]->NumOutputChannels() != 1)
            return false;

        const double modelSR = set->inst[0]->GetExpectedSampleRate();
        // Resamplers are configured for the model's native rate in prepare() (audio stopped); do
        // NOT reconfigure them here — audio may be live, and resetting them would race. Just Reset
        // the new, not-yet-live instances to the run rate. (All factory captures are 48 kHz.)
        for (auto& m : set->inst)
            m->Reset (modelRunSR, maxModelFrames);           // run at native rate; alloc + prewarm

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

        expectedSR.store (set->expectedSR, std::memory_order_relaxed);
        loudnessDb.store (set->loudnessDb, std::memory_order_relaxed);
        hasLoudness.store (set->hasLoudness, std::memory_order_relaxed);

        retire (live.exchange (set.release(), std::memory_order_acq_rel));
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

    // Audio is stopped here, so reconfigure rates + re-Reset the live model in place.
    double modelSR = kModelSampleRate;
    if (ModelSet* m = impl->live.load (std::memory_order_acquire))
        modelSR = (m->expectedSR > 0.0 ? m->expectedSR : kModelSampleRate);
    impl->configureRates (modelSR);
    if (ModelSet* m = impl->live.load (std::memory_order_acquire))
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

bool AmpStage::loadModelFile (const std::string& path, float trimDb)
{
    std::unique_ptr<ModelSet> set;
    try
    {
        std::ifstream f (path);
        nlohmann::json j; f >> j;
        set = buildSetFromJson (j);
    }
    catch (...) { return false; }
    return impl->adopt (std::move (set), trimDb);
}

bool AmpStage::loadModelFromMemory (const void* data, std::size_t size, float trimDb)
{
    std::unique_ptr<ModelSet> set;
    try
    {
        auto j = nlohmann::json::parse (static_cast<const char*> (data),
                                        static_cast<const char*> (data) + size);
        set = buildSetFromJson (j);
    }
    catch (...) { return false; }
    return impl->adopt (std::move (set), trimDb);
}

void AmpStage::clearModel()
{
    impl->expectedSR.store (0.0, std::memory_order_relaxed);
    impl->loudnessDb.store (0.0, std::memory_order_relaxed);
    impl->hasLoudness.store (false, std::memory_order_relaxed);
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
int    AmpStage::latencySamples()   const { return hasModel() ? impl->latencyHostSamples() : 0; }

} // namespace cab
