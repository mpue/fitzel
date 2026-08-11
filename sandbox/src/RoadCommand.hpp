#pragma once

#include <utility>

#include "Command.hpp"
#include "RoadSystem.hpp"

// Road shape edits as undoable commands, so placing, dragging, raising and
// deleting waypoints join the same Ctrl+Z timeline as entity edits instead of
// being the one corner of the editor you cannot take back.
//
// It lives in its own header rather than in Command.hpp: the generic command
// layer knows about the Document and nothing else, and roads are not in the
// Document -- the mesh, collider and graded corridor hang off RoadSystem, which
// main owns for the whole session.

// Before/after snapshot of the road's shape (points, heights, bridges, closed).
// A road is a few hundred small values, so snapshotting it whole is cheaper in
// code than describing each edit as a delta -- and it cannot get the bookkeeping
// wrong the way an index-fixup delta could (a deleted point shifts every bridge
// index after it).
//
// The RoadSystem pointer is borrowed. main owns the road and the history, and
// the road outlives every command that names it; commands never touch it in
// their destructor, so a torn-down editor is safe either way.
class RoadShapeCmd : public Command {
public:
    RoadShapeCmd(RoadSystem& road, RoadSystem::Shape before,
                 RoadSystem::Shape after, const char* label)
        : m_road(&road), m_before(std::move(before)), m_after(std::move(after)),
          m_label(label) {}

    void redo(Document&) override { m_road->setShape(m_after); }
    void undo(Document&) override { m_road->setShape(m_before); }
    const char* name() const override { return m_label; }

    // A drag that ends where it started, or a slider nudged back to its old
    // value, should not land on the undo stack at all.
    bool trivial() const {
        return m_before.points  == m_after.points &&
               m_before.lifts   == m_after.lifts  &&
               m_before.banks   == m_after.banks  &&
               m_before.closed  == m_after.closed &&
               m_before.sideObjects == m_after.sideObjects &&
               m_before.biomes      == m_after.biomes &&
               m_before.loops   == m_after.loops  &&
               m_before.bridges.size() == m_after.bridges.size() &&
               std::equal(m_before.bridges.begin(), m_before.bridges.end(),
                          m_after.bridges.begin(),
                          [](const RoadSystem::BridgeSpec& x,
                             const RoadSystem::BridgeSpec& y) {
                              return x.a == y.a && x.b == y.b;
                          });
    }

private:
    RoadSystem*        m_road;
    RoadSystem::Shape  m_before, m_after;
    const char*        m_label; // static string ("Move point", ...)
};
