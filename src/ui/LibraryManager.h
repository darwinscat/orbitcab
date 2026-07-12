// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "OrbitCabLookAndFeel.h"

#include <functional>
#include <memory>
#include <vector>

//==============================================================================
// LibraryManager<Traits> — the gear-panel "Manage library…" pop-over: the one place to curate a
// stage's USER model library (the per-machine, system-wide AppPreferences folder its selector
// merges in alongside the embedded factory captures). One panel serves both NAM stages; everything
// stage-specific — which library / folder / selection hooks on the processor, the wording, the
// per-entry variant badge, the "Get models…" sources — comes in through a small static Traits
// struct (PreampManagerTraits / PowerampManagerTraits, next to their aliases). The two libraries
// stay deliberately independent: distinct traits, distinct instantiations, zero shared state.
//
//   • Lists the MERGED library (factory first, then user) so it's obvious what the selector shows.
//   • Factory rows are read-only (embedded in the build — nothing to delete); a "Factory" tag, no
//     Remove button. User rows carry a "User" tag + a Remove button (moves the .nam to the Trash).
//   • Add…  — a FileChooser (multi-select) that copies the chosen .nam(s) into the user folder; the
//     same import also runs on a drag-drop of .nam files onto the panel.
//   • Reveal — opens the folder in Finder/Explorer (drop captures in by hand, then they appear here).
//   • Clicking a row makes that model the active selection; the active one is highlighted.
//     (Selection is recorded even with the amp powered off — it loads when the stage is switched on.)
//
// Holds the processor (like SlotComponent) — it IS the library, so it reads/imports/removes directly;
// the single onLibraryChanged hook lets the editor rebuild that stage's selector after a change.
// Launched in a CallOutBox parented to the editor (so it can't outlive the window).
//==============================================================================
template <typename Traits>
class LibraryManager final : public juce::Component,
                             public juce::FileDragAndDropTarget
{
    using Entry = typename Traits::Entry;

public:
    LibraryManager (OrbitCabAudioProcessor& processor, std::function<void()> onLibraryChanged)
        : proc (processor), onChanged (std::move (onLibraryChanged))
    {
        title.setText (Traits::title, juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8ee));
        addAndMakeVisible (title);

        subtitle.setText (juce::String::fromUTF8 (Traits::subtitle), juce::dontSendNotification);
        subtitle.setFont (juce::FontOptions (11.0f));
        subtitle.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa2));
        subtitle.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (subtitle);

        viewport.setScrollBarsShown (true, false);
        viewport.setViewedComponent (&listContent, false);
        addAndMakeVisible (viewport);

        addBtn.setButtonText (juce::String::fromUTF8 ("Add\xe2\x80\xa6"));
        addBtn.setTooltip (Traits::addTooltip);
        addBtn.onClick = [this] { addClicked(); };
        addAndMakeVisible (addBtn);

        revealBtn.setButtonText ("Reveal folder");
        revealBtn.setTooltip (Traits::revealTooltipStart +
                              juce::String (
                              #if JUCE_MAC
                                  "Finder"
                              #elif JUCE_WINDOWS
                                  "Explorer"
                              #else
                                  "your file manager"
                              #endif
                                  ) + ".");
        revealBtn.onClick = [this] { Traits::userDir (proc).revealToUser(); };
        addAndMakeVisible (revealBtn);

        getBtn.setButtonText (juce::String::fromUTF8 ("Get models\xe2\x80\xa6"));
        getBtn.setTooltip (Traits::getTooltip);
        getBtn.onClick = [this] { Traits::showGetModelsMenu (getBtn); };
        addAndMakeVisible (getBtn);

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
        addBtn.setBounds    (toolbar.removeFromLeft (76));
        toolbar.removeFromLeft (6);
        revealBtn.setBounds (toolbar.removeFromLeft (104));
        toolbar.removeFromLeft (6);
        getBtn.setBounds    (toolbar.removeFromLeft (100));
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
            if (f.endsWithIgnoreCase (".nam") || f.endsWithIgnoreCase (".namz"))
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
    // One library entry's row: name + the stage's variant badge (Traits::paintVariant) + source
    // tag + (user) Remove. Clicking the row body selects that model; Remove consumes its clicks.
    struct Row final : public juce::Component
    {
        Row (Entry entry, bool isSelected,
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

            // Variant badge (left of the tag) — stage-specific: the traits trim their badge area
            // off `body` and draw into it (preamp: channel / gain / boost; poweramp: PP / SE / —).
            Traits::paintVariant (g, e, body);

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

        Entry                              e;
        bool                               selected = false, hover = false;
        std::function<void()>              select;
        std::unique_ptr<juce::TextButton>  remove;   // user rows only
    };

    //==============================================================================
    void rebuild()
    {
        rows.clear();
        listContent.removeAllChildren();

        const auto lib = Traits::library (proc);
        const auto sel = Traits::selectedId (proc);
        for (const auto& e : lib)
        {
            const auto id = e.id;
            auto row = std::make_unique<Row> (e, e.id == sel,
                [this, id] { Traits::select (proc, id); refresh(); },
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
        juce::Component::SafePointer<LibraryManager> self (this);
        juce::MessageManager::callAsync ([self]
        {
            if (self == nullptr) return;
            if (self->onChanged) self->onChanged();   // editor rebuilds this stage's selector
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
        // Lifetime audit (JUCE 8.0.14): capturing raw `this` below is safe — the callback's life is
        // bounded by the FileChooser, which is a member: ~FileChooser() nulls asyncCallback and
        // finished() std::exchange's it, so once this panel (and `chooser` with it) dies the lambda
        // can never fire; the macOS-native pimpl additionally guards via SafePointer<Native> and
        // closes an open panel in its dtor. Reassigning `chooser` mid-dialog is equally handled by
        // JUCE (old panel closed, old callback cleared) — and is unreachable anyway, since the
        // chooser's modal state blocks clicks on the rest of the UI while the dialog is up.
        chooser = std::make_unique<juce::FileChooser> (Traits::chooserTitle,
                                                       Traits::userDir (proc), "*.nam;*.namz");
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
            if (f.existsAsFile() && f.hasFileExtension ("nam;namz") && Traits::importModel (proc, f) != juce::File())
                any = true;
        }
        if (any)
            refresh();                    // editor rebuilds its selector (library grew → the stage's UI appears)
    }

    void removeId (const juce::String& id)
    {
        if (Traits::removeModel (proc, id))   // moves the user .nam to the Trash; resets selection if it was active
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
    juce::TextButton addBtn, revealBtn, getBtn;
    std::unique_ptr<juce::FileChooser> chooser;
    bool             dragOver = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryManager)
};
