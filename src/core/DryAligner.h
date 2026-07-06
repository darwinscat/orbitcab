// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>   // AudioBuffer + FloatVectorOperations

namespace cab
{

//==============================================================================
// cab::DryAligner — emits the input delayed by a per-block-VARIABLE number of samples, through a
// fixed-capacity per-channel circular ring fed EVERY block (so it stays warm and the delay tap can
// change block-to-block — tube↔capture, or a model load moving the rate-match latency — with no
// reallocation and no glitch). Its job: hold a bypassed / powered-off stage at the ACTIVE stage's
// PDC, so toggling that stage's power never changes the plugin's reported latency (no host re-sync
// GAP) and an off↔active crossfade blends time-aligned signals (no comb / level JUMP).
//
// Bit-exact: the delayed sample is a pure copy of the input from exactly D samples earlier — no
// arithmetic, no interpolation. D == 0 → an exact identity copy.
//
// 🔴 RT: advance() never allocates, locks or throws — both buffers are sized once in prepare().
//==============================================================================
class DryAligner
{
public:
    // `capacity` must exceed any latency the tap will ever request (the delay is clamped to
    // [0, capacity-1]). Size it to the largest stage latency plus margin.
    void prepare (int numChannels, int maxBlock, int capacity)
    {
        const int ch = juce::jmax (1, numChannels);
        ring_.setSize    (ch, juce::jmax (2, capacity), false, false, true);
        scratch_.setSize (ch, juce::jmax (1, maxBlock), false, false, true);
        reset();
    }

    void reset() { ring_.clear(); scratch_.clear(); pos_ = 0; }

    // Stage the internal scratch = `io` delayed by `delaySamples` (0 → an exact copy) and advance the
    // ring by numSamples. Read the result via delayed(ch). `io` may be the caller's live buffer — call
    // this BEFORE any in-place stage overwrites it.
    void advance (const float* const* io, int numChannels, int numSamples, int delaySamples) noexcept
    {
        const int C = ring_.getNumSamples();
        const int D = juce::jlimit (0, C - 1, delaySamples);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* r = ring_.getWritePointer (ch);
            float* s = scratch_.getWritePointer (ch);
            int wp = pos_;
            for (int i = 0; i < numSamples; ++i)
            {
                r[wp] = io[ch][i];                  // write freshest input
                int rp = wp - D; if (rp < 0) rp += C;
                s[i] = r[rp];                       // read the sample D samples ago (D == 0 → == input)
                if (++wp >= C) wp = 0;
            }
        }
        pos_ = (pos_ + numSamples) % C;             // every channel advanced identically → one commit
    }

    const float* delayed (int channel) const noexcept { return scratch_.getReadPointer (channel); }

private:
    juce::AudioBuffer<float> ring_;      // circular delay history (capacity ≥ any stage latency)
    juce::AudioBuffer<float> scratch_;   // last advance()'s delayed output
    int pos_ = 0;                        // shared write cursor into ring_
};

} // namespace cab
