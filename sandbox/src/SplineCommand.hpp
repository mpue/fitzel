#pragma once

#include <utility>

#include "Command.hpp"
#include "SplineSystem.hpp"

// Spline (fence / wall / track) edits as undoable commands, so laying a path,
// dragging a point and tuning a style join the same Ctrl+Z timeline as entity
// edits. The mirror of RoadCommand.hpp, and for the same reason: the generic
// command layer knows about the Document and nothing else, and the paths are not
// in the Document -- they hang off SplineSystem, which main owns for the session.
//
// The SplineSystem pointer is borrowed: main owns both it and the history, it
// outlives every command that names it, and commands never touch it in their
// destructor.
class SplineShapeCmd : public Command {
public:
    SplineShapeCmd(SplineSystem& sys, SplineSystem::Snapshot before,
                   SplineSystem::Snapshot after, const char* label)
        : m_sys(&sys), m_before(std::move(before)), m_after(std::move(after)),
          m_label(label) {}

    void redo(Document&) override { m_sys->restore(m_after); }
    void undo(Document&) override { m_sys->restore(m_before); }
    const char* name() const override { return m_label; }

    // A drag that ends where it started, or a slider nudged back to its old
    // value, should not land on the undo stack at all.
    bool trivial() const { return m_before.paths == m_after.paths; }

private:
    SplineSystem*          m_sys;
    SplineSystem::Snapshot m_before, m_after;
    const char*            m_label; // static string ("Move point", ...)
};
