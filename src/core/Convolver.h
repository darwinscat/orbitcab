// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa. Part of OrbitCab — see LICENSE.

#pragma once

#include <felitronics/convolution/IrResampler.h>   // resample-on-load (message thread, one-shot)
#include <felitronics/core/Fft.h>                   // DefaultRealFft — analysis FFT for the reference-unity gain
#include <juce_dsp/juce_dsp.h>                       // juce::dsp::Convolution — the cab convolution backend

#include <algorithm>
#include <cmath>
#include <vector>

//==============================================================================
// cab::Convolver — the cab IR convolution. The backend is `juce::dsp::Convolution` (default ctor =
// zero-latency, uniform-partitioned, Apple vDSP / JUCE FFT), the fast path. The public methods speak
// raw floats + sizes, so the adapter (IRSlot) is unchanged.
//
// PERF/PROVENANCE: this is the pre-#89 convolution backend, re-instated. The interim
// felitronics::convolution::ConvolutionEngine (scalar direct-head, ~5x slower than juce at block=512)
// was a JUCE-free experiment; juce's partitioned FFT is the shipping cab DSP path. NOTE: this makes
// the CAB path JUCE-dependent again (the JUCE-free-core property is lost for the cab — an accepted
// tradeoff, tracked as felitronics-core#21 for a proper uniform-partition core convolver). The IR
// ANALYSIS below is still JUCE-free (felitronics DefaultRealFft, a message-thread one-shot).
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
// We measure the reference gain on the RESAMPLED taps, apply it, then hand the taps to juce with
// Normalise::no + at HOST rate (so juce neither re-normalizes nor resamples them — it copies them
// in verbatim). This deliberately supersedes JUCE's own gain semantics (Normalise::no's irSr/hostSr
// factor and the Normalise::yes 0.125-energy fallback). Byte-decoding stays OUT in the adapter
// (IRSlot); this header handles taps + sizes only.
//
// THREADING (juce::dsp::Convolution): loadImpulseResponse() is wait-free and safe to call from the
// message thread while process() runs on the audio thread — the heavy FFT partition build happens on
// juce's own background loader thread and the new IR is atomic-swapped in during process() with a
// 50 ms crossfade (click-free). So a fresh load never needs the felitronics-style retry-coalescing:
// flushPending()/hasPending() are sane no-ops, and isBusy() reflects "load in flight" via
// getCurrentIRSize(). Zero latency (juce default ctor) — latencySamples() == 0.
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

    // maxIrSeconds is retained for API parity (it sized the felitronics engine's cold fade). juce's
    // Convolution sizes its partitions per-IR automatically, so the argument is accepted but unused.
    void prepare (double sampleRate, int maxBlock, int numChannels, double maxIrSeconds = 4.0)
    {
        juce::ignoreUnused (maxIrSeconds);
        hostSr_   = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels_ = std::clamp (numChannels, 1, 2);
        maxBlock_ = std::max (1, maxBlock);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = hostSr_;
        spec.maximumBlockSize = (juce::uint32) maxBlock_;
        spec.numChannels      = (juce::uint32) channels_;
        convolution_.prepare (spec);                 // arms the engine; zero-latency default ctor
        pendingLen_ = 0;
    }

    void reset() { convolution_.reset(); }

    // Load an IR (JUCE Stereo::yes parity: mono broadcasts) — normalized to reference-unity, resampled
    // to host rate. Message thread (wait-free; the FFT build + atomic swap happen on juce's loader).
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
        const int nc = std::clamp (std::min (numChannels, channels_), 1, 2);
        juce::dsp::AudioBlock<float> block (io, (std::size_t) nc, (std::size_t) numSamples);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        convolution_.process (ctx);
    }

    // "A load is in flight": the async loader hasn't yet installed the requested IR as the active
    // engine. juce reports the active engine's IR length via getCurrentIRSize(); we load at host
    // rate (no juce-side resample/trim), so it equals the length we handed in once live.
    bool isBusy() const noexcept
    {
        return pendingLen_ > 0 && convolution_.getCurrentIRSize() != pendingLen_;
    }

    static constexpr int latencySamples() noexcept { return 0; }   // juce default ctor = zero latency

    // Message thread, driven by the reload poll. juce's async loader always eventually applies the
    // most recent load (it coalesces + retries internally), so there is nothing to retry here: a
    // fresh load never gets rejected the way the felitronics coalesced-swap could. Kept for API
    // parity — returns true (nothing pending), so the poll is a harmless no-op.
    bool flushPending() { return true; }
    bool hasPending() const noexcept { return false; }

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
        if (outLen <= 0) return;

        // Reference-unity normalization of the FINAL (resampled) IR — one common gain, all channels.
        const float g = normalizationGain (outLen);
        normGain_   = g;
        normGainDb_ = 20.0f * std::log10 (std::max (1.0e-6f, g));
        for (auto& ch : ir_) for (float& v : ch) v *= g;

        // Hand the normalized taps to juce AT HOST RATE (irSr == hostSr here — we already resampled),
        // Trim::no + Normalise::no so juce copies them in verbatim: no re-normalize, no re-resample.
        juce::AudioBuffer<float> buf (nch, outLen);
        for (int c = 0; c < nch; ++c)
            juce::FloatVectorOperations::copy (buf.getWritePointer (c), ir_[(std::size_t) c].data(), outLen);

        convolution_.loadImpulseResponse (std::move (buf), hostSr_,
                                          juce::dsp::Convolution::Stereo::yes,
                                          juce::dsp::Convolution::Trim::no,
                                          juce::dsp::Convolution::Normalise::no);
        pendingLen_ = outLen;
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
    int    maxBlock_ = 512;
    float  normGain_ = 1.0f;
    float  normGainDb_ = 0.0f;
    juce::dsp::Convolution convolution_;   // zero-latency, uniform-partitioned (default ctor)

    std::vector<std::vector<float>> ir_;   // staging: resampled + normalized taps (message thread)
    int pendingLen_ = 0;                   // length of the last requested IR (for isBusy)
};

} // namespace cab
