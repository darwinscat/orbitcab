// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "AmpStage.h"

#include <NAM/dsp.h>        // NAM_SAMPLE (float, via NAM_SAMPLE_FLOAT) + class nam::DSP
#include <NAM/get_dsp.h>    // nam::get_dsp(path|json)

#include <felitronics/neural/NeuralStage.h>   // the shared swap-safe holder (extracted FROM this file)

#include "StreamResampler.h"   // cab::StreamResampler (header-only, unit-tested separately)
#include "NamCodec.h"          // ocnam:: — load path accepts BOTH raw .nam JSON and packed .namz

#include <algorithm>
#include <atomic>
#include <cmath>
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

//==============================================================================
// NamBackend — the NAM half of the split. The generic model-swap machinery (atomic live pointer,
// block-counter retire, message-thread GC) moved to felitronics::neural::NeuralStage (it was
// extracted from this file); what stays local is everything NAM-specific, shaped as a
// felitronics::neural::Inference backend. One backend instance = one loaded capture: TWO
// independent nam::DSP instances of the SAME capture (true stereo — L/R independent; a mono
// stream uses just instance 0), the −18 dB loudness makeup (per-model trim folded in), and the
// per-channel StreamResampler rate-match state + scratch. NeuralStage swaps WHOLE instances of
// this class, so the resampler/scratch state travels with the model it belongs to.
class NamBackend
{
public:
    // Both instances always exist (built by the loader) and prepare() configures both per-channel
    // resamplers, so a host switching the layout mono<->stereo (a re-prepare) is safe.
    // `normalizeFlag` is owned by AmpStage::Impl: the audio thread stores the per-call `normalize`
    // argument there right before NeuralStage::process() reaches this backend (same thread →
    // sequenced), keeping AmpStage's per-block flag without widening the shared Inference seam.
    NamBackend (std::unique_ptr<nam::DSP> m0, std::unique_ptr<nam::DSP> m1,
                float trimDb, const std::atomic<bool>& normalizeFlag)
        : normalize (&normalizeFlag)
    {
        inst[0] = std::move (m0);
        inst[1] = std::move (m1);

        expectedSR = inst[0]->GetExpectedSampleRate();
        if (inst[0]->HasLoudness())
        {
            loudnessDb  = inst[0]->GetLoudness();
            hasLoudness = true;
            makeup = dbToGain ((float) (kNormTargetDb - loudnessDb) + trimDb);
        }
        else
        {
            loudnessDb = 0.0; hasLoudness = false;
            makeup = dbToGain (trimDb);
        }
    }

    //--- felitronics::neural::Inference seam --------------------------------------
    // Message thread, with audio stopped (live instance, via NeuralStage::prepare) or on a
    // not-yet-live instance (the loader). Decide the run rate, (re)configure the per-channel
    // resamplers/scratch, and Reset both instances to it (alloc + prewarm — fine off the audio
    // thread). Both lanes are always configured — see the ctor note on mono<->stereo re-prepare.
    void prepare (double sampleRate, int maxBlockIn, int /*maxChannels*/) noexcept
    {
        hostSR   = sampleRate;
        maxBlock = std::max (1, maxBlockIn);
        configureRates (expectedSR > 0.0 ? expectedSR : kModelSampleRate);
        for (auto& m : inst)
            if (m) m->Reset (modelRunSR, maxModelFrames);
    }

    // 🔴 RT-safe, in place — the audio thread reaches this via NeuralStage::process() on the
    // live instance. Never allocates, locks, does IO or throws.
    void process (float* const* io, int numChannels, int numSamples) noexcept
    {
        if (numChannels <= 0 || numSamples <= 0)
            return;

        const int   n = std::min (numSamples, maxBlock);
        const float g = normalize->load (std::memory_order_relaxed) ? makeup : 1.0f;

        if (numChannels == 1)
        {
            processChannel (ch[0], inst[0].get(), io[0], n, g);   // mono track → 1 instance
        }
        else
        {
            // stereo track → 2 independent instances (true stereo). Extra channels (none on a stereo
            // bus) are left untouched.
            processChannel (ch[0], inst[0].get(), io[0], n, g);
            processChannel (ch[1], inst[1].get(), io[1], n, g);
        }
    }

    void reset() noexcept {}   // transient state cleared by prepare()'s Reset on the next play

