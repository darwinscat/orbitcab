// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <vector>

namespace cab
{

// Geometry + DSP behind the IR waveform view, pulled out of the GUI component so they can
// be unit-tested without a display. Everything here is plain numbers — no juce::Rectangle /
// Graphics — so the headless (core-only) test target links them. WaveformDisplay keeps thin
// wrappers that feed in its rectangle's edges.

// --- log-frequency axis: frequency <-> horizontal pixel. The axis spans [fMin, fMax] across
// the pixel span [x0, x0 + w]. xForFreq and freqForX are inverses on that range. ---
inline float xForFreq (float f, float x0, float w, float fMin, float fMax)
{
    return x0 + w * std::log (f / fMin) / std::log (fMax / fMin);
}

inline float freqForX (float x, float x0, float w, float fMin, float fMax)
{
    const float t = juce::jlimit (0.0f, 1.0f, (x - x0) / juce::jmax (1.0f, w));
    return fMin * std::pow (fMax / fMin, t);
}

// --- 2nd-order HPF/LPF magnitude product (the shape the EQ curve draws; matches the web
// drawEqCurve). ~1.0 in the passband, ~0.707 at a corner, → 0 deep in a stopband. ---
inline float eqMagnitude (float f, bool hpfOn, float hpfHz, bool lpfOn, float lpfHz)
{
    float m = 1.0f;
    if (hpfOn) { const float r = hpfHz / f; m *= 1.0f / std::sqrt (1.0f + r * r * r * r); }
    if (lpfOn) { const float r = f / lpfHz; m *= 1.0f / std::sqrt (1.0f + r * r * r * r); }
    return m;
}

// --- magnitude (0..1) -> y within a band [y0, y0 + h]. The passband (mag = 1) sits at
// 0.62*h so the curve hints at the EQ below the waveform midline; mag = 0 runs off the
// bottom (1.30*h). ---
inline float eqCurveY (float mag, float y0, float h)
{
    const float top = y0 + h * 0.62f;   // mag = 1
    const float bot = y0 + h * 1.30f;   // mag = 0 (off-screen)
    return bot - (bot - top) * mag;
}

// --- downsample an IR into `buckets` peak magnitudes, normalised to 0..1 (the drawn
// envelope). Each bucket holds the loudest sample magnitude across all channels in its
// span. Returns all-zero on empty input. ---
inline std::vector<float> computePeaks (const juce::AudioBuffer<float>& buf, int numSamples, int buckets)
{
    std::vector<float> peaks ((size_t) juce::jmax (0, buckets), 0.0f);
    if (buckets <= 0 || numSamples <= 0 || buf.getNumChannels() <= 0)
        return peaks;

    float globalMax = 0.0f;
    for (int b = 0; b < buckets; ++b)
    {
        // Proportional bucket -> sample range so SHORT IRs (fewer samples than `buckets`)
        // stretch to fill the width instead of squashing into the left edge, and a long IR
        // is covered in full (the old `per = numSamples / buckets` truncated both: n < buckets
        // left most buckets empty, and n not a multiple dropped the tail).
        const int start = (int) ((juce::int64) b       * numSamples / buckets);
        int       end   = (int) ((juce::int64) (b + 1) * numSamples / buckets);
        if (end <= start) end = juce::jmin (numSamples, start + 1);
        float mx = 0.0f;
        for (int c = 0; c < buf.getNumChannels(); ++c)
        {
            const float* d = buf.getReadPointer (c);
            for (int k = start; k < end; ++k)
                mx = juce::jmax (mx, std::abs (d[k]));
        }
        peaks[(size_t) b] = mx;
        globalMax = juce::jmax (globalMax, mx);
    }
    if (globalMax > 0.0f)
        for (auto& v : peaks)
            v /= globalMax;

    return peaks;
}

// --- normalised peak magnitude (0..1, 1 = the IR's peak) -> a 0..1 height factor on a dB
// scale with a fixed floor. A cab IR's onset peak crushes its decay tail on a linear scale
// (the impulse reads as a tick); mapping to dB lifts the tail into view. 0 at/below the
// floor, 1 at the peak. ---
inline float dbHeightFactor (float mag01, float floorDb)
{
    const float db = juce::Decibels::gainToDecibels (mag01, floorDb);
    return juce::jlimit (0.0f, 1.0f, (db - floorDb) / (0.0f - floorDb));
}

} // namespace cab
