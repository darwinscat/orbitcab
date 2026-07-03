// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "OrbitCabLookAndFeel.h"
#include "BrandMark.h"          // orbitcab::brand — shared orbit mark + Michroma wordmark
#include "../UpdateChecker.h"
#include "OrbitCabVersion.h"   // orbitcab::version::* — the generated build stamp (git describe, build no., arch)

//==============================================================================
// VersionBadge — a small clickable "v1.6.0 / <format>" label, bottom-right. Always shown,
// offline-safe. A bright static accent dot appears when a stored "latest" is newer than the
// installed version. Click → a CallOutBox popup with the brand mark, the full build stamp (with
// GitHub links for the version / commit / core), and an opt-in "Check for updates" button (the
// ONLY thing that hits the network — never silent).
//==============================================================================
class VersionBadge final : public juce::Component,
                           public juce::SettableTooltipClient
{
public:
    VersionBadge (orbitcab::UpdateChecker& uc, juce::String pluginFormat)
        : checker (uc), format (std::move (pluginFormat))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("OrbitCab v" + checker.currentVersion() + " (" + format + ") — click to check for updates");
    }

    // The editor supplies the embedded Michroma typeface (loaded once for the header) so the popover's
    // title mark matches the window header. Call after construction, before the first popup.
    void setBrandTypeface (juce::Typeface::Ptr tf) { brandTypeface = std::move (tf); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const bool upd = checker.updateAvailable();

        // Line 1 — the version, a touch bigger; line 2 (below) is the running plugin format.
        auto verRow = r.removeFromTop (r.getHeight() * 0.56f);
        const juce::Font verFont (juce::FontOptions (14.0f, juce::Font::bold));
        const juce::String ver = "v" + checker.currentVersion();
        g.setFont (verFont);
        g.setColour (juce::Colour (upd ? OrbitCabLookAndFeel::kAccentHover : 0xff8a8a92));
        g.drawText (ver, verRow, juce::Justification::centredLeft, false);

        // Line 2 — the running plugin format (VST3 / AU / CLAP / Standalone). The build number
        // lives only in the (i) popover — the corner stays version + format.
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (juce::Colour (OrbitCabLookAndFeel::kAccent).withAlpha (0.9f));
        g.drawText (format, r, juce::Justification::centredLeft, false);

        if (upd)   // bright static dot just right of the version (update available)
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (verFont, ver, 0.0f, 0.0f);
            const float tw = ga.getBoundingBox (0, -1, true).getWidth();
            const float cx = verRow.getX() + tw + 7.0f, cy = verRow.getCentreY();
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
        Panel (orbitcab::UpdateChecker& uc, VersionBadge& ownerBadge,
               juce::String pluginFormat, juce::Typeface::Ptr brandTf)
            : checker (uc), owner (&ownerBadge), brandTypeface (std::move (brandTf))
        {
            using namespace orbitcab::version;
            const juce::String mono = juce::Font::getDefaultMonospacedFontName();
            const juce::String mid  = juce::String::fromUTF8 (" \xc2\xb7 ");   // " · "

            // "by Darwin's Cat" brand link (under the title mark).
            link.setButtonText ("by Darwin's Cat");
            link.setURL (juce::URL ("https://darwinscat.com/orbitcab?utm_source=orbitcab&utm_medium=plugin"));
            link.setColour (juce::HyperlinkButton::textColourId, juce::Colour (OrbitCabLookAndFeel::kAccentHover));
            link.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (link);

            // The three GitHub links (version → release tag, commit → HEAD, core → felitronics-core tag).
            auto ghLink = [&] (juce::HyperlinkButton& b, const juce::String& text, const juce::String& url)
            {
                b.setButtonText (text);
                b.setURL (juce::URL (url));
                b.setFont (juce::FontOptions (11.0f).withName (mono), false, juce::Justification::centredLeft);
                b.setColour (juce::HyperlinkButton::textColourId, juce::Colour (OrbitCabLookAndFeel::kAccentHover));
                b.changeWidthToFitText();
                addAndMakeVisible (b);
            };
            const juce::String ver = "v" + checker.currentVersion();
            ghLink (verLink,    ver,                          "https://github.com/darwinscat/orbitcab/releases/tag/" + ver);
            ghLink (commitLink, juce::String ("g") + kGitHash, "https://github.com/darwinscat/orbitcab/commit/" + juce::String (kGitHash));
            // core: strip " (local)" and any "-N-g…" dev suffix → the bare vX.Y.Z release tag.
            const juce::String coreTag = juce::String (kCoreVersion).upToFirstOccurrenceOf (" ", false, false)
                                                                    .upToFirstOccurrenceOf ("-", false, false);
            ghLink (coreLink, coreTag, "https://github.com/darwinscat/felitronics-core/releases/tag/" + coreTag);

            // The plain-text bits that annotate each link line.
            auto info = [&] (juce::Label& l, const juce::String& text)
            {
                l.setText (text, juce::dontSendNotification);
                l.setFont (juce::FontOptions (11.0f).withName (mono));
                l.setJustificationType (juce::Justification::centredLeft);
                l.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));
                addAndMakeVisible (l);
            };
            juce::String tailAtxt;                                   // annotates the version line
            if (kBuildCount > 0) tailAtxt << mid << kBuildCount << " commits";
            if (kGitDirty)       tailAtxt << mid << "dirty";
            info (tailA, tailAtxt);
            info (tailB, juce::String ("  build ") + juce::String (kBuildNumber));   // annotates the commit line
            info (line3, pluginFormat + mid + kOS + " " + kArch + mid + kBuilder);
            info (coreLead, "core ");
            info (coreTail, juce::String (kCoreVersion).fromFirstOccurrenceOf (" ", true, false));   // " (local)" or ""

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

            setSize (300, 248);
        }

        // Brand title: [orbit mark] OrbitCab (Michroma), mirroring the window header. Drawn (not a
        // Label) so the mark + wordmark share the exact renderer from ui/BrandMark.h.
        void paint (juce::Graphics& g) override
        {
            const auto a = titleArea.toFloat();
            const float d  = a.getHeight() * 0.92f;
            const float cy = a.getCentreY();
            orbitcab::brand::drawOrbit (g, a.getX() + d * 0.5f, cy, d);

            const auto wf = orbitcab::brand::wordmarkFont (brandTypeface, a.getHeight() * 0.66f);
            g.setFont (wf);
            g.setColour (juce::Colour (OrbitCabLookAndFeel::kText));
            const float baseline = cy + (wf.getAscent() - wf.getDescent()) * 0.5f;
            g.drawSingleLineText ("OrbitCab", juce::roundToInt (a.getX() + d + 7.0f), juce::roundToInt (baseline));
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (14, 12);
            titleArea = r.removeFromTop (26);           // [orbit] OrbitCab — drawn in paint()
            link.setBounds (r.removeFromTop (16));
            r.removeFromTop (5);

            // Info rows: a GitHub link (fitted width) + a trailing plain label.
            auto rowV = r.removeFromTop (16);
            verLink.setBounds    (rowV.removeFromLeft (verLink.getWidth()));
            tailA.setBounds      (rowV);
            auto rowC = r.removeFromTop (16);
            commitLink.setBounds (rowC.removeFromLeft (commitLink.getWidth()));
            tailB.setBounds      (rowC);
            line3.setBounds      (r.removeFromTop (16));
            auto rowK = r.removeFromTop (16);
            coreLead.setBounds   (rowK.removeFromLeft ((int) orbitcab::brand::textWidth (coreLead.getFont(), coreLead.getText()) + 3));
            coreLink.setBounds   (rowK.removeFromLeft (coreLink.getWidth()));
            coreTail.setBounds   (rowK);

            r.removeFromTop (7);
            check.setBounds    (r.removeFromTop (26));
            r.removeFromTop (4);
            result.setBounds   (r.removeFromTop (18));
            download.setBounds (r.removeFromTop (16));
            note.setBounds     (r.removeFromBottom (14));
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
        juce::Typeface::Ptr   brandTypeface;                // Michroma for the title (from the editor; bold fallback if null)
        juce::Rectangle<int>  titleArea;                    // where paint() draws [orbit] OrbitCab
        juce::Label           result, note, tailA, tailB, line3, coreLead, coreTail;
        juce::HyperlinkButton link, download, verLink, commitLink, coreLink;
        juce::TextButton      check;
    };

    void showPopup()
    {
        auto panel = std::make_unique<Panel> (checker, *this, format, brandTypeface);

        // Parent the call-out to the editor, NOT the desktop (the nullptr overload). A
        // desktop call-out outlives the editor: close the plugin window with it open and it
        // orphans on screen with no way to dismiss it. As a child of the top-level editor it
        // is destroyed with the window. `areaToPointTo` must be in the parent's coordinates.
        if (auto* top = getTopLevelComponent())
            juce::CallOutBox::launchAsynchronously (std::move (panel),
                                                    top->getLocalArea (this, getLocalBounds()), top);
        else
            juce::CallOutBox::launchAsynchronously (std::move (panel), getScreenBounds(), nullptr);
    }

    orbitcab::UpdateChecker& checker;
    juce::String            format;          // running plugin format (VST3 / AU / CLAP / Standalone)
    juce::Typeface::Ptr     brandTypeface;   // Michroma for the popover title (set by the editor)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VersionBadge)
};