    // Host-rate latency the rate-matcher introduces (0 when not resampling). The cubic resampler
    // needs ~3 samples of lookahead per stage to prime; round-trip ≈ down (model→host) + up.
    int latencySamples() const noexcept
    {
        if (! resampling) return 0;
        return (int) std::ceil (3.0 * hostSR / modelRunSR) + 3;
    }

    //--- model info (read by the loader for AmpStage's UI-mirror atomics) ----------
    double reportedSampleRate()  const noexcept { return expectedSR; }
    double reportedLoudnessDb()  const noexcept { return loudnessDb; }
    bool   reportedHasLoudness() const noexcept { return hasLoudness; }

private:
    // Decide the run rate + (re)configure per-channel resamplers/scratch (prepare() only — never
    // while this instance is live and audio runs). modelRunSR = the loaded model's native rate.
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

    // Per-channel resampler state + model-rate scratch (used only when resampling).
    struct Ch
    {
        StreamResampler down, up;       // host->model, model->host
        std::vector<float> modelIn, modelOut;
    };

    // RT-safe: run one channel through its model instance, with optional resampling, applying makeup.
    void processChannel (Ch& c, nam::DSP* m, float* io, int n, float g) noexcept
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

    std::unique_ptr<nam::DSP> inst[2];
    const std::atomic<bool>*  normalize = nullptr;   // AmpStage::Impl's per-call flag (see ctor)

    float  makeup      = 1.0f;
    double expectedSR  = 0.0;
    double loudnessDb  = 0.0;
    bool   hasLoudness = false;

    double hostSR   = 48000.0;
    int    maxBlock = 512;
    bool   resampling = false;              // hostSR != model native rate
    double modelRunSR = kModelSampleRate;   // the rate the NAM instances are Reset to / run at
    int    maxModelFrames = 1024;
    Ch     ch[2];
};

static_assert (felitronics::neural::Inference<NamBackend>,
               "NamBackend must satisfy the felitronics::neural process-only inference seam");

} // namespace

//==============================================================================
struct AmpStage::Impl
{
    // The swap-safe holder: atomic live-pointer swap, block-counter retire, message-thread GC.
    // MaxRetired 64: the pre-extraction code kept an UNBOUNDED retire vector; 64 pending models
    // keeps load/clear effectively unconditional (the queue can only stay full if the audio
    // thread is frozen across 64 swaps — the message thread drains it via collectGarbage()).
    felitronics::neural::NeuralStage<NamBackend, 64> stage;

    // UI mirrors (message thread reads these).
    std::atomic<double> expectedSR  { 0.0 };
    std::atomic<double> loudnessDb  { 0.0 };
    std::atomic<bool>   hasLoudness { false };

    // Per-call `normalize` handoff to the live backend (see AmpStage::process / NamBackend ctor).
    std::atomic<bool> normalize { true };

    double hostSR   = 48000.0;
    int    maxBlock = 512;
    double modelRunSR = kModelSampleRate;   // run rate configured at the last prepare() — the
                                            // reference for the mid-stream model-rate contract
};

//==============================================================================
AmpStage::AmpStage()  : impl (std::make_unique<Impl>()) {}
AmpStage::~AmpStage() = default;   // NeuralStage frees the live + retired models

void AmpStage::prepare (double sampleRate, int maxBlock)
{
    impl->hostSR   = sampleRate;
    impl->maxBlock = std::max (1, maxBlock);

    // Audio is stopped here. The live model (if any) decides the run rate — remember it for the
    // mid-stream rate contract in loadModelFromMemory() — and NeuralStage re-prepares the live
    // backend in place (it captures the live pointer ONCE, so a concurrent message-thread swap
    // can't split the rate-config from the Reset).
    const double liveSR = impl->expectedSR.load (std::memory_order_relaxed);
    impl->modelRunSR = (liveSR > 0.0 ? liveSR : kModelSampleRate);
    impl->stage.prepare ({ sampleRate, impl->maxBlock, 2 });
}

void AmpStage::reset() {}   // transient state cleared by prepare()'s Reset on the next play

