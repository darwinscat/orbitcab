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
// cab::Convolver — the cab IR convolution, JUCE-FREE: it runs on
// felitronics::convolution::ConvolutionEngine (partitioned, zero-latency, click-free IR swap) instead
// of juce::dsp::Convolution. The public methods speak raw floats + sizes, so the adapter (IRSlot) is
// unchanged. NULL-tested to −170 dB against a direct convolution on real cabs (host + resampled).
//
// LOUDNESS: every IR is normalized AT LOAD to reference-unity — unity RMS gain for a guitar-band
// reference signal (white noise through a one-pole low-pass at kIrRefShapeHz), computed in the
// frequency domain from the FINAL IR (post-trim, post-resample) and applied as ONE common gain
// across channels (stereo imaging untouched). Vendor cab IRs are peak-normalized bandpasses, so
// their passband sits +13…+18 dB above unity (measured across the factory set) — convolving them
// RAW (JUCE Normalise::no, the previous behaviour) made the wet path that much hotter than the
// dry, pushed the output past the ±24 dB trim range with auto-level off, and got worse the
// quieter the input. After normalization a cab contributes TONE, not gain: the auto-leveler
// shrinks to a small signal-dependent corrector and the plugin is usable with it off.
//
// This deliberately REPLACES the JUCE gain semantics (Normalise::no's irSr/hostSr factor and the
// Normalise::yes 0.125-energy fallback — both paths now normalize identically): the reference
// gain is measured on the resampled IR, which supersedes both. Engine behaviour is otherwise
// JUCE-parity per felitronics-core/docs/migration/convolution-core-api.md: resample ONLY when
// irSr != hostSr; 50 ms swap crossfade; zero latency. Byte-decoding stays OUT in the adapter
// (IRSlot) so this header remains JUCE-free.
//==============================================================================
namespace cab
{

class Convolver
{
public:
    // The reference the IRs are normalized against: RMS gain for white noise shaped by a one-pole
    // low-pass at this corner — a deterministic stand-in for guitar-band program material. The
    // same 2 kHz/-18 dBFS reference anchors the NAM-stage level probes, so "unity" means the same
    // thing across the whole plugin.
    static constexpr double kIrRefShapeHz = 2000.0;
    // Clamp: a pathological IR can't blast or vanish. ±30 dB clears every REAL library measured
    // (186 commercial IRs: reference gains +12…+24.1 dB — 96 kHz packs run the hottest) with
    // headroom, so the guard only ever bites genuine garbage.
    static constexpr float  kIrNormMinDb  = -30.0f;
    static constexpr float  kIrNormMaxDb  = 30.0f;
    static constexpr double kIrRefFloorDb = -60.0;    // near-silent IR ⇒ keep g = 1 (don't amplify garbage)

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

    // Load an IR (JUCE Stereo::yes parity: mono broadcasts) — normalized to reference-unity.
    // Message thread.
    void loadIR (const float* const* samples, int numChannels, int numSamples, double irSampleRate)
    {
        buildAndStage (samples, numChannels, numSamples, irSampleRate);
    }

    // The normalization applied to the last loaded IR — the exact linear gain (for references
    // that must null against the engine) and its dB reading (diagnostics). Message thread.
    float irNormalizationGain()   const noexcept { return normGain_; }
    float irNormalizationGainDb() const noexcept { return normGainDb_; }

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
    void buildAndStage (const float* const* samples, int nch, int len, double irSr)
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

        // Reference-unity normalization of the FINAL (resampled) IR — one common gain, all channels.
        const float g = normalizationGain (outLen);
        normGain_   = g;
        normGainDb_ = 20.0f * std::log10 (std::max (1.0e-6f, g));
        for (auto& ch : ir_) for (float& v : ch) v *= g;

        ptrs_.resize ((std::size_t) nch);
        for (int c = 0; c < nch; ++c) ptrs_[(std::size_t) c] = ir_[(std::size_t) c].data();
        if (! engine_.setIr (ptrs_.data(), nch, outLen)) { pendingLen_ = outLen; pendingNch_ = nch; }
        else pendingLen_ = 0;
    }

    // 1 / (reference RMS gain) of the staged IR: G² = Σ w(f)·P(f) / Σ w(f) over the positive-
    // frequency bins (DC excluded), where P(f) is the channel-mean power response |H(f)|² and
    // w(f) = 1 / (1 + (f/kIrRefShapeHz)²) is the one-pole-shaped reference's power spectrum.
    // Frequency domain (one real FFT per channel on the message thread) — equals convolving the
    // reference noise through the IR and reading the RMS ratio, without needing a signal.
    float normalizationGain (int len) const
    {
        // Analysis window: the first second. An IR's tail past that carries so little band energy
        // that it moves the reference gain by < 0.1 dB (measured on the factory set down to 10%
        // trims), and the trim drag re-runs this per mouse move — the cap keeps the drag light.
        const int cap = std::min (len, (int) std::lround (hostSr_));
        int N = 256; while (N < cap * 2 && N < (1 << 21)) N <<= 1;   // dense DTFT sampling of the IR
        felitronics::core::fft::DefaultRealFft fft;
        if (! fft.prepare (N)) return 1.0f;

        std::vector<float> padded ((std::size_t) N, 0.0f);
        std::vector<float> spec ((std::size_t) felitronics::core::fft::DefaultRealFft::spectrumFloats (N));
        std::vector<double> power ((std::size_t) (N / 2 + 1), 0.0);
        for (const auto& ch : ir_)
        {
            std::fill (padded.begin(), padded.end(), 0.0f);
            std::copy (ch.begin(), ch.begin() + std::min<std::ptrdiff_t> (cap, (std::ptrdiff_t) ch.size()),
                       padded.begin());
            fft.forward (padded.data(), spec.data());
            power[0]                    += (double) spec[0] * spec[0];                 // DC (unused below)
            power[(std::size_t) (N / 2)] += (double) spec[1] * spec[1];               // Nyquist
            for (int k = 1; k < N / 2; ++k)
                power[(std::size_t) k] += (double) spec[(std::size_t) (2 * k)] * spec[(std::size_t) (2 * k)]
                                        + (double) spec[(std::size_t) (2 * k + 1)] * spec[(std::size_t) (2 * k + 1)];
        }

        double num = 0.0, den = 0.0;
        const double chInv = 1.0 / std::max<std::size_t> (1, ir_.size());
        for (int k = 1; k <= N / 2; ++k)                             // DC excluded: not audio
        {
            const double f = (double) k * hostSr_ / N;
            const double w = 1.0 / (1.0 + (f / kIrRefShapeHz) * (f / kIrRefShapeHz));
            num += w * power[(std::size_t) k] * chInv;
            den += w;
        }
        const double gSq = den > 0.0 ? num / den : 0.0;
        if (gSq < std::pow (10.0, kIrRefFloorDb / 10.0))             // near-silent IR: leave it alone
            return 1.0f;
        const float gDb = (float) (-10.0 * std::log10 (gSq));
        return std::pow (10.0f, std::clamp (gDb, kIrNormMinDb, kIrNormMaxDb) / 20.0f);
    }

    double hostSr_   = 48000.0;
    int    channels_ = 2;
    float  normGain_ = 1.0f;
    float  normGainDb_ = 0.0f;
    felitronics::convolution::ConvolutionEngine<felitronics::core::fft::DefaultRealFft, 2> engine_;

    std::vector<std::vector<float>> ir_;
    std::vector<const float*>       ptrs_;
    int pendingLen_ = 0, pendingNch_ = 0;
};

} // namespace cab
