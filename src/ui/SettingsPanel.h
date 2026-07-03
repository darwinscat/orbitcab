// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "OrbitCabLookAndFeel.h"

#include <functional>

//==============================================================================
// SettingsPanel — the gear-button pop-over content. Holds the rarely-touched toggles
// that don't earn a slot on the main surface:
//   • HEAD trim     — snap each IR to its onset (drop baked-in cabinet pre-delay).
//                     A session setting (saved with the project) — audio-affecting.
//   • Dry/Wet       — reveal the per-slot Dry/Wet sliders (under the checkbox rows).
//                     A view setting (default off), so the wet/dry blend is opt-in.
//   • Spectrum      — the faint pre/post analyser behind the waveforms. A view setting.
// Pure view: it owns no state, only reflects the values passed in and reports clicks
// via the callbacks. Launched in a CallOutBox parented to the editor.
//==============================================================================
class SettingsPanel final : public juce::Component
{
public:
    SettingsPanel (bool headOn, int inputSource, bool dryWetOn, bool spectrumOn, bool waveLogOn, int waveFloorDb,
                   bool showTubes,
                   std::function<void (bool)> onHead,
                   std::function<void (int)>  onInputSource,
                   std::function<void (bool)> onDryWet,
                   std::function<void (bool)> onSpectrum,
                   std::function<void (bool)> onWaveLog,
                   std::function<void (int)>  onWaveFloor,
                   std::function<void (bool)> onShowTubes,
                   std::function<void ()>     onManageAmp,
                   std::function<void ()>     onManagePreamp)
    {
        title.setText ("Settings", juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8ee));
        addAndMakeVisible (title);

        setUp (head,     "Trim leading silence", headOn,
               "Snap each IR to its onset — drops the baked-in cabinet pre-delay so dry/wet and A/B blends stay phase-aligned.",
               std::move (onHead));

        // INPUT source — fold the stereo input to one channel copied to both, for a mono source
        // (e.g. a guitar) on a single interface input. Stereo = unchanged. Audio-affecting +
        // session-scoped, so it lives in this "SAVED WITH THE PROJECT" section.
        setCaption (inputCap, "Input");
        inputSrc.addItem ("Stereo (L/R)", 1);   // id 1 → mode 0
        inputSrc.addItem ("Left (mono)",  2);   // id 2 → mode 1
        inputSrc.addItem ("Right (mono)", 3);   // id 3 → mode 2
        inputSrc.setSelectedId (juce::jlimit (0, 2, inputSource) + 1, juce::dontSendNotification);
        inputSrc.setTooltip ("Take the input from one channel and send it to both outputs - for a mono source (a guitar) on a single interface input. Stereo = process left and right independently (unchanged).");
        inputSrc.onChange = [this, fn = std::move (onInputSource)]
        {
            if (fn) fn (juce::jlimit (0, 2, inputSrc.getSelectedId() - 1));
        };
        addAndMakeVisible (inputSrc);

        setUp (dryWet,   "Dry/Wet sliders", dryWetOn,
               "Show a per-slot Dry/Wet mix slider under each checkbox row (blend the raw input back in).",
               std::move (onDryWet));
        setUp (spectrum, "Spectrum analyser", spectrumOn,
               "Show the faint pre/post spectrum behind the waveforms.",
               std::move (onSpectrum));

        // Waveform amplitude scale (view pref): log (dB, lifts the decay tail into view) vs
        // linear, with a selectable dB floor for the log scale.
        waveLog.setButtonText ("Log waveform (dB)");
        waveLog.setToggleState (waveLogOn, juce::dontSendNotification);
        waveLog.setTooltip ("Show the IR on a dB (log) amplitude scale so its decay tail is visible — off = linear.");
        waveLog.setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
        waveLog.onClick = [this, fn = std::move (onWaveLog)]
        {
            const bool on = waveLog.getToggleState();
            waveFloor.setEnabled (on);
            if (fn) fn (on);
        };
        addAndMakeVisible (waveLog);

        setCaption (floorCap, "dB floor");
        waveFloor.addItem (juce::String::fromUTF8 ("\xe2\x88\x92") + "60 dB", 1);   // −60
        waveFloor.addItem (juce::String::fromUTF8 ("\xe2\x88\x92") + "48 dB", 2);   // −48
        waveFloor.addItem (juce::String::fromUTF8 ("\xe2\x88\x92") + "32 dB", 3);   // −32
        waveFloor.setSelectedId (waveFloorDb == -60 ? 1 : waveFloorDb == -32 ? 3 : 2, juce::dontSendNotification);
        waveFloor.setEnabled (waveLogOn);
        waveFloor.setTooltip ("Lowest level shown on the dB scale — lower reveals more decay / room tail.");
        waveFloor.onChange = [this, fn = std::move (onWaveFloor)]
        {
            const int db = waveFloor.getSelectedId() == 1 ? -60 : waveFloor.getSelectedId() == 3 ? -32 : -48;
            if (fn) fn (db);
        };
        addAndMakeVisible (waveFloor);

