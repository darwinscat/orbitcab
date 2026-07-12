// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa Lafoks <alisa@darwinscat.com>. Part of OrbitCab — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace orbitcab::ui
{

//==============================================================================
// SnapshotButton — one A/B/C/D compare-register button. On top of a stock TextButton
// (left-click = recall, wired by the editor via onClick) it owns the register-COPY gestures:
//
//   • right-click  → onPopup (the editor's copy menu). The gate must live HERE, not in a mouse
//     listener: juce::Button fires its click for ANY mouse button, so an unfiltered right-click
//     would recall the register underneath the menu.
//   • drag onto a sibling → that sibling's onCopyDrop (copy this register there, one undoable
//     edit in the target). A finished drag swallows the release — it must not double as a click.
//   • drop target for a sibling's drag, with an "orbitDropHover" ring painted by the LookAndFeel
//     while a compatible drag hovers.
//
// The editor is the juce::DragAndDropContainer; the drag payload is dragTag(index).
class SnapshotButton final : public juce::TextButton,
                             public juce::DragAndDropTarget
{
public:
    SnapshotButton() = default;

    void setRegisterIndex (int i)                        { index = i; }
    static juce::String dragTag (int i)                  { return "orbitcab-snapshot:" + juce::String (i); }

    std::function<void()>                  onPopup;      // right-click → the editor's copy menu
    std::function<void (int from, int to)> onCopyDrop;   // a sibling was dropped here → copy from → to

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onPopup) onPopup(); return; }   // no press, no recall
        TextButton::mouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging && ! e.mods.isPopupMenu() && e.getDistanceFromDragStart() > 6)
            if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor (this))
            {
                dragging = true;
                setState (buttonNormal);    // release the pressed look — the drop is the action now
                dnd->startDragging (dragTag (index), this);
            }
        if (! dragging)
            TextButton::mouseDrag (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // A popup release or a finished drag must not fall through to Button::mouseUp — it would
        // fire the click and recall this register on top of the menu / the copy.
        if (dragging)              { dragging = false; return; }
        if (e.mods.isPopupMenu())  return;
        TextButton::mouseUp (e);
    }

    // ---- DragAndDropTarget (a sibling A/B/C/D drag) ----
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        const auto s = d.description.toString();
        return s.startsWith ("orbitcab-snapshot:") && s.getTrailingIntValue() != index;
    }
    void itemDragEnter (const SourceDetails&)   override { setDropHover (true); }
    void itemDragExit  (const SourceDetails&)   override { setDropHover (false); }
    void itemDropped   (const SourceDetails& d) override
    {
        setDropHover (false);
        if (onCopyDrop)
            onCopyDrop (d.description.toString().getTrailingIntValue(), index);
    }

private:
    void setDropHover (bool h) { getProperties().set ("orbitDropHover", h); repaint(); }

    int  index    = 0;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnapshotButton)
};

} // namespace orbitcab::ui
