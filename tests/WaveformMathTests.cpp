// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for cab::WaveformMath — the geometry + envelope math behind the IR
// waveform view, extracted from the GUI component so it's testable without a display.
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "core/WaveformMath.h"

#include <cmath>
#include <vector>

using namespace cab;

struct WaveformMathTest : juce::UnitTest
{
    WaveformMathTest() : juce::UnitTest ("WaveformMath") {}

    // The display's real axis range.
    static constexpr float fMin = 20.0f, fMax = 20000.0f;
    static constexpr float x0 = 10.0f, w = 600.0f;

    void runTest() override
    {
        //--------------------------------------------------------------------
        beginTest ("freq<->x are inverses across the range");
        {
            for (const float f : { 20.0f, 50.0f, 100.0f, 440.0f, 1000.0f, 5000.0f, 12000.0f, 20000.0f })
            {
                const float x  = xForFreq (f, x0, w, fMin, fMax);
                const float f2 = freqForX (x, x0, w, fMin, fMax);
                expectWithinAbsoluteError<float> (f2, f, f * 0.001f);   // <0.1% round-trip
            }
        }

        beginTest ("x axis: endpoints land on the rectangle edges, monotonic in between");
        {
            expectWithinAbsoluteError<float> (xForFreq (fMin, x0, w, fMin, fMax), x0,     0.01f);
            expectWithinAbsoluteError<float> (xForFreq (fMax, x0, w, fMin, fMax), x0 + w, 0.01f);

            float prev = -1.0e9f;
            for (const float f : { 20.0f, 200.0f, 2000.0f, 20000.0f })
            {
                const float x = xForFreq (f, x0, w, fMin, fMax);
                expect (x > prev, "xForFreq must increase with frequency");
                prev = x;
            }
        }

        beginTest ("freqForX clamps outside the rectangle");
        {
            expectWithinAbsoluteError<float> (freqForX (x0 - 100.0f, x0, w, fMin, fMax), fMin, fMin * 0.001f);
            expectWithinAbsoluteError<float> (freqForX (x0 + w + 100.0f, x0, w, fMin, fMax), fMax, fMax * 0.001f);
        }

        beginTest ("freqForX tolerates zero width (no divide-by-zero)");
        {
            const float f = freqForX (x0, x0, 0.0f, fMin, fMax);
            expect (std::isfinite (f), "freqForX must stay finite at w=0");
        }

        //--------------------------------------------------------------------
        beginTest ("eqMagnitude: unity passband, ~-3 dB at corners, deep stopbands");
        {
            // No filters → flat unity everywhere.
            expectWithinAbsoluteError<float> (eqMagnitude (1000.0f, false, 80.0f, false, 8000.0f), 1.0f, 1.0e-6f);

            // At a filter's corner, magnitude is 1/sqrt(2) ≈ 0.707 (−3 dB).
            expectWithinAbsoluteError<float> (eqMagnitude (80.0f,   true, 80.0f, false, 8000.0f), 0.70710678f, 0.001f);
            expectWithinAbsoluteError<float> (eqMagnitude (8000.0f, false, 80.0f, true, 8000.0f), 0.70710678f, 0.001f);

            // HPF passes highs, cuts lows; LPF the opposite.
            expect (eqMagnitude (20.0f,    true, 200.0f, false, 8000.0f) < 0.1f,  "HPF must strongly cut well below corner");
            expect (eqMagnitude (5000.0f,  true, 200.0f, false, 8000.0f) > 0.95f, "HPF must pass well above corner");
            expect (eqMagnitude (18000.0f, false, 200.0f, true, 4000.0f) < 0.1f,  "LPF must strongly cut well above corner");
            expect (eqMagnitude (200.0f,   false, 200.0f, true, 4000.0f) > 0.95f, "LPF must pass well below corner");
        }

        beginTest ("eqMagnitude: both filters multiply (a mid-pass window)");
        {
            const float mid  = eqMagnitude (1000.0f, true, 100.0f, true, 8000.0f);
            const float low  = eqMagnitude (30.0f,   true, 100.0f, true, 8000.0f);
            const float high = eqMagnitude (16000.0f, true, 100.0f, true, 8000.0f);
            expect (mid > 0.95f, "passband between the corners stays near unity");
            expect (low  < mid,  "below the HPF corner is attenuated");
            expect (high < mid,  "above the LPF corner is attenuated");
        }

        //--------------------------------------------------------------------
        beginTest ("eqCurveY: passband sits inside the band, mag=0 runs off the bottom, monotonic");
        {
            const float y0 = 0.0f, h = 100.0f;
            const float yPass = eqCurveY (1.0f, y0, h);
            const float yZero = eqCurveY (0.0f, y0, h);
            expectWithinAbsoluteError<float> (yPass, 62.0f,  0.01f);   // 0.62*h
            expectWithinAbsoluteError<float> (yZero, 130.0f, 0.01f);   // 1.30*h (off-screen)
            expect (yZero > yPass, "lower magnitude must draw lower (larger y)");
            expect (eqCurveY (0.5f, y0, h) > yPass && eqCurveY (0.5f, y0, h) < yZero, "mid magnitude lies between");
        }

        //--------------------------------------------------------------------
        beginTest ("computePeaks: normalised to 0..1 with a real maximum of 1");
        {
            juce::AudioBuffer<float> buf (1, 1000);
            auto* d = buf.getWritePointer (0);
            for (int i = 0; i < 1000; ++i)
                d[i] = 0.25f * std::sin (juce::MathConstants<float>::twoPi * (float) i / 50.0f);

            const auto peaks = computePeaks (buf, 1000, 64);
            expectEquals ((int) peaks.size(), 64);
            float mx = 0.0f, mn = 1.0e9f;
            for (auto v : peaks) { mx = juce::jmax (mx, v); mn = juce::jmin (mn, v); }
            expectWithinAbsoluteError<float> (mx, 1.0f, 1.0e-5f);   // normalised peak hits 1
            expect (mn >= 0.0f, "peaks are magnitudes (non-negative)");
        }

        beginTest ("computePeaks: an early impulse normalises to a leading spike");
        {
            juce::AudioBuffer<float> buf (1, 1024);
            buf.clear();
            buf.getWritePointer (0)[3] = 1.0f;     // lone spike near the start

            const auto peaks = computePeaks (buf, 1024, 128);
            expectWithinAbsoluteError<float> (peaks.front(), 1.0f, 1.0e-5f);
            for (size_t i = 1; i < peaks.size(); ++i)
                expectWithinAbsoluteError<float> (peaks[i], 0.0f, 1.0e-5f);
        }

        beginTest ("computePeaks: silence stays all-zero (no divide-by-zero normalisation)");
        {
            juce::AudioBuffer<float> buf (2, 500);
            buf.clear();
            const auto peaks = computePeaks (buf, 500, 32);
            expectEquals ((int) peaks.size(), 32);
            for (auto v : peaks)
                expectWithinAbsoluteError<float> (v, 0.0f, 0.0f);
        }

        beginTest ("computePeaks: takes the max across channels");
        {
            juce::AudioBuffer<float> buf (2, 200);
            buf.clear();
            buf.getWritePointer (0)[10] = 0.3f;    // quiet left
            buf.getWritePointer (1)[10] = 0.9f;    // loud right
            const auto peaks = computePeaks (buf, 200, 200);   // 1 sample/bucket
            expectWithinAbsoluteError<float> (peaks[10], 1.0f, 1.0e-5f);   // 0.9 is the global max → 1.0
        }

        beginTest ("computePeaks: degenerate inputs return an empty/zero envelope");
        {
            juce::AudioBuffer<float> buf (1, 100);
            buf.clear();
            expect (computePeaks (buf, 0,   64).size() == 64, "zero samples → 64 zeros");
            expect (computePeaks (buf, 100,  0).empty(),       "zero buckets → empty");
            const auto z = computePeaks (buf, 0, 64);
            for (auto v : z) expectWithinAbsoluteError<float> (v, 0.0f, 0.0f);
        }

        beginTest ("computePeaks: a short IR (fewer samples than buckets) stretches to fill, not squashes left");
        {
            // 4 samples, 16 buckets, peak on the LAST sample. The old `per = n/buckets` left the
            // spike in bucket 3 with buckets 4..15 empty; proportional bucketing must spread the
            // 4 samples across the whole width so the peak lands at the far right.
            juce::AudioBuffer<float> buf (1, 4);
            buf.clear();
            buf.getWritePointer (0)[3] = 1.0f;
            const auto peaks = computePeaks (buf, 4, 16);
            expectEquals ((int) peaks.size(), 16);
            float maxRight = 0.0f;
            for (int i = 12; i < 16; ++i) maxRight = juce::jmax (maxRight, peaks[(size_t) i]);
            expectWithinAbsoluteError<float> (maxRight, 1.0f, 1.0e-5f);          // spike at the right quarter
            expect (peaks[3] < 0.5f, "the old code's bucket-3 must no longer hold the spike");
        }

        beginTest ("dbHeightFactor: floor→0, peak→1, monotonic, lifts the decay tail");
        {
            expectWithinAbsoluteError<float> (dbHeightFactor (1.0f, -60.0f), 1.0f, 1.0e-5f);   // 0 dB → 1
            expectWithinAbsoluteError<float> (dbHeightFactor (0.0f, -60.0f), 0.0f, 1.0e-5f);   // silence → floor → 0
            expectWithinAbsoluteError<float> (dbHeightFactor (juce::Decibels::decibelsToGain (-30.0f), -60.0f),
                                              0.5f, 0.01f);                                    // -30 dB → mid
            expect (dbHeightFactor (0.01f, -60.0f) > 0.3f, "a -40 dB tail lifts well above the floor (linear ~0.01)");
        }
    }
};

static WaveformMathTest waveformMathTest;
