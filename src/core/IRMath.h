// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace cab
{

// Leading-silence onset, in samples: the first sample (any channel, within the first
// `numSamples`) above 0.001 x peak, minus a ~0.2 ms pre-roll so the transient's leading
// edge isn't clipped. Returns 0 when nothing meaningful (>~0.5 ms) precedes the onset.
// Shared by the IR slot (HEAD trim) and the waveform display (HEAD indicator) so the two
// can't drift apart.
inline int detectLeadingSilence (const juce::AudioBuffer<float>& buf, int numSamples, double sampleRate)
{
    const int total = numSamples;
    const int nch   = buf.getNumChannels();
    if (total <= 0 || nch <= 0)
        return 0;

    float peak = 0.0f;
    for (int ch = 0; ch < nch; ++ch)
        peak = juce::jmax (peak, buf.getMagnitude (ch, 0, total));
    if (peak <= 0.0f)
        return 0;

    const float thresh = 0.001f * peak;
    int onset = total;
    for (int ch = 0; ch < nch && onset > 0; ++ch)
    {
        const float* d = buf.getReadPointer (ch);
        for (int i = 0; i < onset; ++i)
            if (std::abs (d[i]) > thresh) { onset = i; break; }
    }

    const int preRoll = (int) (0.0002 * sampleRate);    // ~0.2 ms
    const int lead    = juce::jmax (0, onset - preRoll);
    return lead > (int) (0.0005 * sampleRate) ? lead : 0;
}

} // namespace cab
