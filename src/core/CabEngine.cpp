// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "CabEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>   // AudioBuffer, Decibels, FloatVectorOperations
#include <cmath>

namespace cab
{

namespace
{
    constexpr double kRampSeconds = 0.03;          // live tweaks / automation glide
}

//==============================================================================
void CabEngine::prepare (double sampleRate, int maxBlock, int numChannels, const Params& initial)
{
    currentSampleRate = sampleRate;

    preamp.prepare (sampleRate, maxBlock);
    amp.prepare (sampleRate, maxBlock);

    for (int i = 0; i < 2; ++i)
        slot[i].prepare (sampleRate, maxBlock, numChannels);

    // Seed smoothers from the initial parameter values so a restored session doesn't ramp
    // from zero on the first block.
    for (int i = 0; i < 2; ++i)
    {
        mixSm[i].reset (sampleRate, kRampSeconds);
        mixSm[i].setCurrentAndTargetValue (initial.slot[i].dryWet01);
        phaseSm[i].reset (sampleRate, kRampSeconds);
        phaseSm[i].setCurrentAndTargetValue (initial.slot[i].phase ? -1.0f : 1.0f);
    }
    gainSmoothed.reset  (sampleRate, kRampSeconds);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (initial.outputGainDb));
    mixABSmoothed.reset (sampleRate, kRampSeconds);
    mixABSmoothed.setCurrentAndTargetValue (initial.bLoaded ? initial.mixAB01 : 0.0f);
    muteGateSmoothed.reset (sampleRate, kRampSeconds);
    muteGateSmoothed.setCurrentAndTargetValue (1.0f);

    autoLeveler.prepare (sampleRate, kRampSeconds);

    inputGainPrev = juce::Decibels::decibelsToGain (initial.inputGainDb);
    inLevel.store (0.0f);
    outLevel.store (0.0f);

    dryBuffer.setSize (numChannels, maxBlock, false, false, true);
    wet[0].setSize    (numChannels, maxBlock, false, false, true);
    wet[1].setSize    (numChannels, maxBlock, false, false, true);
}

void CabEngine::reset()
{
    preamp.reset();
    amp.reset();
    for (int i = 0; i < 2; ++i)
        slot[i].reset();
    autoLeveler.reset();
}

void CabEngine::seedAutoLevel()
{
    // Estimate the convolution's broadband (white-noise) RMS gain from the loaded IR and
    // seed the leveler's makeup to ~1/gain, so the first audio starts at the converged level
    // instead of kicking while the follower crawls in (#48). The convolver is Normalise::no,
    // so that gain ≈ ‖ir‖₂ at the processing rate ≈ sqrt(SRhost / SRir) · ‖ir_native‖₂.
    auto slotGain = [this] (int s) -> double
    {
        if (! slot[s].hasOriginal())
            return 0.0;
        const auto& ir = slot[s].originalBuffer();
        const int n = ir.getNumSamples();
        if (n <= 0)
            return 0.0;
        double e = 0.0;
        for (int c = 0; c < ir.getNumChannels(); ++c)
        {
            const float* d = ir.getReadPointer (c);
            for (int i = 0; i < n; ++i)
                e += (double) d[i] * d[i];
        }
        e /= juce::jmax (1, ir.getNumChannels());            // per-channel energy
        const double srRatio = currentSampleRate / juce::jmax (1.0, slot[s].originalSampleRate());
        return std::sqrt (e * srRatio);
    };

    const double g = slotGain (0);                           // slot A — the ballpark
    if (g > 1.0e-6)
        autoLeveler.seed ((float) (1.0 / g));
}

