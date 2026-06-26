// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#include "Parameters.h"

#include <cmath>

namespace orbitcab
{

namespace
{
    // Bump only when a parameter's meaning changes — keeps host automation stable across
    // releases (a new version hint => the host treats it as a new param). FROZEN at 1.
    constexpr int kParamVersion = 1;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    // String formatters double as the on-screen readout — a SliderAttachment overwrites
    // the slider's textFromValueFunction with the parameter's getText, so clean text must
    // live on the *parameter* to show in both the editor and the host.
    auto hzText  = [] (float v, int) { return juce::String (juce::roundToInt (v)) + " Hz"; };
    auto kHzText = [] (float v, int) { return juce::String (v / 1000.0f, 1) + " kHz"; };
    auto pctText = [] (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; };
    auto dbText  = [] (float v, int) { return juce::String (v, 1) + " dB"; };

    // ---- global ----
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "inputGain", kParamVersion }, "Input",
                                                        NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "bypass", kParamVersion }, "Bypass", false));
    // A<->B IR crossfade: 0 = Slot A, 100 = Slot B. Only audible once B loads.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mixAB",  kParamVersion }, "A/B Mix",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
                                                        AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pctText)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain",   kParamVersion }, "Output",
                                                        NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                                        AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "autoLevel", kParamVersion }, "Auto Level", true));
    // POWERAMP (NAM) — `ampOn` is the only host param here: the master gate / bypass for the
    // neural poweramp stage in front of the cab (off by default, shapers-off policy). WHICH model
    // is loaded is NOT a host param — it's a library selection persisted as the "ampSel" property
    // on the state tree (rides session / A-B-C-D / undo / presets, like "headTrim"), resolved from
    // the merged factory+user library and loaded off the audio thread (PluginProcessor::applyPoweramp).
    // (Output loudness-normalisation is always on — there's no user reason to hear models at
    // mismatched levels — so no param/toggle for it either.)
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "ampOn", kParamVersion }, "Amp (NAM)", false));
    // PREAMP (NAM) — `preampOn` gates the SECOND neural stage, run BEFORE the poweramp (input →
    // PREAMP → POWERAMP → cab). Same shape as `ampOn`: off by default; WHICH model is loaded is the
    // "preampSel" library selection (not a host param), resolved off the audio thread in
    // PluginProcessor::applyPreamp(). Output loudness-normalisation is always on (no toggle).
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "preampOn", kParamVersion }, "Preamp (NAM)", false));

    // ---- per slot (A/B): HPF + LPF + Phase + Dry/Wet + Trim-enable ----
    // Cutoff ranges (widened per user): HPF 30–400 Hz (def 80), LPF 2–12 kHz (def 7k);
    // skew to the geometric centre for a musical log-ish feel.
    auto addSlot = [&] (juce::String s)
    {
        NormalisableRange<float> hpfRange (30.0f, 400.0f);     hpfRange.setSkewForCentre (std::sqrt (30.0f * 400.0f));
        NormalisableRange<float> lpfRange (2000.0f, 12000.0f); lpfRange.setSkewForCentre (std::sqrt (2000.0f * 12000.0f));

        layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "hpfOn" + s,  kParamVersion }, "HPF " + s, false));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "hpfFreq" + s, kParamVersion }, "HPF Freq " + s, hpfRange, 80.0f,
                                                            AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (hzText)));
        layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "lpfOn" + s,  kParamVersion }, "LPF " + s, false));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "lpfFreq" + s, kParamVersion }, "LPF Freq " + s, lpfRange, 7000.0f,
                                                            AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (kHzText)));
        layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "phase" + s, kParamVersion }, "Phase " + s, false));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix" + s,   kParamVersion }, "Dry/Wet " + s,
                                                            NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                            AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (pctText)));
        layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "trimOn" + s, kParamVersion }, "Trim " + s, false));
        // HEAD (trim leading silence) is NOT a per-slot param: it's a single global, on-by-default
        // session setting (the "headTrim" property on the APVTS state tree, toggled in the gear
        // settings panel) — not host-automatable. See PluginProcessor::setHeadTrim.
        layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "mute" + s,   kParamVersion }, "Mute " + s, false));
    };
    addSlot ("A");
    addSlot ("B");
    return layout;
}

} // namespace orbitcab
