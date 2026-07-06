// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// PROCESSOR-level PDC regression gate — the invariant that, when violated, gave the enable/disable
// GAP: toggling a stage's power must NOT change the plugin's reported latency (setLatencySamples).
// The host re-syncs its delay-compensation only when the reported PDC changes, and that re-sync IS
// the audible gap. Run at 96 kHz (the tube's oversampling latency is always present; a bug would make
// the OFF report collapse to 0). This is the exact bug the SIMULATOR toggle hit — off must report the
// same PDC as on. Returns non-zero on failure (CI gate).
//
// (The capture/preamp NAM stages need a real rate-matching model to exercise; because a headless
// harness can't reliably drive the embedded-library load + startup-fade + reload-timer, their dry-
// path alignment + rate-match latency are proven directly on cab::CabEngine in the core test suite —
// see PowerAmpRouterAlignTests. Here we lock the always-present tube latency at the processor seam.)
#include "../src/PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <cstdio>
#include <memory>

static int failures = 0;
static void check (bool ok, const char* msg)
{
    std::printf ("  [%s] %s\n", ok ? "ok  " : "FAIL", msg);
    if (! ok) ++failures;
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;

    auto proc = std::make_unique<OrbitCabAudioProcessor>();
    const double sr = 96000.0; const int block = 512;
    proc->setPlayConfigDetails (2, 2, sr, block);   // as a host would — sets base rate + channel count
    proc->prepareToPlay (sr, block);

    auto& apvts = proc->apvts;
    auto setP = [&] (const char* id, float norm) { if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (norm); };
    juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi;
    // The pending latency-refresh flag (ampMode change → updateLatency) is drained on the message-thread
    // timer — so PUMP the message loop after a param change, then read the reported latency.
    auto pump    = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
    auto latency = [&] { pump (300); buf.clear(); proc->processBlock (buf, midi); pump (300); return proc->getLatencySamples(); };

    std::printf ("== PDC invariance @ %.0f Hz (engine SR=%.0f) ==\n", sr, proc->getSampleRate());
    setP ("bypass", 0.f);

    // TUBE: toggling the SIMULATOR power must NOT change PDC. Its oversampling latency is always present,
    // so on the FIXED seam (mode-based, not power-gated) off and on report the same value; the old seam
    // (power-gated) reported 0 when off → this check would go red.
    setP ("ampMode", 1.f);                                  // tube
    setP ("ampOn", 0.f);  const int tubeOff1 = latency();
    setP ("ampOn", 1.f);  const int tubeOn1  = latency();
    setP ("ampOn", 0.f);  const int tubeOff2 = latency();
    setP ("ampOn", 1.f);  const int tubeOn2  = latency();
    std::printf ("tube  off=%d on=%d off=%d on=%d\n", tubeOff1, tubeOn1, tubeOff2, tubeOn2);
    check (tubeOn1 > 0, "tube reports real latency (oversampling)");
    check (tubeOff1 == tubeOn1 && tubeOn1 == tubeOff2 && tubeOff2 == tubeOn2,
           "tube PDC constant across every SIMULATOR power toggle");

    std::printf (failures ? "\n==== PDC TEST: %d FAILURE(S) ====\n" : "\n==== PDC TEST: all checks passed ====\n", failures);
    return failures ? 1 : 0;
}
