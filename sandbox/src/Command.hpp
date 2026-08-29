#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "Document.hpp"

// One reversible edit against the Document. Concrete edits capture the minimal
// delta they need and know how to (re)apply and revert it. This is the single
// channel through which authored content changes -- the backbone that makes the
// editor safe to experiment in (the first thing a non-technical artist needs).
class Command {
public:
    virtual ~Command() = default;
    virtual void redo(Document& doc) = 0;
    virtual void undo(Document& doc) = 0;
    virtual const char* name() const = 0; // short label for the Edit menu
};

// Undo/redo history. push() executes a command and records it (clearing the redo
// branch); undo()/redo() walk the timeline. clear() drops the history (on project
// load/new, where crossing the boundary must not be undoable).
class CommandStack {
public:
    void push(std::unique_ptr<Command> c, Document& doc) {
        if (!c) return;
        c->redo(doc);
        m_undo.push_back(std::move(c));
        m_redo.clear();
        ++m_revision;
    }
    // Bank a change that is ALREADY in the document. push() runs the command's
    // redo, which is exactly right when the command IS the change -- and wrong
    // when the caller made it by hand first, which is what everything that
    // snapshots an entity before and after does. Redo then assigns that "after"
    // copy over the live entity to reach a state it is already in: a deep copy
    // of every component for nothing, and -- because assigning a ComponentList
    // destroys the old components and clones new ones -- every pointer into that
    // entity left dangling. A viewport tool that goes on drawing from the
    // component it just edited then reads freed memory, which is a crash that
    // waits for the allocator to reuse the block rather than one that happens
    // where the mistake is.
    //
    // Undo and redo are unaffected: the command still carries both snapshots, so
    // the first Ctrl+Z has a "before" to go back to and the redo after it puts
    // the "after" back the ordinary way.
    //
    // Not every already-applied caller can use this. It is right when redo()
    // only ASSIGNS state -- the entity snapshot commands. Where redo() also does
    // work (RoadShapeCmd::redo calls setShape, which flags a rebuild and
    // regenerates the side objects, city and decals) that work still has to
    // happen, so those callers stay on push().
    void pushApplied(std::unique_ptr<Command> c) {
        if (!c) return;
        m_undo.push_back(std::move(c));
        m_redo.clear();
        ++m_revision;
    }
    void undo(Document& doc) {
        if (m_undo.empty()) return;
        m_undo.back()->undo(doc);
        m_redo.push_back(std::move(m_undo.back()));
        m_undo.pop_back();
        ++m_revision;
    }
    void redo(Document& doc) {
        if (m_redo.empty()) return;
        m_redo.back()->redo(doc);
        m_undo.push_back(std::move(m_redo.back()));
        m_redo.pop_back();
        ++m_revision;
    }
    void clear() { m_undo.clear(); m_redo.clear(); ++m_revision; }

    // A counter bumped by every edit that passes through here (and by clear(),
    // which replaces the document wholesale). It never resets, so anyone who
    // remembers a value can ask "has the scene changed since?" with one integer
    // compare -- which is what lets the autosave stay silent in an idle editor
    // instead of rewriting an unchanged snapshot every few minutes.
    unsigned    revision() const { return m_revision; }

    bool        canUndo()  const { return !m_undo.empty(); }
    bool        canRedo()  const { return !m_redo.empty(); }
    const char* undoName() const { return m_undo.empty() ? "" : m_undo.back()->name(); }
    const char* redoName() const { return m_redo.empty() ? "" : m_redo.back()->name(); }

private:
    std::vector<std::unique_ptr<Command>> m_undo, m_redo;
    unsigned                              m_revision = 0;
};

// Value equality across every editable/serialized field (so a no-op edit can be
// dropped instead of polluting the undo history).
inline bool sameEntity(const Entity& a, const Entity& b) {
    return a.id == b.id && a.type == b.type &&
           a.localCenter == b.localCenter && a.localRotation == b.localRotation &&
           a.half == b.half &&
           a.parent == b.parent && a.name == b.name &&
           componentsEqual(a.components, b.components);
}

