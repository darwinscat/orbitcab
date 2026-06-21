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
    SettingsPanel (bool headOn, bool dryWetOn, bool spectrumOn,
                   std::function<void (bool)> onHead,
                   std::function<void (bool)> onDryWet,
                   std::function<void (bool)> onSpectrum)
    {
        title.setText ("Settings", juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8ee));
        addAndMakeVisible (title);

        setUp (head,     "Trim leading silence", headOn,
               "Snap each IR to its onset — drops the baked-in cabinet pre-delay so dry/wet and A/B blends stay phase-aligned.",
               std::move (onHead));
        setUp (dryWet,   "Dry/Wet sliders", dryWetOn,
               "Show a per-slot Dry/Wet mix slider under each checkbox row (blend the raw input back in).",
               std::move (onDryWet));
        setUp (spectrum, "Spectrum analyser", spectrumOn,
               "Show the faint pre/post spectrum behind the waveforms.",
               std::move (onSpectrum));

        // Two scopes: HEAD trim is audio-affecting and rides the DAW session; Dry/Wet +
        // Spectrum are app-wide view prefs (this computer, every instance). Caption + divide
        // so it's obvious which toggles travel with the project and which don't.
        setCaption (sessionCap, "SAVED WITH THE PROJECT");
        setCaption (globalCap,  "THIS COMPUTER");

        setSize (264, 184);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 12);
        title.setBounds (r.removeFromTop (20));
        r.removeFromTop (6);
        sessionCap.setBounds (r.removeFromTop (14));
        head.setBounds       (r.removeFromTop (26));
        r.removeFromTop (7);
        dividerY = r.getY();                            // section divider, drawn in paint()
        r.removeFromTop (8);
        globalCap.setBounds (r.removeFromTop (14));
        dryWet.setBounds    (r.removeFromTop (26));
        spectrum.setBounds  (r.removeFromTop (26));
    }

    void paint (juce::Graphics& g) override
    {
        if (dividerY > 0)                               // faint rule between the two scopes
        {
            g.setColour (juce::Colour (0x22ffffff));
            g.fillRect (14, dividerY, getWidth() - 28, 1);
        }
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

    juce::Label        title, sessionCap, globalCap;
    juce::ToggleButton head, dryWet, spectrum;
    int                dividerY = 0;   // y of the section divider (set in resized(), drawn in paint())

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};
