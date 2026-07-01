// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// The acceptance gate for the IRSlot de-JUCE (juce::dsp::StateVariableTPTFilter → felitronics::eq::Svf,
// juce::SmoothedValue → felitronics::core::LinearSmoother). "Sound-preserving" means "nulls against what
// ships today", so we run BOTH implementations on identical input and measure the residual — the same
// trust-the-numbers proof as the −170 dB convolution null. This test links JUCE on purpose (the reference);
// the JUCE-free analytic golden lives in felitronics-core (felitronics_eq_svf_tests).

#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <felitronics/eq/Svf.h>
#include <felitronics/core/Smoother.h>

#include <cmath>
#include <vector>

struct SvfSmootherNullTest : juce::UnitTest
{
    SvfSmootherNullTest() : juce::UnitTest ("SVF + Smoother de-JUCE null (vs JUCE)") {}

    // 10·log10( Σ(a−b)² / Σb² ) — residual energy of (felitronics − juce) relative to the juce output.
    static double nullDb (const std::vector<float>& a, const std::vector<float>& b)
    {
        double num = 0.0, den = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const double d = (double) a[i] - (double) b[i];
            num += d * d; den += (double) b[i] * (double) b[i];
        }
        return 10.0 * std::log10 ((num + 1e-40) / (den + 1e-40));
    }

    static std::vector<float> noise (int n)
    {
        std::vector<float> x ((size_t) n);
        unsigned s = 2463534242u;
        for (auto& v : x) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; v = (float) ((int) (s >> 9) - 4194304) / 4194304.0f; }
        return x;
    }

    void runTest() override
    {
        beginTest ("eq::Svf HPF/LPF nulls juce::dsp::StateVariableTPTFilter (RMS <= -120 dB) over the shipped ranges");
        {
            const double hpFc[] = { 30.0, 80.0, 200.0, 400.0 };          // OrbitCab HPF range
            const double lpFc[] = { 2000.0, 4000.0, 8000.0, 12000.0 };   // OrbitCab LPF range
            for (double sr : { 44100.0, 48000.0, 96000.0 })
            {
                for (int hp = 1; hp >= 0; --hp)
                {
                    const double* fcs = hp ? hpFc : lpFc;
                    for (int k = 0; k < 4; ++k)
                    {
                        const double fc = fcs[k];
                        const int n = 16384;
                        const auto x = noise (n);

                        juce::dsp::StateVariableTPTFilter<float> jf;
                        juce::dsp::ProcessSpec spec; spec.sampleRate = sr; spec.maximumBlockSize = (juce::uint32) n; spec.numChannels = 1;
                        jf.prepare (spec);
                        jf.setType (hp ? juce::dsp::StateVariableTPTFilterType::highpass
                                       : juce::dsp::StateVariableTPTFilterType::lowpass);
                        jf.setResonance (0.707f);
                        jf.setCutoffFrequency ((float) fc);

                        felitronics::eq::Svf ff; ff.prepare (sr, 1);
                        ff.setParams (hp ? felitronics::eq::FilterType::HighPass : felitronics::eq::FilterType::LowPass, fc, 0.707, 0.0);

                        std::vector<float> yj ((size_t) n), yf ((size_t) n);
                        for (int i = 0; i < n; ++i) { yj[(size_t) i] = jf.processSample (0, x[(size_t) i]); yf[(size_t) i] = ff.processSample (0, x[(size_t) i]); }

                        const double db = nullDb (yf, yj);
                        expect (db <= -120.0, juce::String (hp ? "HP" : "LP") + " fc=" + juce::String (fc, 0) + " sr=" + juce::String (sr, 0)
                                              + "  null RMS = " + juce::String (db, 1) + " dB (want <= -120)");
                    }
                }
            }
        }

        beginTest ("eq::Svf: a per-block cutoff SWEEP still nulls juce (coeff update, kept state)");
        {
            const double sr = 48000.0; const int n = 24000;
            const auto x = noise (n);
            juce::dsp::StateVariableTPTFilter<float> jf;
            juce::dsp::ProcessSpec spec; spec.sampleRate = sr; spec.maximumBlockSize = 64; spec.numChannels = 1; jf.prepare (spec);
            jf.setType (juce::dsp::StateVariableTPTFilterType::highpass); jf.setResonance (0.707f);
            felitronics::eq::Svf ff; ff.prepare (sr, 1);

            std::vector<float> yj ((size_t) n), yf ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                if (i % 64 == 0)                                         // move the cutoff 30 → 400 Hz across the run, per block
                {
                    const double fc = 30.0 + 370.0 * (double) i / (double) n;
                    jf.setCutoffFrequency ((float) fc);
                    ff.setParams (felitronics::eq::FilterType::HighPass, fc, 0.707, 0.0);
                }
                yj[(size_t) i] = jf.processSample (0, x[(size_t) i]);
                yf[(size_t) i] = ff.processSample (0, x[(size_t) i]);
            }
            const double db = nullDb (yf, yj);
            expect (db <= -120.0, "swept-cutoff null RMS = " + juce::String (db, 1) + " dB (want <= -120)");
        }

        beginTest ("core::LinearSmoother is bit-exact vs juce::SmoothedValue<float, Linear> (incl. 1-ULP jitter)");
        {
            juce::SmoothedValue<float>          js;
            felitronics::core::LinearSmoother   fs;
            js.reset (48000.0, 0.05); fs.reset (48000.0, 0.05);
            js.setCurrentAndTargetValue (0.0f); fs.setCurrentAndTargetValue (0.0f);

            float maxDiff = 0.0f;
            float t = 1.0f;
            const float seq[] = { 1.0f, 0.25f, -0.5f, 0.9f, 0.0f, 0.333333f };
            for (float target : seq)
            {
                js.setTargetValue (target);                  fs.setTargetValue (target);
                for (int i = 0; i < 3000; ++i) maxDiff = std::max (maxDiff, std::fabs (js.getNextValue() - fs.getNextValue()));
                // a ~1-ULP jitter on the SAME value: JUCE ignores it (approximatelyEqual) and so must we
                t = std::nextafter (target, target + 1.0f);
                js.setTargetValue (t);                       fs.setTargetValue (t);
                for (int i = 0; i < 200; ++i) maxDiff = std::max (maxDiff, std::fabs (js.getNextValue() - fs.getNextValue()));
            }
            expect (maxDiff == 0.0f, "per-sample ramp bit-identical, max |diff| = " + juce::String (maxDiff));
        }
    }
};

static SvfSmootherNullTest svfSmootherNullTest;
