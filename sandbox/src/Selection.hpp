#pragma once

#include <cstddef>
#include <vector>

#include "SceneTypes.hpp"  // Entity

// Which entities are selected -- and the invariant that keeps that answer
// consistent.
//
// The editor needs two levels of it at once. The ACTIVE object is the one that
// drives the Inspector, the gizmo and every single-object path; the SET is what
// box-select and the batch operations act on. The set is EMPTY for a plain
// single selection, so the many code paths that only ever care about one object
// can ignore it entirely -- that is the reason for the split, not an accident.
//
// The invariant: if the set is non-empty it contains the active object's id and
// only ids that still exist. It used to be restored once per frame by a repair
// pass, because ~20 sites open-coded "select this entity" as a bare assignment
// and none of them cleared the set. Those sites call select() now, so the
// invariant holds at the point of change; normalize() stayed on as the safety
// net for the paths that still move the index by hand (undo, deletion).
//
// One asymmetry worth knowing before reading the code: the active object is an
// INDEX into the document's entity list, while the set holds entity IDs, which
// survive reordering and undo. That is what the surrounding editor reads, so
// this class keeps it rather than papering over it -- but it is the only place
// that has to know, because select() takes an id and index() hands back an
// index.
class Selection {
public:
    // The entity list it selects from -- not the whole Document, because the
    // list is all it needs and that keeps it usable from the harnesses, which
    // have entities without a document around them.
    explicit Selection(const std::vector<Entity>& entities) : m_entities(entities) {}

    // --- Reading -------------------------------------------------------------
    // The active object as a list index; -1 when nothing is selected. Prefer
    // valid() over comparing this against entities.size() at the call site.
    int  index() const { return m_active; }
    bool valid() const;
    // The active object's id, or -1. Survives reordering; index() does not.
    int  activeId() const;

    // The effective selection as ids: the set if there is one, else just the
    // active object (empty when nothing is selected).
    std::vector<int> ids() const;
    // The raw multi-set. Empty for a plain single selection -- ask count() or
    // ids() unless you specifically mean "is this a MULTI-selection".
    const std::vector<int>& multi() const { return m_multi; }
    bool        contains(int id) const;
    std::size_t count() const;

    // --- Changing ------------------------------------------------------------
    // Plain click: select exactly this entity, dropping any set.
    void select(int id);
    // The same, for the callers that already hold an index (a raycast hit).
    void selectIndex(int i);
    // Ctrl+click: toggle `id` in or out. The clicked object becomes active;
    // removing the active one promotes another survivor.
    void toggle(int id);
    // Box-select: add a batch, keeping what was already there. The last one
    // becomes active.
    void addMany(const std::vector<int>& ids);
    void clear();

    // The safety net, run once per frame: drop ids whose entities are gone, and
    // collapse the set if something moved the active object out of it.
    void normalize();

private:
    // A one-element set is a plain single selection, not a multi-selection.
    // Collapsing it here means no caller ever has to special-case that.
    void collapseIfSingle();

    // Index of `id` in the list, or -1. The same scan Document does; kept here
    // so the class owns its one lookup rather than borrowing a document for it.
    int indexOf(int id) const;

    const std::vector<Entity>& m_entities;
    int              m_active = -1;
    std::vector<int> m_multi;
};
