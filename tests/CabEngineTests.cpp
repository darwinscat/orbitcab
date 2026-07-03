// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for cab::CabEngine — verify the extracted core reproduces the
// original signal-path behaviour without a host or GUI.
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>          // juce::File / juce::MemoryBlock for the real-.nam tests

#include "core/CabEngine.h"

#include <cmath>
#include <vector>

using namespace cab;

namespace
{
    void fillRamp (juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int n = 0; n < b.getNumSamples(); ++n)
                b.setSample (ch, n, std::sin (0.05f * (float) n) * 0.5f + (ch == 1 ? 0.1f : 0.0f));
    }

    void run (CabEngine& e, juce::AudioBuffer<float>& b, const Params& p)
    {
        e.process (b.getArrayOfWritePointers(), b.getNumChannels(), b.getNumSamples(), p, false);
    }

    float maxAbsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        float m = 0.0f;
        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            for (int n = 0; n < a.getNumSamples(); ++n)
                m = juce::jmax (m, std::abs (a.getSample (ch, n) - b.getSample (ch, n)));
        return m;
    }
}

struct CabEngineTest : juce::UnitTest
{
    CabEngineTest() : juce::UnitTest ("CabEngine") {}

    void runTest() override
    {
        const double sr = 48000.0;
        const int    block = 512;

        beginTest ("dry passthrough (dryWet=0) is ~bit-exact");
        {
            Params p; p.autoLevel = false; p.slot[0].dryWet01 = 0.0f;
            CabEngine e; e.prepare (sr, block, 2, p);
            juce::AudioBuffer<float> buf (2, block); fillRamp (buf);
            juce::AudioBuffer<float> ref; ref.makeCopyOf (buf);
            run (e, buf, p);
            expect (maxAbsDiff (buf, ref) < 1.0e-6f, "dry path altered the signal");
        }

        beginTest ("empty slot A (aLoaded=false) => dry passthrough, not silence");
        {
            // The "no cabinet" contract: an empty slot must output the DRY signal (clean
            // passthrough), never silence — and never read a stale wet[] buffer. FULL wet on A
            // + no IR loaded: with the slot empty the wet branch must be skipped entirely.
            Params p; p.autoLevel = false; p.aLoaded = false; p.slot[0].dryWet01 = 1.0f;
            CabEngine e; e.prepare (sr, block, 2, p);   // note: no setSlotOriginalIR → A is empty
            juce::AudioBuffer<float> buf (2, block); fillRamp (buf);
            juce::AudioBuffer<float> ref; ref.makeCopyOf (buf);
            run (e, buf, p);
            expect (maxAbsDiff (buf, ref) < 1.0e-5f, "empty slot A should pass the dry signal through unchanged");
        }

        beginTest ("bypass returns the input untouched");
        {
            Params p; p.bypass = true;
            CabEngine e; e.prepare (sr, block, 2, p);
            juce::AudioBuffer<float> buf (2, block); fillRamp (buf);
            juce::AudioBuffer<float> ref; ref.makeCopyOf (buf);
            run (e, buf, p);
            expect (maxAbsDiff (buf, ref) < 1.0e-7f, "bypass changed the signal");
        }

        beginTest ("input gain scales the dry path");
        {
            Params p; p.autoLevel = false; p.slot[0].dryWet01 = 0.0f; p.inputGainDb = 6.0f;
            CabEngine e; e.prepare (sr, block, 2, p);
            juce::AudioBuffer<float> buf (2, block); fillRamp (buf);
            juce::AudioBuffer<float> ref; ref.makeCopyOf (buf);
            run (e, buf, p);
            const float g = juce::Decibels::decibelsToGain (6.0f);
            ref.applyGain (g);
            expect (maxAbsDiff (buf, ref) < 1.0e-4f, "input gain mismatch");
        }

        beginTest ("output gain scales the dry path");
        {
            Params p; p.autoLevel = false; p.slot[0].dryWet01 = 0.0f; p.outputGainDb = 6.0f;
            CabEngine e; e.prepare (sr, block, 2, p);
            juce::AudioBuffer<float> buf (2, block); fillRamp (buf);
            juce::AudioBuffer<float> ref; ref.makeCopyOf (buf);
            run (e, buf, p);
            const float g = juce::Decibels::decibelsToGain (6.0f);
            ref.applyGain (g);
            expect (maxAbsDiff (buf, ref) < 1.0e-4f, "output gain mismatch");
        }

        beginTest ("unit-impulse IR => output ~= input (convolution identity)");
        {
            Params p; p.autoLevel = false; p.slot[0].dryWet01 = 1.0f;   // full wet
            CabEngine e; e.prepare (sr, block, 2, p);

            // mono unit-impulse IR (delta): convolution becomes the identity.
            const int irN = 64;
            std::vector<float> ir ((size_t) irN, 0.0f); ir[0] = 1.0f;
            const float* irPtr[1] = { ir.data() };
            e.setSlotOriginalIR (0, irPtr, 1, irN, sr);
            e.slotApplyTrim (0, false, 1.0f, false);   // full length -> loads the convolver

            // The IR build + atomic swap run on the convolver's background thread; pump
            // blocks (with short sleeps) until it has taken effect.
            juce::AudioBuffer<float> warm (2, block);
            for (int i = 0; i < 40; ++i) { warm.clear(); run (e, warm, p); juce::Thread::sleep (10); }

            juce::AudioBuffer<float> buf (2, block); buf.clear();
            buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f);   // impulse in
            run (e, buf, p);

            expect (buf.getSample (0, 0) > 0.5f, "impulse did not pass through the convolver");
            float tail = 0.0f;
            for (int n = 1; n < block; ++n) tail = juce::jmax (tail, std::abs (buf.getSample (0, n)));
            expect (tail < 0.2f, "delta IR should leave no tail");
        }

