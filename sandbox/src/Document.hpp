#pragma once

#include <vector>

#include "SceneTypes.hpp"

// The scene document: the single source of truth for authored content (the
// entity list + material library). Systems (render / physics / script) read it;
// ProjectIO (de)serializes it. Every content edit goes through the CommandStack
// (see Command.hpp) -- never by mutating the document ad hoc -- so the whole
// history is reversible. Ownership lives here, not in main().
//
// Deliberately small and dumb: it stores data and offers id lookup. Edit
// *policy* (what a delete does to children, etc.) lives in the commands, not
// here, so there is exactly one place that decides how the scene changes.
class Document {
public:
    std::vector<Entity>&            entities()        { return m_entities; }
    const std::vector<Entity>&      entities() const  { return m_entities; }
    std::vector<MaterialDef>&       materials()       { return m_materials; }
    const std::vector<MaterialDef>& materials() const { return m_materials; }

    // Index of the entity with `id` in the list, or -1 if absent.
    int indexOf(int id) const {
        for (int i = 0; i < static_cast<int>(m_entities.size()); ++i)
            if (m_entities[i].id == id) return i;
        return -1;
    }
    Entity* find(int id) {
        const int i = indexOf(id);
        return i >= 0 ? &m_entities[i] : nullptr;
    }
    const Entity* find(int id) const {
        const int i = indexOf(id);
        return i >= 0 ? &m_entities[i] : nullptr;
    }

private:
    std::vector<Entity>      m_entities;
    std::vector<MaterialDef> m_materials;
};