        // NAM stages (PREAMP / POWERAMP): the model is picked from the bottom-strip selectors; this
        // panel just carries the shared "show tubes" view toggle + the two library managers.
        setCaption (ampCap, "NAM STAGES (PREAMP / POWERAMP)");
        setUp (showTubesBtn, "Show tubes", showTubes,
               "Draw the glowing schematic tubes in the PREAMP / POWERAMP rows. Off = just the controls.",
               std::move (onShowTubes));

        // "Manage library…" → the Preamp/Poweramp manager pop-overs (Add / Remove your .nam captures).
        // Always present, even with an empty library — it's how the first user model gets added.
        managePreampBtn.setButtonText (juce::String::fromUTF8 ("Preamp library\xe2\x80\xa6"));
        managePreampBtn.setTooltip ("Add or remove your preamp captures (the .nam models the PREAMP selector lists).");
        managePreampBtn.onClick = [fn = std::move (onManagePreamp)] { if (fn) fn(); };
        addAndMakeVisible (managePreampBtn);

        manageBtn.setButtonText (juce::String::fromUTF8 ("Poweramp library\xe2\x80\xa6"));
        manageBtn.setTooltip ("Add or remove your poweramp captures (the .nam models the POWERAMP selector lists).");
        manageBtn.onClick = [fn = std::move (onManageAmp)] { if (fn) fn(); };
        addAndMakeVisible (manageBtn);

        // Two scopes: HEAD trim is audio-affecting and rides the DAW session; Dry/Wet +
        // Spectrum are app-wide view prefs (this computer, every instance). Caption + divide
        // so it's obvious which toggles travel with the project and which don't.
        setCaption (sessionCap, "SAVED WITH THE PROJECT");
        setCaption (globalCap,  "THIS COMPUTER");

        setSize (264, 402);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 12);
        title.setBounds (r.removeFromTop (20));
        r.removeFromTop (6);
        sessionCap.setBounds (r.removeFromTop (14));
        head.setBounds       (r.removeFromTop (26));
        r.removeFromTop (4);
        auto inputRow = r.removeFromTop (26);
        inputCap.setBounds (inputRow.removeFromLeft (58).withTrimmedTop (6));
        inputSrc.setBounds (inputRow.removeFromLeft (118).reduced (0, 3));
        r.removeFromTop (7);
        dividerY = r.getY();                            // section divider, drawn in paint()
        r.removeFromTop (8);
        globalCap.setBounds (r.removeFromTop (14));
        dryWet.setBounds    (r.removeFromTop (26));
        spectrum.setBounds  (r.removeFromTop (26));
        waveLog.setBounds   (r.removeFromTop (26));
        auto floorRow = r.removeFromTop (26);
        floorCap.setBounds  (floorRow.removeFromLeft (58).withTrimmedTop (6));
        waveFloor.setBounds (floorRow.removeFromLeft (98).reduced (0, 3));

        r.removeFromTop (8);
        ampDividerY = r.getY();                         // second section divider (drawn in paint())
        r.removeFromTop (8);
        ampCap.setBounds (r.removeFromTop (14));
        showTubesBtn.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        managePreampBtn.setBounds (r.removeFromTop (26).removeFromLeft (160));
        r.removeFromTop (4);
        manageBtn.setBounds (r.removeFromTop (26).removeFromLeft (160));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0x22ffffff));
        if (dividerY > 0)                               // faint rule between the two scopes
            g.fillRect (14, dividerY, getWidth() - 28, 1);
        if (ampDividerY > 0)                            // rule above the NAM section
            g.fillRect (14, ampDividerY, getWidth() - 28, 1);
    }

private:
    void setUp (juce::ToggleButton& b, const juce::String& text, bool on,
                const juce::String& tip, std::function<void (bool)> cb)
    {
        b.setButtonText (text);
        b.setToggleState (on, juce::dontSendNotification);
        b.setTooltip (tip);
        b.setColour (juce::ToggleButton::tickColourId, juce::Colour (OrbitCabLookAndFeel::kAccent));
        b.onClick = [&b, fn = std::move (cb)] { if (fn) fn (b.getToggleState()); };
        addAndMakeVisible (b);
    }

    void setCaption (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        l.setColour (juce::Label::textColourId, juce::Colour (0xff80808a));
        addAndMakeVisible (l);
    }

    juce::Label        title, sessionCap, globalCap, floorCap, ampCap, inputCap;
    juce::ToggleButton head, dryWet, spectrum, waveLog, showTubesBtn;
    juce::TextButton   manageBtn, managePreampBtn;
    juce::ComboBox     waveFloor, inputSrc;
    int                dividerY = 0, ampDividerY = 0;   // section dividers (set in resized(), drawn in paint())

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};
