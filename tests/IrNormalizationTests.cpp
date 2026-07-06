// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// The user-facing ACCEPTANCE contract of the load-time IR normalization (see Convolver.h):
// through the WHOLE engine (cab only, auto-level OFF), a factory cab must pass the reference
// stimulus at ~unity gain. Before the normalization the same path measured +13…+15 dB (peak-
// normalized vendor IRs convolved raw) — the reason the output trim ran out of its ±24 dB range.
//
// Also pinned here: trim re-normalizes for free (the trim path reloads the convolver), the
// resample path measures the FINAL IR (a 48 kHz IR on a 96 kHz host still lands at unity — the
// old JUCE gain semantics made that case +6 dB hotter still), seedAutoLevel seeds unity, and the
// factory set lands CONSISTENT with itself (browsing compares tone, not loudness).
#include "core/CabEngine.h"
#include "core/Convolver.h"
#include "core/Params.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

using namespace cab;

namespace
{
    // The reference stimulus the normalization targets: LCG white noise through a one-pole
    // low-pass at Convolver::kIrRefShapeHz, level-calibrated analytically (same recipe as the
    // NAM-stage level probes — "unity" means the same thing plugin-wide).
    std::vector<float> referenceStimulus (int numSamples, double sr, float rmsDb = -18.0f)
    {
        const double a = 1.0 - std::exp (-juce::MathConstants<double>::twoPi * Convolver::kIrRefShapeHz / sr);
        const double whiteRms = 1.0 / std::sqrt (3.0);
        const double shapedRms = whiteRms * std::sqrt (a / (2.0 - a));
        const float g = (float) (std::pow (10.0, rmsDb / 20.0) / shapedRms);
        std::vector<float> out ((size_t) numSamples);
        float lp = 0.0f;
        for (int n = 0; n < numSamples; ++n)
        {
            juce::uint32 s = (juce::uint32) ((long long) n * 2654435761u + 1013904223u);
            s ^= s >> 15; s *= 2246822519u; s ^= s >> 13;
            const float w = ((float) ((double) s / 4294967296.0) - 0.5f) * 2.0f;
            lp += (float) a * (w - lp);
            out[(size_t) n] = lp * g;
        }
        return out;
    }

    juce::AudioBuffer<float> loadIRFile (const juce::String& name, double& srOut)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        const juce::File f = juce::File (ORBITCAB_RES_DIR).getChildFile ("ir").getChildFile (name);
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
        if (rd == nullptr) { srOut = 0.0; return {}; }
        srOut = rd->sampleRate;
        juce::AudioBuffer<float> ir ((int) rd->numChannels, (int) rd->lengthInSamples);
        rd->read (&ir, 0, (int) rd->lengthInSamples, 0, true, true);
        return ir;
    }
}

struct IrNormalizationTest : juce::UnitTest
{
    IrNormalizationTest() : juce::UnitTest ("IR normalization (reference-unity at load)") {}

    static constexpr int kBlk = 512;

    // Feed the reference stimulus through a cab-only engine (auto-level off, full wet) and
    // return the measured through-gain in dB. Pumps the async convolver build first.
    float throughGainDb (CabEngine& e, Params& p, double sr)
    {
        {   // pump until the off-thread convolver build is live
            juce::AudioBuffer<float> warm (2, kBlk);
            for (int i = 0; i < 40; ++i)
            {
                warm.clear();
                e.process (warm.getArrayOfWritePointers(), 2, kBlk, p, false);
                juce::Thread::sleep (8);
            }
        }
        const int total = (int) (1.5 * sr), skip = (int) (0.5 * sr);
        const auto stim = referenceStimulus (total, sr);
        juce::AudioBuffer<float> buf (2, kBlk);
        double si = 0.0, so = 0.0;
        for (int off = 0; off + kBlk <= total; off += kBlk)
        {
            for (int n = 0; n < kBlk; ++n) { const float x = stim[(size_t) (off + n)]; buf.setSample (0, n, x); buf.setSample (1, n, x); }
            if (off >= skip) for (int n = 0; n < kBlk; ++n) { const double x = buf.getSample (0, n); si += x * x; }
            e.process (buf.getArrayOfWritePointers(), 2, kBlk, p, false);
            if (off >= skip) for (int n = 0; n < kBlk; ++n) { const double y = buf.getSample (0, n); so += y * y; }
        }
        return (float) (10.0 * std::log10 (std::max (1.0e-18, so / std::max (1.0, si))));
    }

