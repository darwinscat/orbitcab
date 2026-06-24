// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "OrbitCabLookAndFeel.h"

#include <functional>
#include <memory>
#include <vector>

//==============================================================================
// PowerampManager — the gear-panel "Manage library…" pop-over: the one place to curate the
// USER poweramp library (the per-machine, system-wide AppPreferences::powerampDir() folder the
// POWERAMP selector merges in alongside the embedded factory captures).
//
//   • Lists the MERGED library (factory first, then user) so it's obvious what the selector shows.
//   • Factory rows are read-only (embedded in the build — nothing to delete); a "Factory" tag, no
//     Remove button. User rows carry a "User" tag + a Remove button (moves the .nam to the Trash).
//   • Add…  — a FileChooser (multi-select) that copies the chosen .nam(s) into powerampDir; the
//     same import also runs on a drag-drop of .nam files onto the panel.
//   • Reveal — opens powerampDir in Finder/Explorer (drop captures in by hand, then they appear here).
//   • Clicking a row makes that model the active selection ("ampSel"); the active one is highlighted.
//     (Selection is recorded even with the amp powered off — it loads when POWERAMP is switched on.)
//
// Holds the processor (like SlotComponent) — it IS the library, so it reads/imports/removes directly;
// the single onLibraryChanged hook lets the editor rebuild its bottom-strip selector after a change.
// Launched in a CallOutBox parented to the editor (so it can't outlive the window).
//==============================================================================
class PowerampManager final : public juce::Component,
                              public juce::FileDragAndDropTarget
{
public:
    PowerampManager (OrbitCabAudioProcessor& processor, std::function<void()> onLibraryChanged)
        : proc (processor), onChanged (std::move (onLibraryChanged))
    {
        title.setText ("Poweramp library", juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8ee));
        addAndMakeVisible (title);

        subtitle.setText (juce::String::fromUTF8 ("Models the POWERAMP selector lists. Drop .nam files here, or Add\xe2\x80\xa6"),
                          juce::dontSendNotification);
        subtitle.setFont (juce::FontOptions (11.0f));
        subtitle.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa2));
        subtitle.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (subtitle);

        viewport.setScrollBarsShown (true, false);
        viewport.setViewedComponent (&listContent, false);
        addAndMakeVisible (viewport);

        addBtn.setButtonText (juce::String::fromUTF8 ("Add\xe2\x80\xa6"));
        addBtn.setTooltip ("Copy one or more .nam captures into your poweramp library.");
        addBtn.onClick = [this] { addClicked(); };
        addAndMakeVisible (addBtn);

        revealBtn.setButtonText ("Reveal folder");
        revealBtn.setTooltip ("Open the per-machine poweramp folder in " +
                              juce::String (
                              #if JUCE_MAC
                                  "Finder"
                              #elif JUCE_WINDOWS
                                  "Explorer"
                              #else
                                  "your file manager"
                              #endif
                                  ) + ".");
        revealBtn.onClick = [this] { proc.appPreferences().powerampDir().revealToUser(); };
        addAndMakeVisible (revealBtn);

        rebuild();   // populate rows + size the panel
    }

    //==============================================================================
    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 12);
        title.setBounds    (r.removeFromTop (20));
        r.removeFromTop (4);
        subtitle.setBounds (r.removeFromTop (30));
        r.removeFromTop (6);

        auto toolbar = r.removeFromBottom (30);
        addBtn.setBounds    (toolbar.removeFromLeft (84));
        toolbar.removeFromLeft (8);
        revealBtn.setBounds (toolbar.removeFromLeft (108));
        r.removeFromBottom (8);

        viewport.setBounds (r);
        layoutRows();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (OrbitCabLookAndFeel::kPanel));

        // Framed list well behind the viewport so an empty library still reads as a drop target.
        g.setColour (juce::Colour (0xff141418));
        g.fillRoundedRectangle (viewport.getBounds().toFloat(), 4.0f);
        g.setColour (juce::Colour (dragOver ? OrbitCabLookAndFeel::kAccent : 0x22ffffff));
        g.drawRoundedRectangle (viewport.getBounds().toFloat().reduced (0.5f), 4.0f, dragOver ? 1.5f : 1.0f);

        if (rows.empty())
        {
            g.setColour (juce::Colour (0xff70707a));
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (juce::String::fromUTF8 ("No models yet \xe2\x80\x94 Add\xe2\x80\xa6 or drop .nam files here."),
                        viewport.getBounds(), juce::Justification::centred);
        }
    }

    //==============================================================================
    // FileDragAndDropTarget — drop .nam files anywhere on the panel to import them.
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (const auto& f : files)
            if (f.endsWithIgnoreCase (".nam"))
                return true;
        return false;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true;  repaint(); }
    void fileDragExit  (const juce::StringArray&)           override { dragOver = false; repaint(); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        dragOver = false;
        importFiles (files);
        repaint();
    }