//==============================================================================
void CabEngine::process (float* const* io, int numChannels, int numSamples,
                         const Params& p, bool nonRealtime)
{
    // Guard the prepared scratch size. A well-behaved host honours prepare()'s channel
    // count + maxBlock, but the core is now standalone (a direct/WASM/embedded caller may
    // not), so clamp to what wet[]/dryBuffer can hold — process() must never read or write
    // past the preallocated scratch. In the normal case (in==out, n<=maxBlock) this is a
    // no-op.
    numChannels = juce::jmin (numChannels, dryBuffer.getNumChannels());
    numSamples  = juce::jmin (numSamples,  dryBuffer.getNumSamples());

    // Wrap the host's planar buffers so the body reads like the original processBlock.
    juce::AudioBuffer<float> buffer (io, numChannels, numSamples);
    const int numCh = numChannels;

    // --- bypass: clean passthrough (no input trim, no processing) ---
    if (p.bypass)
    {
        inputGainPrev = juce::Decibels::decibelsToGain (p.inputGainDb);   // keep ramp continuity
        const float lvl = buffer.getMagnitude (0, numSamples);
        inLevel.store  (lvl, std::memory_order_relaxed);
        outLevel.store (lvl, std::memory_order_relaxed);
        return;
    }

    // --- input trim (block-ramped so it doesn't zipper) + input meter ---
    {
        const float igTarget = juce::Decibels::decibelsToGain (p.inputGainDb);
        buffer.applyGainRamp (0, numSamples, inputGainPrev, igTarget);
        inputGainPrev = igTarget;
        inLevel.store (buffer.getMagnitude (0, numSamples), std::memory_order_relaxed);
    }

    // --- map parameters onto the live smoothers ---
    const bool aLoaded  = p.aLoaded;   // false = slot A empty (no cab) → A contributes the dry signal
    const bool bLoaded  = p.bLoaded;
    const bool hpfOn[2] = { p.slot[0].hpfOn, p.slot[1].hpfOn };
    const bool lpfOn[2] = { p.slot[0].lpfOn, p.slot[1].lpfOn };
    for (int i = 0; i < 2; ++i)
    {
        mixSm[i].setTargetValue   (p.slot[i].dryWet01);
        phaseSm[i].setTargetValue (p.slot[i].phase ? -1.0f : 1.0f);
    }
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (p.outputGainDb));

    // MUTE: muting a slot solos the other (overrides MIX); both muted => the dry
    // signal passes through (bypass), not silence (smoothed gate, click-free).
    const bool aOn = ! p.slot[0].mute;
    const bool bOn = bLoaded && ! p.slot[1].mute;
    float abTarget = bLoaded ? p.mixAB01 : 0.0f;
    if      (! aOn) abTarget = 1.0f;     // A muted => full B
    else if (! bOn) abTarget = 0.0f;     // B muted/absent => full A
    mixABSmoothed.setTargetValue (abTarget);
    muteGateSmoothed.setTargetValue ((aOn || bOn) ? 1.0f : 0.0f);

    // --- NAM stages (PREAMP → POWERAMP): two nonlinear stages in front of the cab, in signal
    // order. They run on the signal BEFORE the dry tap, so their output becomes the "dry"
    // reference for both the per-slot Dry/Wet blend and the auto-level match — i.e. we cab the
    // amp, not the clean DI. Each is a no-op passthrough when off or no model is loaded. The
    // preamp feeds the poweramp (both load-normalised to a consistent level). ---
    if (p.preampOn)
        preamp.process (buffer.getArrayOfWritePointers(), numCh, numSamples, /*normalize*/ true);
    if (p.ampOn)
        amp.process (buffer.getArrayOfWritePointers(), numCh, numSamples, /*normalize*/ true);

    // --- stash the dry (now post-amp) signal for the per-slot Dry/Wet blend, before the filters ---
    for (int ch = 0; ch < numCh; ++ch)
        dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    // --- per-slot wet path: HPF -> LPF -> Convolution (into wet[0] / wet[1]) ---
    // Skip a slot's convolution when it's empty — its branch falls back to the dry signal
    // below (so an empty slot = "no cab", a clean passthrough, not silence).
    if (aLoaded)
        slot[0].processWet (wet[0], buffer, numCh, numSamples, hpfOn[0], p.slot[0].hpfHz, lpfOn[0], p.slot[0].lpfHz);
    if (bLoaded)
        slot[1].processWet (wet[1], buffer, numCh, numSamples, hpfOn[1], p.slot[1].hpfHz, lpfOn[1], p.slot[1].lpfHz);

    // --- per-slot Phase + Dry/Wet, then A<->B crossfade -> buffer (the mix) ---
    {
        const auto* const* dry = dryBuffer.getArrayOfReadPointers();
        const auto* const* wa  = wet[0].getArrayOfReadPointers();
        const auto* const* wb  = wet[1].getArrayOfReadPointers();
        auto* const*       out = buffer.getArrayOfWritePointers();
        for (int n = 0; n < numSamples; ++n)
        {
            const float phA = phaseSm[0].getNextValue(), mA = mixSm[0].getNextValue();
            const float phB = phaseSm[1].getNextValue(), mB = mixSm[1].getNextValue();
            const float ab   = mixABSmoothed.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float d  = dry[ch][n];
                const float sA = aLoaded ? d * (1.0f - mA) + wa[ch][n] * phA * mA : d;   // empty A → dry
                const float sB = bLoaded ? d * (1.0f - mB) + wb[ch][n] * phB * mB : sA;
                // MUTE gate is applied AFTER auto-level (#45) so the leveler measures the
                // *ungated* wet — its makeup stays put through a toggle instead of lagging
                // ~1 s behind the gate (that lag caused the mute dip + un-mute overshoot).
                out[ch][n] = bLoaded ? (sA * (1.0f - ab) + sB * ab) : sA;
            }
        }
    }

    // --- auto-level (global on the final mix): measure the dry + mixed mean-square,
    // hand it to the follower, which returns the per-sample makeup gain. ---
    {
        double dMS = 0.0, mMS = 0.0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float dr = dryBuffer.getRMSLevel (ch, 0, numSamples);
            const float mr = buffer.getRMSLevel (ch, 0, numSamples);
            dMS += (double) dr * dr;
            mMS += (double) mr * mr;
        }
        const int chs = juce::jmax (1, numCh);
        autoLeveler.processBlock (dMS / chs, mMS / chs, p.autoLevel, numSamples);
    }

    // --- auto-level makeup (wet only), then the MUTE gate (→ dry), then master gain ---
    // The leveler measured the ungated wet above, so its makeup is steady across a mute.
    // Here the wet is leveled and crossfaded to the raw (un-leveled) dry by the gate: the
    // dry passes clean (like bypass) and the leveled wet is already matched → no dip / poof.
    {
        const auto* const* dry = dryBuffer.getArrayOfReadPointers();
        auto* const*       out = buffer.getArrayOfWritePointers();
        for (int n = 0; n < numSamples; ++n)
        {
            const float g    = gainSmoothed.getNextValue();
            const float mg   = autoLeveler.getNextGain();
            const float gate = muteGateSmoothed.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
                out[ch][n] = (out[ch][n] * mg * gate + dry[ch][n] * (1.0f - gate)) * g;
        }
    }

    // --- output meter (post everything) ---
    outLevel.store (buffer.getMagnitude (0, numSamples), std::memory_order_relaxed);

    // --- feed the spectrum taps (pre = dry input, post = output) ---
    if (spectrumActive.load (std::memory_order_relaxed) && ! nonRealtime)
    {
        const auto* const* dry = dryBuffer.getArrayOfReadPointers();
        const auto* const* out = buffer.getArrayOfReadPointers();
        const float inv = 1.0f / (float) juce::jmax (1, numCh);
        for (int n = 0; n < numSamples; ++n)
        {
            float pre = 0.0f, post = 0.0f;
            for (int ch = 0; ch < numCh; ++ch) { pre += dry[ch][n]; post += out[ch][n]; }
            preTap.push  (pre  * inv);
            postTap.push (post * inv);
        }
    }
}