// --- Concrete commands ------------------------------------------------------

// Add an entity. Redo inserts it (at the end); undo removes it by id.
class AddEntityCmd : public Command {
public:
    explicit AddEntityCmd(Entity e) : m_entity(std::move(e)) {}
    void redo(Document& d) override { d.entities().push_back(m_entity); }
    void undo(Document& d) override {
        auto& es = d.entities();
        for (auto it = es.begin(); it != es.end(); ++it)
            if (it->id == m_entity.id) { es.erase(it); break; }
    }
    const char* name() const override { return "Add"; }
    int entityId() const { return m_entity.id; }

private:
    Entity m_entity;
};

// Delete an entity, reparenting its children onto its own parent (the editor's
// delete semantics). Undo restores the entity at its original index and the
// children's original parent.
class DeleteEntityCmd : public Command {
public:
    DeleteEntityCmd(const Document& d, int id) {
        m_index = d.indexOf(id);
        if (m_index >= 0) {
            m_entity = d.entities()[m_index];
            for (const Entity& e : d.entities())
                if (e.parent == id) m_children.push_back(e.id);
        }
    }
    void redo(Document& d) override {
        if (m_index < 0) return;
        auto& es = d.entities();
        for (Entity& e : es)
            if (e.parent == m_entity.id) e.parent = m_entity.parent;
        es.erase(es.begin() + m_index);
    }
    void undo(Document& d) override {
        if (m_index < 0) return;
        auto& es = d.entities();
        const int at = std::min<int>(m_index, static_cast<int>(es.size()));
        es.insert(es.begin() + at, m_entity);
        for (int cid : m_children)
            for (Entity& e : es)
                if (e.id == cid) e.parent = m_entity.id;
    }
    const char* name() const override { return "Delete"; }

private:
    Entity           m_entity;
    int              m_index = -1;
    std::vector<int> m_children;
};

// Add several entities as one undoable step (a scatter stamp drops a whole
// handful of objects; undoing them one by one would be misery). Insertion
// order is preserved, so parents can precede their children in the batch.
class AddEntitiesCmd : public Command {
public:
    AddEntitiesCmd(std::vector<Entity> es, const char* label)
        : m_entities(std::move(es)), m_label(label) {}
    void redo(Document& d) override {
        for (const Entity& e : m_entities) d.entities().push_back(e);
    }
    void undo(Document& d) override {
        auto& es = d.entities();
        for (auto it = m_entities.rbegin(); it != m_entities.rend(); ++it)
            for (auto e = es.begin(); e != es.end(); ++e)
                if (e->id == it->id) { es.erase(e); break; }
    }
    const char* name() const override { return m_label; }

private:
    std::vector<Entity> m_entities;
    const char*         m_label; // static string ("Scatter", ...)
};

