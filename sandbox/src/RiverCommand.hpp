#pragma once

#include <utility>

#include "Command.hpp"
#include "RiverSystem.hpp"

// Watercourse edits as undoable commands, so drawing a brook, dragging a point
// and tuning a channel join the same Ctrl+Z timeline as everything else. The
// mirror of SplineCommand.hpp / RoadCommand.hpp, and for the same reason: the
// generic command layer knows about the Document and nothing else, and the paths
// are not in the Document -- they hang off RiverSystem, which main owns.
//
// Undoing one puts the PATHS back and marks the system dirty; the bed follows on
// the next carve, which main triggers on the same bracket that pushes this. That
// split is deliberate: a command that also cut the terrain would have to snapshot
// the height field to be able to undo itself, and the whole point of deriving the
// cut is that it never has to be stored.
//
// The RiverSystem pointer is borrowed: main owns both it and the history, it
// outlives every command that names it, and commands never touch it in their
// destructor.
class RiverShapeCmd : public Command {
public:
    RiverShapeCmd(RiverSystem& sys, RiverSystem::Snapshot before,
                  RiverSystem::Snapshot after, const char* label)
        : m_sys(&sys), m_before(std::move(before)), m_after(std::move(after)),
          m_label(label) {}

    void redo(Document&) override { m_sys->restore(m_after); }
    void undo(Document&) override { m_sys->restore(m_before); }
    const char* name() const override { return m_label; }

    // A drag that ends where it started, or a slider nudged back to its old
    // value, should not land on the undo stack at all.
    bool trivial() const { return m_before.paths == m_after.paths; }

private:
    RiverSystem*          m_sys;
    RiverSystem::Snapshot m_before, m_after;
    const char*           m_label; // static string ("Move point", ...)
};
