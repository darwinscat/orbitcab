// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <cstddef>
#include <memory>

//==============================================================================
// cab::AmpStage — a neural amp (NAM) stage that runs IN FRONT of the cab convolution
// (signal path: input → AMP → cab). Same "seam" discipline as cab::Convolver: the public
// API speaks raw float* + sizes, and the Neural Amp Modeler inference engine (Eigen +
// nlohmann/json) is hidden in the .cpp behind a pImpl — so NAM never leaks into the rest
// of the Core, and the dependency stays swappable.
//
// Threading mirrors the IR path exactly:
//   • prepare() / loadModelFromMemory() / clearModel() / collectGarbage() — message thread.
//   • process() — the ONLY thing the audio thread calls. 🔴 It never allocates, locks,
//     does IO or throws. A freshly-loaded model is atomic-swapped into the live pointer;
//     the replaced model is parked and freed on the message thread (collectGarbage) only
//     once the audio thread has provably stepped past it — so no use-after-free and the
//     audio thread never deletes.
//
// The swap machinery itself (atomic live pointer, block-counter retire, message-thread GC) is
// the shared felitronics::neural::NeuralStage — extracted FROM this class into felitronics-core;
// what stays here is the NAM-specific backend (dual-instance true stereo, loudness makeup, .namz
// unpack, rate-match) in the .cpp. This class remains the stable public seam — call-sites don't
// see the split.
//
// Channels: a MONO stream (1 ch) runs ONE model instance on the single channel; a STEREO stream
// (2 ch) runs TWO independent instances of the SAME capture (true stereo — L/R independent). Both
// instances are always built on load, and prepare() configures both per-channel resamplers, so a
// host switching the layout mono<->stereo (a re-prepare) is safe.
//==============================================================================
namespace cab
{

class AmpStage
{
public:
    AmpStage();
    ~AmpStage();

    // Allocate the mono scratch for this stream and (re)configure a live model for the
    // new sample-rate / block size. Message/host thread (prepareToPlay) — never the audio
    // thread (it can allocate + prewarm the network).
    void prepare (double sampleRate, int maxBlock);
    void reset();

    // 🔴 RT-safe, in place. No model loaded → clean passthrough (no-op).
    // `normalize` applies the model's loudness makeup (output normalisation) when the model
    // carries a loudness tag — brings raw model output to a consistent reference level.
    void process (float* const* io, int numChannels, int numSamples, bool normalize);

    //--- model lifecycle (message thread) ----------------------------------------
    // Build a NAM model from raw .nam bytes off the audio thread and atomic-swap it in.
    // Returns false (and leaves the current model untouched) on a bad / unsupported /
    // non-mono model. The replaced model is reclaimed later via collectGarbage().
    // A load (or clear) that was ACCEPTED is guaranteed to apply: immediately in the normal
    // case, or — if the bounded swap-retire queue is momentarily full (audio frozen across
    // many swaps) — on the next collectGarbage()/prepare() drain. The info getters below
    // update only when it actually lands, so they always describe the model that is live.
    // trimDb = per-model output offset (dB) folded into the loudness-normalisation makeup.
    // (Bytes only — file I/O stays in the adapter layer, never in pure-DSP core.)
    bool   loadModelFromMemory (const void* data, std::size_t size, float trimDb = 0.0f);
    void   clearModel();
    void   collectGarbage();          // free models retired by a swap, once audio moved past them,
                                      // and land any load/clear deferred by a full retire queue

    bool   hasModel() const;
    double modelSampleRate() const;   // the model's expected sample rate (<= 0 if none / unknown)
    double modelLoudness()   const;   // the model's tagged loudness in dB (0 if none / no model)
    bool   modelHasLoudness() const;  // whether the loaded model carries a loudness tag
    int    latencySamples()  const;   // host-rate latency from rate-matching (0 if none / not resampling)

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cab
