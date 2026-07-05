// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko & Alisa. Part of OrbitCab — see LICENSE.

#pragma once

#include <felitronics/convolution/MatrixConvolverNupc.h>  // JUCE-free cab convolution backend (non-uniform/Gardner)
#include <felitronics/convolution/IrResampler.h>          // resample-on-load (message thread, one-shot)
#include <felitronics/core/Fft.h>                          // DefaultRealFft — analysis FFT + the scalar fallback backend

#if defined(FELITRONICS_WITH_PFFFT)
 #include <felitronics/fftpffft/PffftRealFft.h>            // SIMD (z-order) audio FFT — the desktop-speed backend
#endif

#include <algorithm>
#include <cmath>
#include <vector>

//==============================================================================
// cab::Convolver — the cab IR convolution. The backend is `felitronics::convolution::MatrixConvolverNupc`
// (non-uniform / Gardner partition schedule, block-INDEPENDENT, TRUE sample-zero-latency), the JUCE-free
// cab DSP path. The public methods speak raw floats + sizes, so the adapter (IRSlot) is unchanged.
//
// PERF/PROVENANCE: this replaces the interim `juce::dsp::Convolution` backend (felitronics-core#21). NUPC
// keeps the head + doubling stages warm every sample so a live cabinet swap is click-free, yet its spectral
// MACs scale with the ACTUAL IR length (activeParts), so a short cab IR (<1 s of taps — the norm) stays
// cheap (<1% RT). The audio FFT is pffft (SIMD, BSD) when FELITRONICS_WITH_PFFFT is set, else the scalar
// reference. With this swap the whole cab path is JUCE-free (only the plugin shell / adapters touch JUCE).
//
// IR-LENGTH CAP: unlike juce (which sized its partitions per-IR), NUPC builds a FIXED schedule from
// maxIrSamples at prepare() time. We cap at maxIrSeconds of taps (kNupcHeadPartition head + one uniform
// tail stage): covers every real cabinet IR with headroom while keeping RAM sane. An IR longer than the cap
// convolves only its first maxIrSeconds — a deliberate product cap (cab IRs are <1 s; longer is off-label).
//
// LOUDNESS: every IR is normalized AT LOAD to reference-unity — unity RMS gain for a guitar-band
// reference signal (white noise through a one-pole low-pass at kIrRefShapeHz), computed in the
// frequency domain from the FINAL IR (post-trim, post-resample) and applied as ONE common gain
// across channels (stereo imaging untouched). Vendor cab IRs are peak-normalized bandpasses, so
// their passband sits +13…+18 dB above unity (measured across the factory set) — convolving them
// RAW made the wet path that much hotter than the dry, pushed the output past the ±24 dB trim range
// with auto-level off, and got worse the quieter the input. After normalization a cab contributes
// TONE, not gain: the auto-leveler shrinks to a small signal-dependent corrector and the plugin is
// usable with it off. The normalized taps are handed to the convolver verbatim (NUPC applies no gain
// of its own), so our reference-unity gain is the single, exact loudness contract.
//
// THREADING: loadIR() runs on the message thread (resample + FFT-domain gain + the convolver's partition
// build all allocate). MatrixConvolverNupc::setIr()/setOperator() build synchronously into the INACTIVE
// slot then publish; process() picks up the new operator and crossfades it in over ~50 ms (click-free), so
// a live swap never touches the audio thread's active operator. While a crossfade is in flight the convolver
// REJECTS a new load (returns false) — we retain the staged taps and retry on the reload poll (flushPending),
// latest-wins (coalescing). Zero latency — latencySamples() == 0.
//==============================================================================
namespace cab
{

// Audio-path real-FFT backend for the convolver: pffft (SIMD, z-order) on desktop when opted in, else the
// header-only scalar reference. The choice MUST be uniform across every TU that sees this header (the two
// instantiations are distinct types) — it is a single target-wide compile definition.
#if defined(FELITRONICS_WITH_PFFFT)
using CabConvFft = felitronics::fftpffft::PffftRealFft;
#else
using CabConvFft = felitronics::core::fft::DefaultRealFft;
#endif

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

