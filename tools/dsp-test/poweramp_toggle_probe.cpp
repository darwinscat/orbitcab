// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Offline reproduction of the SIMULATOR enable/disable "kick": runs a real guitar DI (looped) through the
// processor and toggles the tube power-amp OFF→ON→OFF, printing the per-block output RMS so the volume
// jump on each switch is visible + measurable (AUTO off and on). Dev diagnostic; not a CI gate.
//   orbitcab_poweramp_toggle_probe <DI.wav|flac> [cabIR.wav]
#include "../../src/PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <cstdio>
#include <cmath>
#include <memory>

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioFormatManager fm; fm.registerBasicFormats();
   #if JUCE_USE_FLAC
    fm.registerFormat (new juce::FlacAudioFormat(), false);
   #endif
    juce::AudioBuffer<float> di;
    if (argc >= 2)
        if (std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (juce::String (argv[1])))); rd != nullptr)
        { di.setSize (1, (int) rd->lengthInSamples); rd->read (&di, 0, (int) rd->lengthInSamples, 0, true, true); }
    const bool haveDI = di.getNumSamples() > 0;

    auto proc = std::make_unique<OrbitCabAudioProcessor>();
    const double sr = 48000.0; const int block = 128;
    proc->prepareToPlay (sr, block);
    auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
    if (argc >= 3) proc->loadIRFromFile (juce::File (juce::String (argv[2])), true);
    pump (600);

    auto& apvts = proc->apvts;
    auto setP = [&] (const char* id, float norm) { if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (norm); };
    setP ("bypass", 0.f); setP ("mixA", 1.f);
    setP ("ampMode", 1.f); setP ("tubeType", 0.f); setP ("tubeTopo", 1.f);   // 6L6, single-ended (x1, as tested)
    setP ("tubeDrive", 18.0f / 36.0f); setP ("tubeSag", 0.75f); setP ("tubePresence", 0.5f); setP ("tubeDepth", 0.5f);

    juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi; juce::Random rng (1); int diPos = 0;
    auto fill = [&] { for (int i = 0; i < block; ++i) { const float s = haveDI ? di.getSample (0, (diPos = (diPos + 1) % di.getNumSamples())) : 0.3f * (rng.nextFloat() * 2.0f - 1.0f); buf.setSample (0, i, s); buf.setSample (1, i, s); } };
    auto rmsDb = [&] { double a = 0.0; for (int i = 0; i < block; ++i) { const double v = buf.getSample (0, i); a += v * v; } return 20.0 * std::log10 (std::max (1.0e-9, std::sqrt (a / block))); };
    auto phase = [&] (const char* tag, float ampOn, int blocks, int detail)
    {
        setP ("ampOn", ampOn);
        for (int b = 0; b < blocks; ++b) { fill(); proc->processBlock (buf, midi);
            if (b < detail) std::printf ("  %-4s b=%2d  %6.1f ms  %7.2f dB\n", tag, b, b * block * 1000.0 / sr, rmsDb()); }
    };

    for (int au = 0; au < 2; ++au)
    {
        setP ("autoLevel", (float) au);
        std::printf ("\n========== AUTO %s ==========\n", au ? "ON" : "OFF");
        setP ("ampOn", 0.f); for (int b = 0; b < 50; ++b) { fill(); proc->processBlock (buf, midi); }   // settle dry
        std::printf ("-- steady dry, then ENABLE SIMULATOR --\n"); phase ("ON",  1.f, 50, 22);
        std::printf ("-- steady tube, then DISABLE --\n");         phase ("off", 0.f, 50, 22);
    }
    std::printf ("\n(a clean switch = a small step to the new steady level; a spike/dip in the first ~20 blocks after a toggle is the kick)\n");
    return 0;
}