private:
    //==============================================================================
    // One library entry's row: name + mode badge (PP / SE / —) + source tag + (user) Remove.
    // Clicking the row body selects that model; the Remove button consumes its own clicks.
    struct Row final : public juce::Component
    {
        Row (orbitcab::PowerampEntry entry, bool isSelected,
             std::function<void()> onSelect, std::function<void()> onRemove)
            : e (std::move (entry)), selected (isSelected), select (std::move (onSelect))
        {
            if (! e.factory)
            {
                remove = std::make_unique<juce::TextButton> ("Remove");
                remove->setTooltip ("Move this capture to the Trash (removes it from every instance).");
                remove->setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe06a6a));
                remove->onClick = std::move (onRemove);
                addAndMakeVisible (*remove);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (8, 4);
            if (remove != nullptr)
                remove->setBounds (r.removeFromRight (64).reduced (0, 1));
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            if (selected)
            {
                g.setColour (juce::Colour (OrbitCabLookAndFeel::kAccent).withAlpha (0.18f));
                g.fillRect (r);
                g.setColour (juce::Colour (OrbitCabLookAndFeel::kAccent));
                g.fillRect (r.removeFromLeft (3));               // accent spine on the active model
            }
            else if (hover)
            {
                g.setColour (juce::Colour (0x14ffffff));
                g.fillRect (r);
            }

            auto body = getLocalBounds().reduced (8, 0);
            if (remove != nullptr) body.removeFromRight (64 + 8);

            // Source tag (right): Factory = embedded, User = your folder.
            auto tagArea = body.removeFromRight (58);
            g.setColour (juce::Colour (0xff80808a));
            g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
            g.drawText (e.factory ? "FACTORY" : "USER", tagArea, juce::Justification::centredRight);

            // Mode badge (left of the tag): PP / SE / — (Other).
            auto badge = body.removeFromRight (40);
            g.setColour (juce::Colour (0xff6a6a72));
            g.drawText (e.cat == orbitcab::PowerampCat::pushPull    ? "PP"
                      : e.cat == orbitcab::PowerampCat::singleEnded ? "SE"
                                                                    : juce::String::fromUTF8 ("\xe2\x80\x94"),
                        badge, juce::Justification::centred);

            // Name (fills the rest).
            g.setColour (juce::Colour (selected ? 0xfff0f0f4 : OrbitCabLookAndFeel::kText));
            g.setFont (juce::FontOptions (12.5f, selected ? juce::Font::bold : juce::Font::plain));
            g.drawText (e.name, body.withTrimmedLeft (4), juce::Justification::centredLeft, true);
        }

        // Clicks on the row body (not the Remove button) select this model.
        void mouseUp (const juce::MouseEvent& ev) override
        {
            if (ev.mouseWasClicked() && select)
                select();
        }
        void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
        void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }

        orbitcab::PowerampEntry            e;
        bool                               selected = false, hover = false;
        std::function<void()>              select;
        std::unique_ptr<juce::TextButton>  remove;   // user rows only
    };

    //==============================================================================
    void rebuild()
    {
        rows.clear();
        listContent.removeAllChildren();

        const auto lib = proc.powerampLibrary();
        const auto sel = proc.selectedPowerampId();
        for (const auto& e : lib)
        {
            const auto id = e.id;
            auto row = std::make_unique<Row> (e, e.id == sel,
                [this, id] { proc.selectPoweramp (id); refresh(); },
                [this, id] { removeId (id); });
            listContent.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }

        // Grow with the list up to a cap, then the viewport scrolls (kVisibleRows tall).
        const int visRows = juce::jlimit (1, kVisibleRows, (int) rows.size());
        const int listH   = (rows.empty() ? 2 : visRows) * kRowH;
        setSize (kWidth, 12 + 20 + 4 + 30 + 6 + listH + 8 + 30 + 12);   // CallOutBox re-fits via childBoundsChanged
        layoutRows();   // a rebuild that doesn't change the panel height won't fire resized() — lay out anyway
        repaint();
    }

    // Refresh AFTER the current event returns. A row/Remove handler that mutates the library would
    // otherwise call rebuild() — which frees the very Row (and its std::function) still on the stack
    // (delete-this-from-my-own-callback). Defer it, guarded by a SafePointer (the call-out may close).
    void refresh()
    {
        juce::Component::SafePointer<PowerampManager> self (this);
        juce::MessageManager::callAsync ([self]
        {
            if (self == nullptr) return;
            if (self->onChanged) self->onChanged();   // editor rebuilds its bottom-strip selector
            self->rebuild();                          // re-render the manager rows + re-fit the call-out
        });
    }

    void layoutRows()
    {
        // Derive the row width from the viewport's OWN bounds (always valid once setBounds ran),
        // minus the vertical scrollbar when the content overflows. getMaximumVisibleWidth() read 0
        // at first-layout time → rows came out zero-wide (a blank well under a present scrollbar).
        const int contentH = (int) rows.size() * kRowH;
        const bool needsBar = contentH > viewport.getHeight();
        const int  w        = juce::jmax (0, viewport.getWidth() - (needsBar ? viewport.getScrollBarThickness() : 0));
        listContent.setSize (w, contentH);
        for (int i = 0; i < (int) rows.size(); ++i)
            rows[(size_t) i]->setBounds (0, i * kRowH, w, kRowH);
    }

    void addClicked()
    {
        chooser = std::make_unique<juce::FileChooser> ("Add poweramp captures",
                                                       proc.appPreferences().powerampDir(), "*.nam");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
            [this] (const juce::FileChooser& fc)
            {
                juce::StringArray paths;
                for (const auto& f : fc.getResults())
                    paths.add (f.getFullPathName());
                importFiles (paths);
            });
    }

    void importFiles (const juce::StringArray& paths)
    {
        bool any = false;
        for (const auto& p : paths)
        {
            const juce::File f (p);
            if (f.existsAsFile() && f.hasFileExtension ("nam") && proc.importPoweramp (f) != juce::File())
                any = true;
        }
        if (any)
            refresh();                    // editor rebuilds its selector (library grew → POWERAMP appears)
    }

    void removeId (const juce::String& id)
    {
        if (proc.removePoweramp (id))     // moves the user .nam to the Trash; resets selection if it was active
            refresh();
    }

    //==============================================================================
    static constexpr int kWidth       = 380;
    static constexpr int kRowH        = 30;
    static constexpr int kVisibleRows = 7;    // taller libraries scroll inside the viewport

    OrbitCabAudioProcessor&  proc;
    std::function<void()>    onChanged;

    juce::Label      title, subtitle;
    juce::Viewport   viewport;
    juce::Component  listContent;                  // sized to rows; lives inside the viewport
    std::vector<std::unique_ptr<Row>> rows;
    juce::TextButton addBtn, revealBtn;
    std::unique_ptr<juce::FileChooser> chooser;
    bool             dragOver = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerampManager)
};