        beginTest ("mono-amp fold is sonically transparent for a mono (identical L/R) source");
        {
            // A folded mono source has identical L/R. The monoAmp CPU path runs the amp section on
            // ch0 ONLY (frontCh=1 => AmpStage's single-instance branch => 1x NAM) then duplicates
            // ch0 before the cab. For identical input this MUST produce bit-identical output to the
            // full-stereo path — the core guarantee: Mono halves the amp/NAM cost WITHOUT changing
            // the sound. EQ is ON so the amp section genuinely runs at a narrowed width (otherwise
            // the two paths would be trivially equal even if the fold were broken).
            auto fillMono = [] (juce::AudioBuffer<float>& b)
            {
                for (int n = 0; n < b.getNumSamples(); ++n)
                {
                    const float s = std::sin (0.05f * (float) n) * 0.5f;
                    b.setSample (0, n, s); b.setSample (1, n, s);   // identical L/R = post-fold mono
                }
            };
            Params base; base.autoLevel = false; base.aLoaded = false; base.slot[0].dryWet01 = 1.0f;
            base.eq.on = true; base.eq.bassDb = 6.0f; base.eq.trebleDb = -4.0f;   // a stage that actually processes

            Params pMono = base;   pMono.monoAmp   = true;
            Params pStereo = base; pStereo.monoAmp = false;

            CabEngine em; em.prepare (sr, block, 2, pMono);
            CabEngine es; es.prepare (sr, block, 2, pStereo);
            juce::AudioBuffer<float> bm (2, block); fillMono (bm);
            juce::AudioBuffer<float> bs (2, block); fillMono (bs);
            run (em, bm, pMono);
            run (es, bs, pStereo);
            expect (maxAbsDiff (bm, bs) < 1.0e-6f, "mono-amp path changed the sound of a mono source");

            float lr = 0.0f;   // and the mono path's own output must be perfectly centred (L == R)
            for (int n = 0; n < block; ++n) lr = juce::jmax (lr, std::abs (bm.getSample (0, n) - bm.getSample (1, n)));
            expect (lr < 1.0e-7f, "mono-amp output is not centred (L != R)");
        }

        beginTest ("mono-amp sources ch0 only + centres an asymmetric input");
        {
            // With monoAmp the engine amps ch0 and duplicates it before the cab, so even an
            // asymmetric stereo input yields a centred (L==R) output and ch1's original is discarded.
            // (Host-side selects L or R into ch0; here we prove the engine centres + drops ch1.)
            Params p; p.autoLevel = false; p.aLoaded = false; p.slot[0].dryWet01 = 1.0f; p.monoAmp = true;
            CabEngine e; e.prepare (sr, block, 2, p);
            juce::AudioBuffer<float> buf (2, block);
            for (int n = 0; n < block; ++n)
            {
                buf.setSample (0, n, std::sin (0.05f * (float) n) * 0.5f);   // ch0 = signal
                buf.setSample (1, n, std::sin (0.11f * (float) n) * 0.3f);   // ch1 = DIFFERENT (must be dropped)
            }
            run (e, buf, p);
            float lr = 0.0f;
            for (int n = 0; n < block; ++n) lr = juce::jmax (lr, std::abs (buf.getSample (0, n) - buf.getSample (1, n)));
            expect (lr < 1.0e-7f, "mono-amp did not centre an asymmetric input");
        }

