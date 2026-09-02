#include "Selection.hpp"

#include <algorithm>


int Selection::indexOf(int id) const {
    for (int i = 0; i < static_cast<int>(m_entities.size()); ++i)
        if (m_entities[i].id == id) return i;
    return -1;
}

bool Selection::valid() const {
    return m_active >= 0 && m_active < static_cast<int>(m_entities.size());
}

int Selection::activeId() const {
    return valid() ? m_entities[m_active].id : -1;
}

std::vector<int> Selection::ids() const {
    if (!m_multi.empty()) return m_multi;
    const int id = activeId();
    return id >= 0 ? std::vector<int>{id} : std::vector<int>{};
}

bool Selection::contains(int id) const {
    if (activeId() == id && id >= 0) return true;
    return std::find(m_multi.begin(), m_multi.end(), id) != m_multi.end();
}

std::size_t Selection::count() const {
    if (!m_multi.empty()) return m_multi.size();
    return valid() ? 1u : 0u;
}

void Selection::select(int id) {
    m_active = indexOf(id);
    m_multi.clear();
}

void Selection::selectIndex(int i) {
    m_active = i;
    m_multi.clear();
}

void Selection::clear() {
    m_active = -1;
    m_multi.clear();
}

void Selection::collapseIfSingle() {
    if (m_multi.size() == 1) m_multi.clear();
}

void Selection::toggle(int id) {
    if (id < 0) return;
    // Seed the set from the current active object: Ctrl+clicking a second
    // object means "both", not "just this one".
    if (m_multi.empty() && valid()) m_multi = { activeId() };
    const auto it = std::find(m_multi.begin(), m_multi.end(), id);
    if (it != m_multi.end()) {
        m_multi.erase(it);
        m_active = m_multi.empty() ? -1 : indexOf(m_multi.back());
    } else {
        m_multi.push_back(id);
        m_active = indexOf(id);
    }
    collapseIfSingle();
}

void Selection::addMany(const std::vector<int>& ids) {
    if (ids.empty()) return;
    if (m_multi.empty() && valid()) m_multi = { activeId() };
    for (int id : ids) {
        // Ignore ids that name nothing. A caller handing over a stale one used
        // to take the WHOLE selection down with it: the id went into the set,
        // became the active object, resolved to index -1, and normalize() then
        // cleared everything because the anchor was gone. Adding objects must
        // not be able to deselect the ones already there.
        if (indexOf(id) < 0) continue;
        if (std::find(m_multi.begin(), m_multi.end(), id) == m_multi.end())
            m_multi.push_back(id);
    }
    if (!m_multi.empty()) m_active = indexOf(m_multi.back());
    collapseIfSingle();
}

void Selection::normalize() {
    m_multi.erase(std::remove_if(m_multi.begin(), m_multi.end(),
                                 [&](int id) { return indexOf(id) < 0; }),
                  m_multi.end());
    const int activeId_ = activeId();
    // The active object is gone (deleted, or the scene was replaced). There is
    // no survivor to promote to -- the set was only ever a selection ANCHORED
    // on it -- so the whole thing goes, index included: a stale index into a
    // list that has since changed is the one value worse than -1.
    if (activeId_ < 0) { clear(); return; }
    // Something moved the active object out of the set (undo, a delete, a path
    // that still assigns the index directly): the set no longer describes a
    // selection this object is part of, so it goes.
    if (!m_multi.empty() &&
        std::find(m_multi.begin(), m_multi.end(), activeId_) == m_multi.end())
        m_multi.clear();
    collapseIfSingle();
}
