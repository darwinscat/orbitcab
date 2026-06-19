// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>   // FloatVectorOperations
#include <atomic>

//==============================================================================
// cab::SpectrumTap — one mono capture window with an atomic ready handshake (SPSC).
// The audio thread pushes samples; once a window fills, it snapshots into `data`
// (unless the reader hasn't consumed the previous one yet) and flags ready. The
// GUI's analyser pulls the latest frame and runs the display FFT itself.
//
// RT-safe: only fixed-size copies, no alloc/lock. The FFT size is a shared contract
// between the audio-side tap and the GUI-side FFT, so it lives here in the core.
//==============================================================================
namespace cab
{

constexpr int kSpectrumFftOrder = 11;                  // 2048-point
constexpr int kSpectrumFftSize  = 1 << kSpectrumFftOrder;

struct SpectrumTap
{
    float fifo[kSpectrumFftSize] {};
    float data[kSpectrumFftSize] {};
    int   idx = 0;
    std::atomic<bool> ready { false };

    void push (float s) noexcept
    {
        if (idx == kSpectrumFftSize)
        {
            if (! ready.load (std::memory_order_acquire))
            {
                juce::FloatVectorOperations::copy (data, fifo, kSpectrumFftSize);
                ready.store (true, std::memory_order_release);
            }
            idx = 0;
        }
        fifo[idx++] = s;
    }
};

} // namespace cab
