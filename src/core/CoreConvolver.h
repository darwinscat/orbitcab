// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa. Part of OrbitCab — see LICENSE.

#pragma once

#include <felitronics/convolution/ConvolutionEngine.h>
#include <felitronics/convolution/IrResampler.h>
#include <felitronics/core/Fft.h>

#include <algorithm>
#include <cmath>
#include <vector>

//==============================================================================
// cab::CoreConvolver — the JUCE-FREE convolution backend, a drop-in twin of cab::Convolver behind the
// same raw-float signature, for the dev A/B switch (felitronics::convolution::ConvolutionEngine).
//
// It replicates juce::dsp::Convolution's load semantics so the A/B is HONEST, per the line-by-line
// spec docs/migration/convolution-core-api.md (verified vs the vendored juce_Convolution.cpp):
//   • Normalise::no STILL multiplies the IR by irSr/hostSr after any resample (JUCE applyGain).
//   • resample ONLY when irSr != hostSr (the Kaiser path at ratio 1 would still LPF a host-rate IR).
//   • IR swap crossfades 50 ms (== JUCE's CrossoverMixer) → set crossfadeSamples = round(0.05*hostSr).
//   • zero-latency (head + partitioned tail), like JUCE's default → PDC / wet-dry alignment unchanged.
// Templated on the FFT backend: the scalar reference for the headless golden test; a juce::dsp::FFT
// wrapper for the real-time plugin.
//==============================================================================
namespace cab
{

template <class Fft = felitronics::core::fft::DefaultRealFft>
class CoreConvolver
{
public:
    // maxIrSeconds sizes the convolution (and, in this engine, the ONE cold-start fade — which is the
    // configured tail length, NOT the loaded IR's length). Keep it cab-sane (~4 s) so the first-load
    // fade-in isn't huge; OrbitCab's 20 s DECODE cap is a separate safety limit, longer IRs truncate here.
    void prepare (double sampleRate, int maxBlock, int numChannels, double maxIrSeconds = 4.0)
    {
        hostSr_   = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels_ = std::clamp (numChannels, 1, 2);
        int P = 128; while (P < std::max (1, maxBlock)) P <<= 1;               // partition ≥ maxBlock (engine rule)
        const int maxIr = std::max (P * 2, (int) std::ceil (maxIrSeconds * hostSr_));
        const int xfade = std::max (1, (int) std::lround (0.05 * hostSr_));   // 50 ms — matches JUCE
        engine_.prepare (P, maxIr, xfade, channels_);
    }

    void reset() { engine_.reset(); }

    // The normal cab path (JUCE Stereo::yes, Trim::no, Normalise::no). Message thread.
    void loadIR (const float* const* samples, int numChannels, int numSamples, double irSampleRate)
    {
        buildAndStage (samples, numChannels, numSamples, irSampleRate, /*normaliseYes=*/false);
    }

    // The byte-fallback path's equivalent (JUCE Normalise::yes: energy norm to −18 dB). Message thread.
    void loadIRNormalised (const float* const* samples, int numChannels, int numSamples, double irSampleRate)
    {
        buildAndStage (samples, numChannels, numSamples, irSampleRate, /*normaliseYes=*/true);
    }

    // RT-safe in-place convolution of `numChannels` planar channels.
    void process (float* const* io, int numChannels, int numSamples)
    {
        engine_.process (io, io, std::min (numChannels, channels_), numSamples);
    }

    bool isBusy() const noexcept { return engine_.isBusy(); }
    static constexpr int latencySamples() noexcept { return 0; }

private:
    void buildAndStage (const float* const* samples, int nch, int len, double irSr, bool normaliseYes)
    {
        nch = std::clamp (nch, 1, 2);
        ir_.resize ((std::size_t) nch);

        // 1. per-channel: resample to host SR only if the rates differ (else copy as-is).
        int outLen = len;
        for (int c = 0; c < nch; ++c)
        {
            if (irSr > 0.0 && irSr != hostSr_)
                ir_[(std::size_t) c] = felitronics::convolution::resampleIr (samples[c], len, irSr, hostSr_);
            else
                ir_[(std::size_t) c].assign (samples[c], samples[c] + len);
            outLen = (int) ir_[(std::size_t) c].size();
        }

        // 2. the JUCE-equivalent gain (see the spec table).
        float g;
        if (normaliseYes)                                  // Normalise::yes — max-channel energy norm
        {
            double E = 0.0;
            for (int c = 0; c < nch; ++c)
            {
                double s = 0.0;
                for (float v : ir_[(std::size_t) c]) s += (double) v * (double) v;
                E = std::max (E, s);
            }
            g = (E < 1e-8) ? 1.0f : (float) (0.125 / std::sqrt (E));
        }
        else                                               // Normalise::no — the normal cab path
            g = (irSr > 0.0) ? (float) (irSr / hostSr_) : 1.0f;

        for (auto& ch : ir_)
            for (float& v : ch) v *= g;

        // 3. stage into the inactive slot (click-free swap). If a swap is mid-flight, keep the latest
        //    build pending and retry on the next call (mirrors the existing reload-coalescing).
        ptrs_.resize ((std::size_t) nch);
        for (int c = 0; c < nch; ++c) ptrs_[(std::size_t) c] = ir_[(std::size_t) c].data();
        pendingLen_ = outLen; pendingNch_ = nch;
        flushPending();
    }

    void flushPending()
    {
        if (pendingLen_ <= 0) return;
        if (engine_.setIr (ptrs_.data(), pendingNch_, pendingLen_))
            pendingLen_ = 0;   // staged; clear pending
        // else: still busy — caller (message thread) re-invokes loadIR with the latest IR anyway.
    }

    double hostSr_   = 48000.0;
    int    channels_ = 2;
    felitronics::convolution::ConvolutionEngine<Fft, 2> engine_;

    std::vector<std::vector<float>> ir_;
    std::vector<const float*>       ptrs_;
    int pendingLen_ = 0, pendingNch_ = 0;
};

} // namespace cab
