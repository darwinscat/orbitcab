// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cstddef>

//==============================================================================
// cab::Convolver — the convolution behind a JUCE-FREE signature.
//
// This is the seam that makes the math portable:
// the public methods speak raw floats + sizes, and `juce::dsp::Convolution` (the
// off-thread partitioned FFT + atomic swap that obeys the 🔴 RT rule) is hidden in
// the implementation. It is a *concrete* class on purpose — there's one backend
// today, so no abstract interface yet (YAGNI).
//==============================================================================
namespace cab
{

class Convolver
{
public:
    void prepare (double sampleRate, int maxBlock, int numChannels)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = (juce::uint32) maxBlock;
        spec.numChannels      = (juce::uint32) numChannels;
        conv.prepare (spec);
    }

    void reset() { conv.reset(); }

    // Active IR length once JUCE's async load has swapped it in (0 while still loading). For tests/UI.
    int currentIrSize() const noexcept { return (int) conv.getCurrentIRSize(); }

    // Load a (planar) IR. The heavy FFT prep + atomic swap run on JUCE's loader
    // thread — RT-safe — so this is callable from the message thread. Loaded AS-IS
    // (Normalise::no); the engine's auto-level does the leveling. Stereo::yes => a
    // mono IR is applied to L and R alike.
    void loadIR (const float* const* samples, int numChannels, int numSamples, double irSampleRate)
    {
        juce::AudioBuffer<float> ir (numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy (ir.getWritePointer (ch), samples[ch], numSamples);

        conv.loadImpulseResponse (std::move (ir), irSampleRate,
                                  juce::dsp::Convolution::Stereo::yes,
                                  juce::dsp::Convolution::Trim::no,
                                  juce::dsp::Convolution::Normalise::no);
    }

    // Fallback: load directly from encoded bytes with JUCE's own normalisation
    // (used only if decoding the bundled IR to samples fails — see loadBundledIR).
    void loadIRBytes (const void* data, size_t size)
    {
        conv.loadImpulseResponse (data, size,
                                  juce::dsp::Convolution::Stereo::yes,
                                  juce::dsp::Convolution::Trim::no, 0,
                                  juce::dsp::Convolution::Normalise::yes);
    }

    // RT-safe in-place convolution of `numChannels` planar channels.
    void process (float* const* io, int numChannels, int numSamples)
    {
        juce::dsp::AudioBlock<float> blk (io, (size_t) numChannels, (size_t) numSamples);
        juce::dsp::ProcessContextReplacing<float> ctx (blk);
        conv.process (ctx);
    }

private:
    juce::dsp::Convolution conv;
};

} // namespace cab