//==============================================================================
void AmpStage::process (float* const* io, int numChannels, int numSamples, bool normalize)
{
    // The shared Inference seam is process(io, nc, n) — the per-block `normalize` flag travels via
    // an atomic the live backend reads inside the SAME call (same thread → sequenced). Block
    // counting + the live-model resolve live in NeuralStage; no model → clean passthrough.
    impl->normalize.store (normalize, std::memory_order_relaxed);
    impl->stage.process (io, numChannels, numSamples);
}

//==============================================================================
bool AmpStage::loadModelFromMemory (const void* data, std::size_t size, float trimDb)
{
    // Accept BOTH the raw .nam JSON and our packed .namz (weights as float32 + deflate). A packed
    // blob is unpacked to the equivalent JSON first, then the existing parse→get_dsp path runs
    // unchanged — .namz is bit-exact to the float32 the engine computes, so the model is identical.
    // Off the audio thread (message-thread reload poll); the alloc/parse cost is fine here.
    constexpr std::size_t kMaxUnpackedNamBytes = 64u * 1024u * 1024u;   // zip-bomb guard on unpack

    std::unique_ptr<nam::DSP> m0, m1;
    try
    {
        juce::MemoryBlock unpacked;                    // owns the reconstructed JSON iff input was packed
        const char* begin = static_cast<const char*> (data);
        const char* end   = begin + size;
        if (ocnam::isNamz (data, size))
        {
            unpacked = ocnam::unpack (data, size, kMaxUnpackedNamBytes);
            if (unpacked.getSize() == 0)
                return false;
            begin = static_cast<const char*> (unpacked.getData());
            end   = begin + unpacked.getSize();
        }
        auto j = nlohmann::json::parse (begin, end);
        m0 = nam::get_dsp (j);            // two independent instances of the same capture
        m1 = nam::get_dsp (j);
    }
    catch (...) { return false; }

    if (m0 == nullptr || m1 == nullptr)
        return false;
    // prototype: mono amp captures only
    if (m0->NumInputChannels() != 1 || m0->NumOutputChannels() != 1)
        return false;

    // Rate contract: the run rate was decided in prepare() (audio stopped) — a model whose native
    // rate differs from it would need the resamplers reconfigured for a rate the host chain wasn't
    // prepared for, and would report a different latency than the one the host compensated. Refuse
    // it instead of misrepresenting it. A model that doesn't report a rate (<= 0) keeps the
    // previous behaviour (run at modelRunSR). (Factory NAMs are 48k, so this never fires for the
    // bundled library.)
    const double modelSR = m0->GetExpectedSampleRate();
    if (modelSR > 0.0 && std::abs (modelSR - impl->modelRunSR) > 0.5)
        return false;

    // Build + prepare the new backend while it is NOT live (alloc + prewarm is fine here), then
    // hand it to NeuralStage: atomic swap in, old model retired for message-thread GC.
    auto backend = std::make_unique<NamBackend> (std::move (m0), std::move (m1), trimDb, impl->normalize);
    backend->prepare (impl->hostSR, impl->maxBlock, 2);

    const double sr  = backend->reportedSampleRate();
    const double ldb = backend->reportedLoudnessDb();
    const bool   hl  = backend->reportedHasLoudness();
    if (! impl->stage.swapPrepared (std::move (backend)))
        return false;

    impl->expectedSR.store (sr, std::memory_order_relaxed);
    impl->loudnessDb.store (ldb, std::memory_order_relaxed);
    impl->hasLoudness.store (hl, std::memory_order_relaxed);
    return true;
}

void AmpStage::clearModel()
{
    impl->expectedSR.store (0.0, std::memory_order_relaxed);
    impl->loudnessDb.store (0.0, std::memory_order_relaxed);
    impl->hasLoudness.store (false, std::memory_order_relaxed);
    impl->stage.clear();   // retire the live model for message-thread GC
}

void AmpStage::collectGarbage()
{
    impl->stage.collectGarbage();
}

bool   AmpStage::hasModel()         const { return impl->stage.hasModel(); }
double AmpStage::modelSampleRate()  const { return impl->expectedSR.load  (std::memory_order_relaxed); }
double AmpStage::modelLoudness()    const { return impl->loudnessDb.load  (std::memory_order_relaxed); }
bool   AmpStage::modelHasLoudness() const { return impl->hasLoudness.load (std::memory_order_relaxed); }
int    AmpStage::latencySamples()   const { return impl->stage.latencySamples(); }

} // namespace cab