// Delete a set of entities as one undoable step (the scatter erase brush).
// Mirrors DeleteEntityCmd's semantics per entity: a child that is NOT itself
// in the set is reparented onto its nearest surviving ancestor. Undo restores
// every entity at its original index and every reparented child's old parent.
class DeleteEntitiesCmd : public Command {
public:
    DeleteEntitiesCmd(const Document& d, const std::vector<int>& ids) {
        auto inSet = [&](int id) {
            return std::find(ids.begin(), ids.end(), id) != ids.end();
        };
        for (int id : ids) {
            const int idx = d.indexOf(id);
            if (idx >= 0) m_items.push_back({d.entities()[idx], idx});
        }
        std::sort(m_items.begin(), m_items.end(),
                  [](const Item& a, const Item& b) { return a.index < b.index; });
        // Surviving children: climb past deleted ancestors to the first kept one.
        for (const Entity& e : d.entities()) {
            if (inSet(e.id) || !inSet(e.parent)) continue;
            int np = e.parent;
            while (np >= 0 && inSet(np)) {
                const Entity* p = d.find(np);
                np = p ? p->parent : -1;
            }
            m_reparent.push_back({e.id, e.parent, np});
        }
    }
    void redo(Document& d) override {
        for (const Reparent& r : m_reparent)
            if (Entity* c = d.find(r.child)) c->parent = r.newParent;
        auto& es = d.entities();
        for (const Item& it : m_items)
            for (auto e = es.begin(); e != es.end(); ++e)
                if (e->id == it.entity.id) { es.erase(e); break; }
    }
    void undo(Document& d) override {
        auto& es = d.entities();
        for (const Item& it : m_items) { // ascending indices -> insert in order
            const int at = std::min<int>(it.index, static_cast<int>(es.size()));
            es.insert(es.begin() + at, it.entity);
        }
        for (const Reparent& r : m_reparent)
            if (Entity* c = d.find(r.child)) c->parent = r.oldParent;
    }
    const char* name() const override { return "Delete"; }
    bool empty() const { return m_items.empty(); }

private:
    struct Item     { Entity entity; int index; };
    struct Reparent { int child, oldParent, newParent; };
    std::vector<Item>     m_items;
    std::vector<Reparent> m_reparent;
};

// Swap one set of entities for another as a SINGLE undoable step: delete the
// old ones, add the new ones. A procedural generator re-running with tweaked
// parameters (see BuildingGen) replaces its whole output every time; doing that
// as two commands would make one edit cost two undos and could leave a half-
// applied scene in between. Undo unwinds in the mirror order.
class ReplaceEntitiesCmd : public Command {
public:
    ReplaceEntitiesCmd(const Document& d, const std::vector<int>& remove,
                       std::vector<Entity> add, const char* label)
        : m_del(d, remove), m_add(std::move(add), label), m_label(label) {}
    void redo(Document& d) override { m_del.redo(d); m_add.redo(d); }
    void undo(Document& d) override { m_add.undo(d); m_del.undo(d); }
    const char* name() const override { return m_label; }

private:
    DeleteEntitiesCmd m_del;
    AddEntitiesCmd    m_add;
    const char*       m_label; // static string ("Building", ...)
};

// Modify one entity in place via a before/after snapshot. One command type
// covers every per-entity field edit (transform, colour, physics, script, ...)
// because Entity is a small, cheaply-copied value. Callers snapshot the entity
// before a manipulation and after it settles, then push this.
class ModifyEntityCmd : public Command {
public:
    ModifyEntityCmd(Entity before, Entity after)
        : m_before(std::move(before)), m_after(std::move(after)) {}
    void redo(Document& d) override { assign(d, m_after); }
    void undo(Document& d) override { assign(d, m_before); }
    const char* name() const override { return "Edit"; }
    bool trivial() const { return sameEntity(m_before, m_after); }

private:
    void assign(Document& d, const Entity& v) {
        if (Entity* e = d.find(v.id)) *e = v;
    }
    Entity m_before, m_after;
};

// Modify several entities at once via before/after snapshots keyed by id. Used
// for a gizmo drag that moves a parent and its children as one undoable step.
class ModifyEntitiesCmd : public Command {
public:
    ModifyEntitiesCmd(std::vector<Entity> before, std::vector<Entity> after)
        : m_before(std::move(before)), m_after(std::move(after)) {}
    void redo(Document& d) override { for (const Entity& e : m_after)  assign(d, e); }
    void undo(Document& d) override { for (const Entity& e : m_before) assign(d, e); }
    const char* name() const override { return "Transform"; }
    bool trivial() const {
        if (m_before.size() != m_after.size()) return false;
        for (std::size_t i = 0; i < m_before.size(); ++i)
            if (!sameEntity(m_before[i], m_after[i])) return false;
        return true;
    }

private:
    void assign(Document& d, const Entity& v) {
        if (Entity* e = d.find(v.id)) *e = v;
    }
    std::vector<Entity> m_before, m_after;
};
