#pragma once

#include <utility>
#include <vector>

#include "Command.hpp"
#include "UiOverlay.hpp"

// UI-overlay edits as undoable commands, so adding, moving, restyling and
// deleting overlay elements join the same Ctrl+Z timeline as everything else.
//
// Like RoadShapeCmd, the overlay is not in the Document (it hangs off the
// UiOverlay main owns for the session), so this command borrows a UiOverlay* and
// snapshots the whole element list before/after an interaction. The list is a
// handful of small structs, so a whole-list snapshot is cheaper in code than
// describing each edit as a delta -- and it can't mis-track an index the way a
// reorder/delete delta could. The UiOverlay outlives every command that names it.
class UiOverlayCmd : public Command {
public:
    UiOverlayCmd(UiOverlay& overlay, std::vector<UiElement> before,
                 std::vector<UiElement> after)
        : m_overlay(&overlay), m_before(std::move(before)), m_after(std::move(after)) {}

    void redo(Document&) override { m_overlay->setElements(m_after); }
    void undo(Document&) override { m_overlay->setElements(m_before); }
    const char* name() const override { return "UI overlay edit"; }

    // A drag that ends where it started shouldn't land on the undo stack.
    bool trivial() const { return m_before == m_after; }

private:
    UiOverlay*             m_overlay;
    std::vector<UiElement> m_before, m_after;
};