//==============================================================================
void CabEngine::setSlotOriginalIR (int s, const float* const* samples, int numChannels,
                                   int numSamples, double irSampleRate)
{
    slot[s].setOriginalIR (samples, numChannels, numSamples, irSampleRate);
}

double CabEngine::slotApplyTrim (int s, bool trimOn, float trimFraction01, bool headOn)
{
    return slot[s].applyTrim (trimOn, trimFraction01, headOn);
}

void CabEngine::slotLoadBytesFallback (int s, const void* data, size_t size)
{
    slot[s].loadBytesFallback (data, size);
}

void CabEngine::clearSlotOriginal (int s)           { slot[s].clearOriginal(); }
bool CabEngine::slotHasOriginal   (int s) const     { return slot[s].hasOriginal(); }
double CabEngine::slotTrimmedSeconds (int s) const  { return slot[s].trimmedLengthSeconds(); }

const juce::AudioBuffer<float>& CabEngine::slotOriginal (int s) const { return slot[s].originalBuffer(); }
double CabEngine::slotOriginalSampleRate (int s) const               { return slot[s].originalSampleRate(); }

bool CabEngine::pullSpectrum (bool pre, float* destFftSize)
{
    auto& tap = pre ? preTap : postTap;
    if (! tap.ready.load (std::memory_order_acquire))
        return false;
    juce::FloatVectorOperations::copy (destFftSize, tap.data, fftSize);
    tap.ready.store (false, std::memory_order_release);
    return true;
}

} // namespace cab
