// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_dsp/juce_dsp.h>
#include "../PluginProcessor.h"   // fftOrder/fftSize + pullSpectrumFrame + getSampleRate

#include <array>
#include <vector>
#include <cmath>

//==============================================================================
// SpectrumAnalyser — the display-side spectrum (the GUI half of the pre/post analyser).
// Pulls the latest capture windows from the processor's lock-free taps and turns them
// into smoothed log-frequency bins for the waveform overlay. Pure graphics-side DSP:
// runs on the editor's 30 Hz timer, never the audio thread (the audio thread only fills
// the taps — boundary rule).
//==============================================================================
class SpectrumAnalyser
{
public:
    SpectrumAnalyser() { clear(); }

    // Pull a fresh pre/post frame (if ready) and advance the smoothed bins.
    void update (OrbitCabAudioProcessor& proc)
    {
        const double sr = proc.getSampleRate();
        if (kBins < 2 || sr <= 0.0)
            return;
        compute (proc, true,  preBins,  sr);
        compute (proc, false, postBins, sr);
    }

    void clear()
    {
        preBins.assign  ((size_t) kBins, 0.0f);
        postBins.assign ((size_t) kBins, 0.0f);
        specRaw.assign  ((size_t) kBins, 0.0f);
    }

    const std::vector<float>& pre()  const { return preBins; }
    const std::vector<float>& post() const { return postBins; }

private:
    static constexpr int kBins = 160;

    void compute (OrbitCabAudioProcessor& proc, bool pre, std::vector<float>& bins, double sr)
    {
        constexpr int   fftSize = OrbitCabAudioProcessor::fftSize;
        constexpr float fMin = 20.0f, fMax = 20000.0f;
        const float ref = juce::Decibels::gainToDecibels ((float) fftSize);

        if (! proc.pullSpectrumFrame (pre, fftData.data()))
        {
            for (auto& v : bins) v *= 0.92f;            // no new frame → settle
            return;
        }
        window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const int N = kBins;
        // raw per-bin level (log-freq)
        for (int b = 0; b < N; ++b)
        {
            const float f   = fMin * std::pow (fMax / fMin, (float) b / (float) (N - 1));
            const int   bin = juce::jlimit (0, fftSize / 2, (int) std::round (f * (float) fftSize / (float) sr));
            const float dB  = juce::Decibels::gainToDecibels (fftData[(size_t) bin]) - ref;
            specRaw[(size_t) b] = juce::jlimit (0.0f, 1.0f, juce::jmap (dB, -90.0f, 0.0f, 0.0f, 1.0f));
        }
        // smooth envelope (moving average, radius 2) + inertial glide toward it
        for (int b = 0; b < N; ++b)
        {
            float sum = 0.0f; int cnt = 0;
            for (int k = -2; k <= 2; ++k)
            {
                const int i = b + k;
                if (i >= 0 && i < N) { sum += specRaw[(size_t) i]; ++cnt; }
            }
            const float smoothed = sum / (float) cnt;
            bins[(size_t) b] += (smoothed - bins[(size_t) b]) * 0.35f;
        }
    }

    juce::dsp::FFT fft { OrbitCabAudioProcessor::fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) OrbitCabAudioProcessor::fftSize,
                                                 juce::dsp::WindowingFunction<float>::hann };
    std::array<float, 2 * OrbitCabAudioProcessor::fftSize> fftData {};
    std::vector<float> preBins, postBins, specRaw;
};
