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
// PreampManager — the gear-panel "Manage library…" pop-over for the PREAMP stage. An exact sibling
// of PowerampManager (same merged factory + user list, Add / Remove / Reveal / Get models, drag-drop
// import, click-to-select), but pointed at the PRE-amp library (proc.preampLibrary() / preampDir() /
// "preampSel"). The two managers are deliberately separate so the preamp and poweramp libraries stay
// independent. The one onLibraryChanged hook lets the editor rebuild its PREAMP selector after a change.
//==============================================================================
class PreampManager final : public juce::Component,
                            public juce::FileDragAndDropTarget
{
public:
    PreampManager (OrbitCabAudioProcessor& processor, std::function<void()> onLibraryChanged)
        : proc (processor), onChanged (std::move (onLibraryChanged))
    {
        title.setText ("Preamp library", juce::dontSendNotification);
        title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8ee));
        addAndMakeVisible (title);

        subtitle.setText (juce::String::fromUTF8 ("Models the PREAMP selector lists. Drop .nam files here, or Add\xe2\x80\xa6"),
                          juce::dontSendNotification);
        subtitle.setFont (juce::FontOptions (11.0f));
        subtitle.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa2));
        subtitle.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (subtitle);

        viewport.setScrollBarsShown (true, false);
        viewport.setViewedComponent (&listContent, false);
        addAndMakeVisible (viewport);

        addBtn.setButtonText (juce::String::fromUTF8 ("Add\xe2\x80\xa6"));
        addBtn.setTooltip ("Copy one or more .nam captures into your preamp library.");
        addBtn.onClick = [this] { addClicked(); };
        addAndMakeVisible (addBtn);

        revealBtn.setButtonText ("Reveal folder");
        revealBtn.setTooltip ("Open the per-machine preamp folder in " +
                              juce::String (
                              #if JUCE_MAC
                                  "Finder"
                              #elif JUCE_WINDOWS
                                  "Explorer"
                              #else
                                  "your file manager"
                              #endif
                                  ) + ".");
        revealBtn.onClick = [this] { proc.appPreferences().preampDir().revealToUser(); };
        addAndMakeVisible (revealBtn);

        getBtn.setButtonText (juce::String::fromUTF8 ("Get models\xe2\x80\xa6"));
        getBtn.setTooltip ("Open tone3000.com to download preamp captures (.nam) you have the right to use.");
        getBtn.onClick = [this] { getModelsClicked(); };
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
    // A compact descriptor of the variant's parsed sub-dimensions (channel / gain / boost), shown
    // as faint text to the right of the name. Empty → an em-dash (a plain, single-variant model).
    static juce::String describeVariant (const orbitcab::PreampEntry& e)
    {
        juce::StringArray parts;
        if (e.channel > 0) parts.add (e.channelLabel.isNotEmpty() ? e.channelLabel : "ch" + juce::String (e.channel));
        if (e.hours   > 0) parts.add (juce::String (e.hours) + "h");
        if (e.boost)       parts.add ("boost");
        return parts.isEmpty() ? juce::String::fromUTF8 ("\xe2\x80\x94") : parts.joinIntoString (" ");
    }

    //==============================================================================
    // One library entry's row: name + variant descriptor (channel/gain/boost) + source tag +
    // (user) Remove. Clicking the row body selects that model; the Remove button consumes its clicks.
    struct Row final : public juce::Component
    {
        Row (orbitcab::PreampEntry entry, bool isSelected,
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

            // Variant descriptor (left of the tag): channel / gain / boost.
            auto badge = body.removeFromRight (118);
            g.setColour (juce::Colour (0xff6a6a72));
            g.setFont (juce::FontOptions (10.5f));
            g.drawText (describeVariant (e), badge, juce::Justification::centredRight);

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

        orbitcab::PreampEntry              e;
        bool                               selected = false, hover = false;
        std::function<void()>              select;
        std::unique_ptr<juce::TextButton>  remove;   // user rows only
    };

    //==============================================================================
    void rebuild()
    {
        rows.clear();
        listContent.removeAllChildren();

        const auto lib = proc.preampLibrary();
        const auto sel = proc.selectedPreampId();
        for (const auto& e : lib)
        {
            const auto id = e.id;
            auto row = std::make_unique<Row> (e, e.id == sel,
                [this, id] { proc.selectPreamp (id); refresh(); },
                [this, id] { removeId (id); });
            listContent.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }

        const int visRows = juce::jlimit (1, kVisibleRows, (int) rows.size());
        const int listH   = (rows.empty() ? 2 : visRows) * kRowH;
        setSize (kWidth, 12 + 20 + 4 + 30 + 6 + listH + 8 + 30 + 12);   // CallOutBox re-fits via childBoundsChanged
        layoutRows();
        repaint();
    }

    // Refresh AFTER the current event returns (see PowerampManager::refresh for the delete-this rationale).
    void refresh()
    {
        juce::Component::SafePointer<PreampManager> self (this);
        juce::MessageManager::callAsync ([self]
        {
            if (self == nullptr) return;
            if (self->onChanged) self->onChanged();   // editor rebuilds its PREAMP selector
            self->rebuild();                          // re-render the manager rows + re-fit the call-out
        });
    }

    void layoutRows()
    {
        const int contentH = (int) rows.size() * kRowH;
        const bool needsBar = contentH > viewport.getHeight();
        const int  w        = juce::jmax (0, viewport.getWidth() - (needsBar ? viewport.getScrollBarThickness() : 0));
        listContent.setSize (w, contentH);
        for (int i = 0; i < (int) rows.size(); ++i)
            rows[(size_t) i]->setBounds (0, i * kRowH, w, kRowH);
    }

    // "Get models…" → download sources for preamp captures. tone3000 hosts NAM captures.
    void getModelsClicked()
    {
        juce::PopupMenu m;
        m.addSectionHeader ("Download preamp captures (.nam)");
        m.addItem (1, juce::String::fromUTF8 ("Browse tone3000.com\xe2\x80\xa6"));
        m.addItem (2, juce::String::fromUTF8 ("Search preamp captures \xe2\x80\x94 tone3000"));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (getBtn),
            [] (int r)
            {
                const char* url = r == 1 ? "https://www.tone3000.com"
                                : r == 2 ? "https://www.tone3000.com/tones?q=preamp"
                                         : nullptr;
                if (url != nullptr)
                    juce::URL (url).launchInDefaultBrowser();
            });
    }

    void addClicked()
    {
        chooser = std::make_unique<juce::FileChooser> ("Add preamp captures",
                                                       proc.appPreferences().preampDir(), "*.nam;*.namz");
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
            if (f.existsAsFile() && f.hasFileExtension ("nam;namz") && proc.importPreamp (f) != juce::File())
                any = true;
        }
        if (any)
            refresh();                    // editor rebuilds its selector (library grew → PREAMP appears)
    }

    void removeId (const juce::String& id)
    {
        if (proc.removePreamp (id))       // moves the user .nam to the Trash; resets selection if it was active
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreampManager)
};