    Params cabOnlyParams() const
    {
        Params p;
        p.autoLevel = false;          // the whole point: usable WITHOUT the leveler
        p.ampOn = false; p.preampOn = false;
        p.aLoaded = true;
        p.slot[0].dryWet01 = 1.0f;    // full wet
        return p;
    }

    void runTest() override
    {
       #ifndef ORBITCAB_RES_DIR
        beginTest ("skipped — no ORBITCAB_RES_DIR"); expect (true);
       #else
        const double sr = 48000.0;
        // A spread of the factory set: long/short, quietest/hottest reference gains measured.
        const char* names[] = { "01-cookie-monster.wav", "11-wumbo.wav", "16-nacho-guacamole.wav", "21-kill-dill.wav" };

        std::vector<float> gains;
        beginTest ("factory cabs pass the reference stimulus at ~unity (auto-level OFF)");
        for (const char* name : names)
        {
            double irSr = 0.0;
            const auto ir = loadIRFile (name, irSr);
            if (ir.getNumSamples() == 0) { expect (false, juce::String ("missing factory IR ") + name); continue; }

            Params p = cabOnlyParams();
            CabEngine e; e.prepare (sr, kBlk, 2, p);
            const float* ptrs[2] = { ir.getReadPointer (0), ir.getReadPointer (ir.getNumChannels() > 1 ? 1 : 0) };
            e.setSlotOriginalIR (0, ptrs, ir.getNumChannels() >= 2 ? 2 : 1, ir.getNumSamples(), irSr);
            e.slotApplyTrim (0, false, 1.0f, false);
            e.seedAutoLevel();

            const float g = throughGainDb (e, p, sr);
            gains.push_back (g);
            logMessage ("  " + juce::String (name) + " through-gain: " + juce::String (g, 2)
                        + " dB (raw IR would be ~+13..+15)");
            expect (std::fabs (g) < 1.0f, juce::String (name) + " through-gain "
                    + juce::String (g, 2) + " dB — want ~0 (|g| < 1.0)");

            // seedAutoLevel now seeds unity (the normalization made the old ‖ir‖ estimate obsolete)
            expectWithinAbsoluteError<float> (e.autoLevelGain(), 1.0f, 1.0e-4f,
                                              "seedAutoLevel must seed unity makeup");

            // TRIM re-normalizes for free: cut to 25% and re-measure — still unity.
            e.slotApplyTrim (0, true, 0.25f, false);
            const float gTrim = throughGainDb (e, p, sr);
            expect (std::fabs (gTrim) < 1.0f, juce::String (name) + " trimmed-to-25% through-gain "
                    + juce::String (gTrim, 2) + " dB — want ~0 (|g| < 1.0)");
        }

        beginTest ("the factory set is loudness-CONSISTENT (browse tone, not level)");
        if (gains.size() >= 2)
        {
            float lo = gains[0], hi = gains[0];
            for (float g : gains) { lo = juce::jmin (lo, g); hi = juce::jmax (hi, g); }
            expect (hi - lo < 1.5f, "factory IR loudness spread " + juce::String (hi - lo, 2)
                    + " dB after normalization (want < 1.5)");
        }

        beginTest ("48 kHz IR on a 96 kHz host: the resample path stays reference-unity");
        {
            double irSr = 0.0;
            const auto ir = loadIRFile ("01-cookie-monster.wav", irSr);
            expect (ir.getNumSamples() > 0, "missing factory IR");
            if (ir.getNumSamples() > 0)
            {
                Params p = cabOnlyParams();
                CabEngine e; e.prepare (96000.0, kBlk, 2, p);
                const float* ptrs[2] = { ir.getReadPointer (0), ir.getReadPointer (ir.getNumChannels() > 1 ? 1 : 0) };
                e.setSlotOriginalIR (0, ptrs, ir.getNumChannels() >= 2 ? 2 : 1, ir.getNumSamples(), irSr);
                e.slotApplyTrim (0, false, 1.0f, false);
                const float g = throughGainDb (e, p, 96000.0);
                logMessage ("  96 kHz host through-gain: " + juce::String (g, 2)
                            + " dB (the old irSr/hostSr semantics added -6 dB here; raw peak-norm +13)");
                expect (std::fabs (g) < 1.25f, "96 kHz host through-gain " + juce::String (g, 2)
                        + " dB — want ~0 (|g| < 1.25)");
            }
        }
       #endif
    }
};

static IrNormalizationTest irNormalizationTest;
