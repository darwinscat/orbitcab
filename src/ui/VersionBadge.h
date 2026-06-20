// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "OrbitCabLookAndFeel.h"
#include "../UpdateChecker.h"

//==============================================================================
// VersionBadge — a small clickable "v1.0.0" label, bottom-left. Always shown,
// offline-safe. A bright static accent dot appears when a stored "latest" is newer than
// the installed version. Click → a CallOutBox popup with the version, the Darwin's Cat
// link, and an opt-in "Check for updates" button (the ONLY thing that hits the network —
// never silent).
//==============================================================================
class VersionBadge final : public juce::Component,
                           public juce::SettableTooltipClient
{
public:
    explicit VersionBadge (orbitcab::UpdateChecker& uc) : checker (uc)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("OrbitCab v" + checker.currentVersion() + " — click to check for updates");
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const bool upd = checker.updateAvailable();

        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.setColour (juce::Colour (upd ? OrbitCabLookAndFeel::kAccentHover : 0xff70707a));
        g.drawText ("v" + checker.currentVersion(), r, juce::Justification::centredLeft, false);

        if (upd)   // bright static dot just right of the version (update available)
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (g.getCurrentFont(), "v" + checker.currentVersion(), 0.0f, 0.0f);
            const float tw = ga.getBoundingBox (0, -1, true).getWidth();
            const float cx = r.getX() + tw + 7.0f, cy = r.getCentreY();
            g.setColour (juce::Colour (OrbitCabLookAndFeel::kAccentB));   // orange = "new"
            g.fillEllipse (cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()))
            showPopup();
    }

private:
    //--- the CallOutBox content ----------------------------------------------
    struct Panel final : public juce::Component
    {
        explicit Panel (orbitcab::UpdateChecker& uc, VersionBadge& ownerBadge)
            : checker (uc), owner (&ownerBadge)
        {
            title.setText ("OrbitCab", juce::dontSendNotification);
            title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            title.setColour (juce::Label::textColourId, juce::Colour (OrbitCabLookAndFeel::kText));
            addAndMakeVisible (title);

            version.setText ("Version " + checker.currentVersion(), juce::dontSendNotification);
            version.setFont (juce::FontOptions (12.0f));
            version.setColour (juce::Label::textColourId, juce::Colour (0xffb0b0b8));
            addAndMakeVisible (version);

            link.setButtonText ("by Darwin's Cat");
            link.setURL (juce::URL ("https://darwinscat.com/orbitcab?utm_source=orbitcab&utm_medium=plugin"));
            link.setColour (juce::HyperlinkButton::textColourId, juce::Colour (OrbitCabLookAndFeel::kAccentHover));
            link.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (link);

            check.setButtonText ("Check for updates");
            check.onClick = [this] { runCheck(); };
            addAndMakeVisible (check);

            result.setFont (juce::FontOptions (12.0f));
            result.setJustificationType (juce::Justification::centredLeft);
            result.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));
            addAndMakeVisible (result);

            download.setButtonText ("Download");
            download.setColour (juce::HyperlinkButton::textColourId, juce::Colour (OrbitCabLookAndFeel::kAccentB));
            addChildComponent (download);   // hidden until an update is actually available (then setURL + setVisible)

            note.setText ("Opt-in. Sends only product + version.", juce::dontSendNotification);
            note.setFont (juce::FontOptions (10.0f));
            note.setColour (juce::Label::textColourId, juce::Colour (0xff60606a));
            addAndMakeVisible (note);

            // If an update is already known from a previous check, show it up front.
            if (checker.updateAvailable())
                showUpdate (checker.storedLatest(),
                            juce::URL ("https://github.com/darwinscat/orbitcab/releases/latest"));

            setSize (250, 156);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (14, 12);
            title.setBounds   (r.removeFromTop (20));
            link.setBounds    (r.removeFromTop (18));
            version.setBounds (r.removeFromTop (18));
            r.removeFromTop (6);
            check.setBounds   (r.removeFromTop (26));
            r.removeFromTop (4);
            result.setBounds  (r.removeFromTop (18));
            download.setBounds (r.removeFromTop (16));
            note.setBounds    (r.removeFromBottom (14));
        }

        void runCheck()
        {
            check.setEnabled (false);
            result.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));
            result.setText (juce::String::fromUTF8 ("Checking\xe2\x80\xa6"), juce::dontSendNotification);
            download.setVisible (false);

            juce::Component::SafePointer<Panel> safe (this);
            checker.checkNow ([safe] (orbitcab::UpdateChecker::Result res)
            {
                if (auto* self = safe.getComponent())
                    self->onResult (res);
            });
        }

        void onResult (const orbitcab::UpdateChecker::Result& res)
        {
            check.setEnabled (true);
            if (owner != nullptr) owner->repaint();   // badge dot may have appeared/cleared (badge may be gone)

            if (! res.ok)
            {
                result.setColour (juce::Label::textColourId, juce::Colour (0xffb0b0b8));
                result.setText (juce::String::fromUTF8 ("Couldn\xe2\x80\x99t check (offline?)"), juce::dontSendNotification);
                return;
            }
            if (res.outdated)
                showUpdate (res.latest, juce::URL (res.url.isNotEmpty() ? res.url
                                                                        : juce::String ("https://github.com/darwinscat/orbitcab/releases/latest")));
            else
            {
                result.setColour (juce::Label::textColourId, juce::Colour (0xff7be29a));   // green
                result.setText (juce::String::fromUTF8 ("\xe2\x9c\x93 Up to date"), juce::dontSendNotification);
            }
        }

        void showUpdate (const juce::String& latest, const juce::URL& url)
        {
            result.setColour (juce::Label::textColourId, juce::Colour (OrbitCabLookAndFeel::kAccentB));
            result.setText (juce::String::fromUTF8 ("\xe2\x86\x91 Update available: v") + latest, juce::dontSendNotification);
            download.setURL (url);
            download.setVisible (true);
        }

        orbitcab::UpdateChecker& checker;
        juce::Component::SafePointer<VersionBadge> owner;   // the badge may outlive-die before an async check returns
        juce::Label          title, version, result, note;
        juce::HyperlinkButton link, download;
        juce::TextButton     check;
    };

    void showPopup()
    {
        auto panel = std::make_unique<Panel> (checker, *this);
        juce::CallOutBox::launchAsynchronously (std::move (panel), getScreenBounds(), nullptr);
    }

    orbitcab::UpdateChecker& checker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VersionBadge)
};
