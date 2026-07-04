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
    }
    void undo(Document& doc) {
        if (m_undo.empty()) return;
        m_undo.back()->undo(doc);
        m_redo.push_back(std::move(m_undo.back()));
        m_undo.pop_back();
    }
    void redo(Document& doc) {
        if (m_redo.empty()) return;
        m_redo.back()->redo(doc);
        m_undo.push_back(std::move(m_redo.back()));
        m_redo.pop_back();
    }
    void clear() { m_undo.clear(); m_redo.clear(); }

    bool        canUndo()  const { return !m_undo.empty(); }
    bool        canRedo()  const { return !m_redo.empty(); }
    const char* undoName() const { return m_undo.empty() ? "" : m_undo.back()->name(); }
    const char* redoName() const { return m_redo.empty() ? "" : m_redo.back()->name(); }

private:
    std::vector<std::unique_ptr<Command>> m_undo, m_redo;
};

// Value equality across every editable/serialized field (so a no-op edit can be
// dropped instead of polluting the undo history).
inline bool sameEntity(const Entity& a, const Entity& b) {
    return a.id == b.id && a.type == b.type &&
           a.center == b.center && a.half == b.half && a.rotation == b.rotation &&
           a.color == b.color && a.scale == b.scale &&
           a.intensity == b.intensity && a.range == b.range &&
           a.castShadows == b.castShadows && a.shadowBias == b.shadowBias &&
           a.physics == b.physics && a.mass == b.mass &&
           a.parent == b.parent && a.name == b.name && a.script == b.script &&
           a.material == b.material && a.modelId == b.modelId;
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