        // The two tests above fold through the EQ, which does NOT catch a regression that reverts only
        // the NAM stages to full width (correct output, silently 2x NAM CPU — the fatal-on-old-laptop
        // case). These two exercise a REAL nam::DSP through the engine seam (preamp + poweramp router).
        beginTest ("mono-amp NAM path is sonically transparent for a mono source (preamp NAM)");
        {
           #ifdef ORBITCAB_RES_DIR
            const juce::File nf = juce::File (ORBITCAB_RES_DIR).getChildFile ("preamps/V4KRAK-red-12h.nam");
            expect (nf.existsAsFile(), "test resource .nam must exist: " + nf.getFullPathName());
            juce::MemoryBlock mb; if (nf.existsAsFile()) nf.loadFileAsData (mb);
            if (mb.getSize() > 0)
            {
                // Identical L/R = a folded mono source. Mono runs the preamp NAM on ch0 ONLY then
                // duplicates it → output must match the full-stereo path bit-for-bit. autoLevel off so
                // no context-dependent makeup can mask a difference; the NAM is the nonlinear stage.
                Params base; base.autoLevel = false; base.aLoaded = false; base.slot[0].dryWet01 = 1.0f;
                base.preampOn = true;
                Params pMono = base;   pMono.monoAmp   = true;
                Params pStereo = base; pStereo.monoAmp = false;

                CabEngine em; em.prepare (sr, block, 2, pMono);
                CabEngine es; es.prepare (sr, block, 2, pStereo);
                expect (em.loadPreampModelBytes (mb.getData(), mb.getSize()), "preamp model loads (mono engine)");
                expect (es.loadPreampModelBytes (mb.getData(), mb.getSize()), "preamp model loads (stereo engine)");

                auto fillMono = [] (juce::AudioBuffer<float>& b)
                {
                    for (int n = 0; n < b.getNumSamples(); ++n)
                    { const float s = std::sin (0.05f * (float) n) * 0.4f; b.setSample (0, n, s); b.setSample (1, n, s); }
                };
                juce::AudioBuffer<float> bm (2, block), bs (2, block);
                for (int i = 0; i < 8; ++i)   // warm both identically, then compare the settled block
                { fillMono (bm); fillMono (bs); run (em, bm, pMono); run (es, bs, pStereo); }

                expect (maxAbsDiff (bm, bs) < 1.0e-5f, "mono NAM path changed the sound vs stereo for a mono source");
                float lr = 0.0f;
                for (int n = 0; n < block; ++n) lr = juce::jmax (lr, std::abs (bm.getSample (0, n) - bm.getSample (1, n)));
                expect (lr < 1.0e-6f, "mono NAM output is not centred (L != R)");
            }
           #endif
        }

        beginTest ("mono-amp NAM path centres an asymmetric input (poweramp CAPTURE, router seam)");
        {
           #ifdef ORBITCAB_RES_DIR
            const juce::File nf = juce::File (ORBITCAB_RES_DIR).getChildFile ("preamps/V4KRAK-red-12h.nam");
            juce::MemoryBlock mb; if (nf.existsAsFile()) nf.loadFileAsData (mb);
            if (mb.getSize() > 0)
            {
                // monoAmp sources ch0 only and duplicates before the cab, so even asymmetric input must
                // come out centred (L==R) — via the PowerAmpRouter path (ampOn + capture), which the
                // preamp test doesn't exercise. A dropped duplicate or un-narrowed router → L != R.
                Params p; p.autoLevel = false; p.aLoaded = false; p.slot[0].dryWet01 = 1.0f;
                p.monoAmp = true; p.ampOn = true; p.powerAmpMode = PowerAmpMode::capture;
                CabEngine e; e.prepare (sr, block, 2, p);
                expect (e.loadAmpModelBytes (mb.getData(), mb.getSize()), "capture model loads");

                juce::AudioBuffer<float> buf (2, block);
                float lr = 0.0f;
                for (int i = 0; i < 8; ++i)   // warm, refilling asymmetric input each block
                {
                    for (int n = 0; n < block; ++n)
                    {
                        buf.setSample (0, n, std::sin (0.05f * (float) n) * 0.4f);   // ch0 = signal
                        buf.setSample (1, n, std::sin (0.11f * (float) n) * 0.3f);   // ch1 = DIFFERENT (must be dropped)
                    }
                    run (e, buf, p);
                }
                for (int n = 0; n < block; ++n) lr = juce::jmax (lr, std::abs (buf.getSample (0, n) - buf.getSample (1, n)));
                expect (lr < 1.0e-6f, "mono NAM (poweramp capture) did not centre an asymmetric input");
            }
           #endif
        }
    }
};

static CabEngineTest cabEngineTest;