    // maxIrSeconds sizes the NUPC partition schedule (the fixed IR-length cap — see IR-LENGTH CAP above).
    // The default matches the historical value; cab IRs are far shorter, so the cap is generous headroom.
    void prepare (double sampleRate, int maxBlock, int numChannels, double maxIrSeconds = 4.0)
    {
        hostSr_   = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels_ = std::clamp (numChannels, 1, 2);
        maxBlock_ = std::max (1, maxBlock);                 // retained for API parity — NUPC is block-independent

        // Fixed schedule: head kNupcHeadPartition + doubling + one uniform tail covering maxIrSamples.
        const long long maxIr = (long long) std::ceil (std::max (0.0, maxIrSeconds) * hostSr_);
        const int maxIrSamples = (int) std::min<long long> (maxIr, (long long) Conv::kMaxIrSamples);
        // juce swapped IRs over a 50 ms crossfade — match it (the migration spec's verified parity value).
        const int crossfadeSamples = std::max (1, (int) std::lround (0.05 * hostSr_));

        prepared_     = convolution_.prepare (kNupcHeadPartition, maxIrSamples, crossfadeSamples, channels_);
        pendingRetry_ = false;
        pendingLen_   = 0;
        pendingNch_   = 0;
    }

    void reset() { convolution_.reset(); }

    // Load an IR (mono broadcasts to both channels — juce Stereo::yes parity) — normalized to reference-unity,
    // resampled to host rate. Message thread: resample + gain + the convolver's partition build all allocate.
    void loadIR (const float* const* samples, int numChannels, int numSamples, double irSampleRate)
    {
        buildAndStage (samples, numChannels, numSamples, irSampleRate);
    }

    // The normalization applied to the last loaded IR — the exact linear gain (for references
    // that must null against the engine) and its dB reading (diagnostics). Message thread.
    float irNormalizationGain()   const noexcept { return normGain_; }
    float irNormalizationGainDb() const noexcept { return normGainDb_; }

    // RT-safe in-place convolution of `numChannels` planar channels. NUPC processes the prepared channel
    // count and no-ops if handed fewer planes — callers pass the prepared width (wet buffer == bus width).
    void process (float* const* io, int numChannels, int numSamples)
    {
        convolution_.process (io, io, numChannels, numSamples);   // in-place (in == out), zero latency
    }

    // "A load is in flight": either the async crossfade hasn't finished, or a load is still queued because
    // the convolver rejected it mid-crossfade (retried by flushPending).
    bool isBusy() const noexcept { return pendingRetry_ || convolution_.isBusy(); }

    static constexpr int latencySamples() noexcept { return 0; }   // NUPC guarantees sample-zero latency

    // Message thread, driven by the reload poll (IRSlot::pumpReload). If a load was rejected while the
    // convolver was mid-crossfade, retry it now from the retained taps. Returns true when nothing is pending.
    bool flushPending() { return tryPublishPending(); }
    bool hasPending() const noexcept { return pendingRetry_; }

private:
    using Conv = felitronics::convolution::MatrixConvolverNupc<CabConvFft>;
    static constexpr int kNupcHeadPartition = 128;   // time-domain head P0 (pow2) — lineareq uses the same

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

        // Publish the normalized taps to the convolver. On rejection (mid-crossfade) the retry reads back
        // from ir_ — which always holds the LATEST staged IR — so there is no dangling snapshot: a newer
        // load simply overwrites ir_ and the pending state, and flushPending applies the latest (coalescing).
        pendingNch_   = nch;
        pendingLen_   = outLen;
        pendingRetry_ = true;
        tryPublishPending();
    }

    // Hand the currently-staged IR (in ir_) to the convolver. Cheap on rejection: the convolver's state
    // check happens BEFORE any partition build, so a mid-crossfade reject does no work. Message thread.
    bool tryPublishPending()
    {
        if (! pendingRetry_) return true;
        if (! prepared_)     return false;   // convolver failed to arm (impossible with our clamps) — keep pending

        bool ok;
        if (pendingNch_ <= 1)
            ok = convolution_.setIr (ir_[0].data(), pendingLen_);                 // mono → broadcast (Stereo::yes)
        else
        {
            const float* banks[2] { ir_[0].data(), ir_[1].data() };
            ok = convolution_.setOperator (Conv::Topology::LRDiag, banks, 2, pendingLen_);   // true stereo
        }
        if (ok) pendingRetry_ = false;
        return ! pendingRetry_;
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
    bool   prepared_ = false;
    Conv   convolution_;                   // non-uniform (Gardner), block-independent, zero-latency

    std::vector<std::vector<float>> ir_;   // staging + retained latest taps (message thread)
    int  pendingNch_   = 0;                // channels of the staged IR (1 = mono broadcast, 2 = true stereo)
    int  pendingLen_   = 0;                // length of the staged IR
    bool pendingRetry_ = false;            // a staged IR still needs publishing (rejected mid-crossfade)
};

} // namespace cab
