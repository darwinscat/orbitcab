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
// cab::Convolver — the cab IR convolution, now JUCE-FREE: it runs on
// felitronics::convolution::ConvolutionEngine (partitioned, zero-latency, click-free IR swap) instead
// of juce::dsp::Convolution. The public methods speak raw floats + sizes, so the adapter (IRSlot) is
// unchanged. NULL-tested to −170 dB against a direct convolution on real cabs (host + resampled).
//
// JUCE load semantics are replicated EXACTLY (so the swap is sound-preserving), per the line-by-line
// spec felitronics-core/docs/migration/convolution-core-api.md (verified vs juce_Convolution.cpp):
//   • Normalise::no STILL multiplies the IR by irSr/hostSr after any resample (JUCE applyGain);
//   • resample ONLY when irSr != hostSr (the Kaiser path at ratio 1 would still LPF a host-rate IR);
//   • IR swap crossfades 50 ms (== JUCE's CrossoverMixer); zero-latency (PDC / wet-dry unchanged).
// Byte-decoding moved OUT to the adapter (IRSlot) so this stays JUCE-free; loadIR/loadIRNormalised
// take decoded planar samples (Normalise::no = the normal cab path; Normalise::yes = the fallback).
//==============================================================================
namespace cab
{

class Convolver
{
public:
    // maxIrSeconds sizes the convolution and the ONE cold-start fade (= the configured tail, NOT the IR
    // length). ~4 s keeps the first-load fade sane; OrbitCab's 20 s DECODE cap is a separate safety limit.
    void prepare (double sampleRate, int maxBlock, int numChannels, double maxIrSeconds = 4.0)
    {
        hostSr_   = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels_ = std::clamp (numChannels, 1, 2);
        int P = 128; while (P < std::max (1, maxBlock)) P <<= 1;               // partition >= maxBlock (engine rule)
        const int maxIr = std::max (P * 2, (int) std::ceil (maxIrSeconds * hostSr_));
        const int xfade = std::max (1, (int) std::lround (0.05 * hostSr_));    // 50 ms — matches JUCE
        engine_.prepare (P, maxIr, xfade, channels_);
    }

    void reset() { engine_.reset(); }

    // The normal cab path (JUCE Stereo::yes, Trim::no, Normalise::no). Message thread.
    void loadIR (const float* const* samples, int numChannels, int numSamples, double irSampleRate)
    {
        buildAndStage (samples, numChannels, numSamples, irSampleRate, /*normaliseYes=*/false);
    }

    // The fallback path's equivalent (JUCE Normalise::yes: max-channel energy norm to −18 dB). Message thread.
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

    // Message thread. If a previous loadIR() was rejected because the engine was mid-crossfade, retry it
    // now (the latest IR is kept staged in ir_). The engine accepts once its crossfade finishes, so a
    // periodic caller (the reload poll) lands the coalesced swap. Returns true when nothing is pending.
    bool flushPending()
    {
        if (pendingLen_ <= 0) return true;
        if (engine_.setIr (ptrs_.data(), pendingNch_, pendingLen_)) { pendingLen_ = 0; return true; }
        return false;
    }
    bool hasPending() const noexcept { return pendingLen_ > 0; }

private:
    void buildAndStage (const float* const* samples, int nch, int len, double irSr, bool normaliseYes)
    {
        nch = std::clamp (nch, 1, 2);
        ir_.resize ((std::size_t) nch);

        int outLen = len;
        for (int c = 0; c < nch; ++c)                                          // resample only off host rate
        {
            if (irSr > 0.0 && irSr != hostSr_)
                ir_[(std::size_t) c] = felitronics::convolution::resampleIr (samples[c], len, irSr, hostSr_);
            else
                ir_[(std::size_t) c].assign (samples[c], samples[c] + len);
            outLen = (int) ir_[(std::size_t) c].size();
        }

        float g;                                                               // the JUCE-equivalent gain
        if (normaliseYes)                                                      // Normalise::yes — max-channel energy
        {
            double E = 0.0;
            for (int c = 0; c < nch; ++c) { double s = 0.0; for (float v : ir_[(std::size_t) c]) s += (double) v * v; E = std::max (E, s); }
            g = (E < 1e-8) ? 1.0f : (float) (0.125 / std::sqrt (E));
        }
        else                                                                  // Normalise::no — the normal cab path
            g = (irSr > 0.0) ? (float) (irSr / hostSr_) : 1.0f;

        for (auto& ch : ir_) for (float& v : ch) v *= g;

        ptrs_.resize ((std::size_t) nch);
        for (int c = 0; c < nch; ++c) ptrs_[(std::size_t) c] = ir_[(std::size_t) c].data();
        if (! engine_.setIr (ptrs_.data(), nch, outLen)) { pendingLen_ = outLen; pendingNch_ = nch; }
        else pendingLen_ = 0;
    }

    double hostSr_   = 48000.0;
    int    channels_ = 2;
    felitronics::convolution::ConvolutionEngine<felitronics::core::fft::DefaultRealFft, 2> engine_;

    std::vector<std::vector<float>> ir_;
    std::vector<const float*>       ptrs_;
    int pendingLen_ = 0, pendingNch_ = 0;
};

} // namespace cab
