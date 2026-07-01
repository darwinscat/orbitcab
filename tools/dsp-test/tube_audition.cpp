// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

// Offline AUDITION renderer for the white-box "Tube" power-amp (blocks 2 + 3). Feeds a dry guitar DI
// through the REAL OrbitCabAudioProcessor (Tube mode → cab IR — exactly the plugin signal path) at a
// curated matrix of settings and writes one labelled .wav per variation, so the tube voicing / sag /
// presence / depth can be judged by ear without a DAW or any UI. Dev tool; not a CI gate.
//
//   orbitcab_tube_audition <DI.wav> [cabIR.wav] [outDir]
//
// If cabIR is omitted, the processor's default bundled cab is used. Output defaults to
// "<DI-folder>/orbitcab-audition/". Each render is peak-normalised to -3 dBFS for a fair, equal-loudness
// A/B; the pre-normalisation natural-peak is printed, and sag's DYNAMICS within each file are preserved
// (only the overall gain is scaled) — so the touch/bloom is still audible.
#include "../../src/PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
    // A render preset. All values are HOST-NORMALISED [0,1] (what setValueNotifyingHost wants):
    //   tubeType: 6L6=0, EL34=1/3, EL84=2/3, KT88=1   •   tubeTopo: PP=0, SE=1
    //   drive over 0..36 dB (0.25≈9 dB clean, 0.55≈20 dB crunch, 0.85≈31 dB hot)   •   sag/pres/depth = %
    struct Preset { const char* label; float amp, mode, tube, topo, drive, sag, pres, depth, load; };

    const std::vector<Preset> kMatrix = {
        //  label                          amp  mode  tube    topo drive  sag   pres  depth load
        { "00-ref-cab-only-no-poweramp",   0.f, 1.f,  0.f,    0.f, 0.50f, 0.f,  0.f,  0.f,  0.f }, // A/B baseline: DI→cab, tube off
        { "01-6L6-crunch",                 1.f, 1.f,  0.000f, 0.f, 0.55f, 0.4f, 0.4f, 0.4f, 0.f },
        { "02-EL34-crunch",                1.f, 1.f,  0.333f, 0.f, 0.55f, 0.4f, 0.4f, 0.4f, 0.f },
        { "03-EL84-crunch",                1.f, 1.f,  0.667f, 0.f, 0.55f, 0.4f, 0.4f, 0.4f, 0.f },
        { "04-KT88-crunch",                1.f, 1.f,  1.000f, 0.f, 0.55f, 0.4f, 0.4f, 0.4f, 0.f },
        { "05-EL34-clean",                 1.f, 1.f,  0.333f, 0.f, 0.25f, 0.2f, 0.3f, 0.3f, 0.f },
        { "06-EL34-hot",                   1.f, 1.f,  0.333f, 0.f, 0.85f, 0.7f, 0.6f, 0.6f, 0.f },
        { "07-EL34-hot-DRY-no-feel",       1.f, 1.f,  0.333f, 0.f, 0.85f, 0.0f, 0.0f, 0.0f, 0.f }, // isolate the block-2 core
        { "08-EL34-hot-SAG-only",          1.f, 1.f,  0.333f, 0.f, 0.85f, 0.9f, 0.0f, 0.0f, 0.f }, // isolate sag
        { "09-EL34-hot-SE-classA",         1.f, 1.f,  0.333f, 1.f, 0.85f, 0.7f, 0.6f, 0.6f, 0.f }, // even-harmonic single-ended
        { "10-EL84-hot-SE",                1.f, 1.f,  0.667f, 1.f, 0.80f, 0.8f, 0.7f, 0.7f, 0.f },
        { "11-KT88-hifi-clean",            1.f, 1.f,  1.000f, 0.f, 0.30f, 0.2f, 0.2f, 0.4f, 0.f },
        // ---- block 4 VIRTUAL LOAD A/B: identical to 01-04/06 but LOAD=100% (reactive-speaker impedance) ----
        { "01L-6L6-crunch-LOAD",           1.f, 1.f,  0.000f, 0.f, 0.55f, 0.4f, 0.4f, 0.4f, 1.f },
        { "02L-EL34-crunch-LOAD",          1.f, 1.f,  0.333f, 0.f, 0.55f, 0.4f, 0.4f, 0.4f, 1.f },
        { "03L-EL84-crunch-LOAD",          1.f, 1.f,  0.667f, 0.f, 0.55f, 0.4f, 0.4f, 0.4f, 1.f },
        { "04L-KT88-crunch-LOAD",          1.f, 1.f,  1.000f, 0.f, 0.55f, 0.4f, 0.4f, 0.4f, 1.f },
        { "06L-EL34-hot-LOAD",             1.f, 1.f,  0.333f, 0.f, 0.85f, 0.7f, 0.6f, 0.6f, 1.f },
    };
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc < 2)
    {
        std::printf ("usage: orbitcab_tube_audition <DI.wav> [cabIR.wav] [outDir]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;   // MessageManager for the async IR loader

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();                                    // WAV + AIFF
   #if JUCE_USE_FLAC
    fm.registerFormat (new juce::FlacAudioFormat(), false);       // DIs are often FLAC (Cubase/exports)
   #endif
   #if JUCE_USE_OGGVORBIS
    fm.registerFormat (new juce::OggVorbisAudioFormat(), false);
   #endif
    juce::File diFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    if (! diFile.existsAsFile()) diFile = juce::File (juce::String (argv[1]));
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (diFile));
    if (rd == nullptr) { std::printf ("ERROR: cannot read DI wav '%s'\n", argv[1]); return 2; }

    const double sr = rd->sampleRate > 0 ? rd->sampleRate : 48000.0;
    const int    N  = (int) rd->lengthInSamples;
    juce::AudioBuffer<float> di (juce::jmax (1, (int) rd->numChannels), N);
    rd->read (&di, 0, N, 0, true, true);
    std::printf ("DI: %s  %.0f Hz  %d ch  %.2f s\n", diFile.getFileName().toRawUTF8(), sr, di.getNumChannels(), N / sr);

    juce::File outDir = (argc >= 4) ? juce::File (juce::String (argv[3]))
                                    : diFile.getParentDirectory().getChildFile ("orbitcab-audition");
    outDir.createDirectory();

    auto procOwned = std::make_unique<OrbitCabAudioProcessor>();   // heap — large processor vs MSVC 1 MB stack
    auto& proc = *procOwned;
    const int block = 512;
    proc.prepareToPlay (sr, block);

    auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
    if (argc >= 3) proc.loadIRFromFile (juce::File (juce::String (argv[2])), true);
    pump (600);   // bundled/loaded IR decode + settle

    auto& apvts = proc.apvts;
    auto setP = [&] (const char* id, float norm) { if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (norm); };
    setP ("bypass", 0.f); setP ("autoLevel", 0.f); setP ("mixA", 1.f);   // 100% wet A, no auto-level masking

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> work (2, block);

    for (const auto& ps : kMatrix)
    {
        setP ("ampOn", ps.amp);   setP ("ampMode", ps.mode);
        setP ("tubeType", ps.tube); setP ("tubeTopo", ps.topo); setP ("tubeDrive", ps.drive);
        setP ("tubeSag", ps.sag); setP ("tubePresence", ps.pres); setP ("tubeDepth", ps.depth); setP ("tubeLoad", ps.load);
        pump (60);
        proc.reset();   // note: OrbitCabAudioProcessor may not override reset(), so the silent pre-roll below
        const int preRoll = (int) (sr * 0.6 / block) + 1;   // ~600 ms — is what actually flushes the previous
        for (int w = 0; w < preRoll; ++w) { work.clear(); proc.processBlock (work, midi); }   // preset's sag/IR tail

        juce::AudioBuffer<float> out (2, N);
        for (int pos = 0; pos < N; )
        {
            const int n = juce::jmin (block, N - pos);
            work.clear();
            for (int ch = 0; ch < 2; ++ch)
                work.copyFrom (ch, 0, di, ch < di.getNumChannels() ? ch : 0, pos, n);
            juce::AudioBuffer<float> sub (work.getArrayOfWritePointers(), 2, n);
            proc.processBlock (sub, midi);
            for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, sub, ch, 0, n);
            pos += n;
        }

        // Peak-normalise each render to -3 dBFS: prevents digital clipping and loudness-matches the set
        // for a fair A/B (the sag DYNAMICS inside each file are untouched — only the overall gain scales).
        const float peak = out.getMagnitude (0, N);
        const float norm = peak > 1.0e-6f ? 0.707f / peak : 1.0f;
        out.applyGain (norm);

        juce::File of = outDir.getChildFile (juce::String (ps.label) + ".wav");
        of.deleteFile();
        juce::WavAudioFormat wav;
        if (auto* os = of.createOutputStream().release())
        {
            std::unique_ptr<juce::AudioFormatWriter> wr (wav.createWriterFor (os, sr, 2, 24, {}, 0));
            if (wr != nullptr) wr->writeFromAudioSampleBuffer (out, 0, N);
            else delete os;
        }
        std::printf ("  %-30s natural-peak=%6.3f  norm %+5.1f dB  -> %s\n", ps.label, peak,
                     juce::Decibels::gainToDecibels (norm), of.getFileName().toRawUTF8());
    }

    std::printf ("=== %d renders in %s ===\n", (int) kMatrix.size(), outDir.getFullPathName().toRawUTF8());
    return 0;
}
