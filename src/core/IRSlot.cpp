// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "IRSlot.h"
#include "IRMath.h"

#include <juce_audio_formats/juce_audio_formats.h>   // MemoryInputStream + WAV reader for the byte-decode fallback
#include <cmath>

namespace cab
{

// Butterworth Q for the HPF/LPF — matches the old juce setResonance(0.707f) so felitronics::eq::Svf nulls
// against the juce::dsp::StateVariableTPTFilter it replaces (both are the same Zavalishin TPT SVF).
static constexpr double kFilterQ = 0.707;

//==============================================================================
// Bundled-IR fallback: decode encoded bytes here (adapter-level, JUCE) into planar samples, then hand
// them to the JUCE-free convolver. Rare — hit only when the normal sample decode failed upstream.
// Same loadIR as the normal path: the convolver normalizes every IR to reference-unity at load,
// so the old separate Normalise::yes energy-norm fallback (a DIFFERENT loudness) is gone.
void IRSlot::loadBytesFallback (const void* data, size_t size)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (
        fm.createReaderFor (std::make_unique<juce::MemoryInputStream> (data, size, false)));
    if (rd == nullptr || rd->lengthInSamples <= 0) return;
    const int n = (int) rd->lengthInSamples;
    juce::AudioBuffer<float> ir ((int) rd->numChannels, n);
    rd->read (&ir, 0, n, 0, true, true);
    conv.loadIR (ir.getArrayOfReadPointers(), ir.getNumChannels(), n, rd->sampleRate);
}

//==============================================================================
void IRSlot::prepare (double sampleRate, int maxBlock, int numChannels)
{
    conv.prepare (sampleRate, maxBlock, numChannels);

    // JUCE-free HPF/LPF (felitronics::eq::Svf — same Zavalishin TPT as juce::dsp::StateVariableTPTFilter).
    // Type + Butterworth Q are fixed; the cutoff is set per block in processWet. The Svf is per-sample, so it
    // needs no maxBlock; the initial cutoff just keeps the filter valid before the first processWet.
    hpf.prepare (sampleRate, numChannels);
    lpf.prepare (sampleRate, numChannels);
    hpf.setParams (felitronics::eq::FilterType::HighPass, 20.0,    kFilterQ, 0.0);
    lpf.setParams (felitronics::eq::FilterType::LowPass,  20000.0, kFilterQ, 0.0);

    lastTrimSamples = -1;                   // force a reload after (re)prepare
    lastHeadStart   = -1;
    prevHpfOn = prevLpfOn = false;
}

void IRSlot::reset()
{
    conv.reset();
    hpf.reset();
    lpf.reset();
    prevHpfOn = prevLpfOn = false;
}

//==============================================================================
void IRSlot::setOriginalIR (const float* const* samples, int numChannels, int numSamples, double irSr)
{
    original.setSize (numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        juce::FloatVectorOperations::copy (original.getWritePointer (ch), samples[ch], numSamples);

    irSampleRate    = irSr;
    leadSilence     = detectLeadingSilence (original, original.getNumSamples(), irSr);
    lastTrimSamples = -1;                   // new IR => force reload
    lastHeadStart   = -1;
}

void IRSlot::clearOriginal()
{
    original.setSize (0, 0);
    leadSilence     = 0;
    lastTrimSamples = -1;
    lastHeadStart   = -1;
}

double IRSlot::applyTrim (bool trimOn, float trimFraction01, bool headOn)
{
    // HEAD trim: skip the detected leading silence so the IR starts at the onset (removes
    // the cabinet pre-delay). Applied before TRIM.
    const int start = headOn ? leadSilence : 0;
    const int total = original.getNumSamples();
    if (total <= 0 || start >= total)
        return trimmedLengthSeconds();
    const int avail = total - start;        // samples from the onset to the end

    // TRIM is a fraction of the post-head length so the two stack naturally.
    const float frac  = trimOn ? trimFraction01 : 1.0f;
    const int   minLen = juce::jmax (16, (int) (0.001 * irSampleRate));     // >= ~1 ms
    const int   n = juce::jlimit (juce::jmin (minLen, avail), avail,
                                  (int) std::lround ((double) frac * avail));
    if (n == lastTrimSamples && start == lastHeadStart)   // same onset + length => nothing to do
        return trimmedLengthSeconds();
    lastTrimSamples = n;
    lastHeadStart   = start;

    juce::AudioBuffer<float> ir (original.getNumChannels(), n);
    for (int ch = 0; ch < ir.getNumChannels(); ++ch)
        ir.copyFrom (ch, 0, original, ch, start, n);

    // ~2 ms fade-out on the cut so a hard truncation doesn't leave a constant click in
    // the tail (transient crackle *while* dragging is accepted).
    const int fade = juce::jmin (n, (int) (0.002 * irSampleRate));
    if (fade > 1)
        for (int ch = 0; ch < ir.getNumChannels(); ++ch)
            ir.applyGainRamp (n - fade, fade, 1.0f, 0.0f);

    conv.loadIR (ir.getArrayOfReadPointers(), ir.getNumChannels(), n, irSampleRate);
    return (double) n / irSampleRate;
}

double IRSlot::trimmedLengthSeconds() const
{
    return (lastTrimSamples > 0 && irSampleRate > 0.0) ? (double) lastTrimSamples / irSampleRate : 0.0;
}

//==============================================================================
void IRSlot::processWet (juce::AudioBuffer<float>& wetDst, const juce::AudioBuffer<float>& src,
                         int numChannels, int numSamples, bool hpfOn, float hpfHz, bool lpfOn, float lpfHz)
{
    for (int ch = 0; ch < numChannels; ++ch)
        wetDst.copyFrom (ch, 0, src, ch, 0, numSamples);

    hpf.setParams (felitronics::eq::FilterType::HighPass, hpfHz, kFilterQ, 0.0);   // cutoff per block (coeffs only; integrator state kept)
    lpf.setParams (felitronics::eq::FilterType::LowPass,  lpfHz, kFilterQ, 0.0);

    // Re-enabling a filter after it was bypassed: clear its stale internal state so it
    // doesn't emit a transient built from the old samples.
    if (hpfOn && ! prevHpfOn) hpf.reset();
    if (lpfOn && ! prevLpfOn) lpf.reset();
    prevHpfOn = hpfOn;
    prevLpfOn = lpfOn;

    // Per-sample TPT (replaces juce's block process). flushDenormals() once per block == JUCE's internal FTZ.
    float* const* w = wetDst.getArrayOfWritePointers();
    if (hpfOn)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < numSamples; ++i) w[ch][i] = hpf.processSample (ch, w[ch][i]);
        hpf.flushDenormals();
    }
    if (lpfOn)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < numSamples; ++i) w[ch][i] = lpf.processSample (ch, w[ch][i]);
        lpf.flushDenormals();
    }
    conv.process (wetDst.getArrayOfWritePointers(), numChannels, numSamples);
}

} // namespace cab
