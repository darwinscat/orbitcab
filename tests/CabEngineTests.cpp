// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Headless unit tests for cab::CabEngine — verify the extracted core reproduces the
// original signal-path behaviour without a host or GUI.
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_basics/juce_audio_basics.h>

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
    }
};

static CabEngineTest cabEngineTest;
