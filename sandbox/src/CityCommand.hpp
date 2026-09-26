#pragma once

#include <utility>
#include <vector>

#include "CitySystem.hpp"
#include "Command.hpp"
#include "RoadSet.hpp"

// One town edit as one undo step: the town rules before and after, plus the
// roads the edit took out of the scene and put into it. Roads are never
// destroyed (see RoadSet), so undoing a re-lay is flipping ids alive and dead --
// the same mechanism RoadListCmd uses for a single road.
//
// Whenever roads come or go the command asks for a Build, because the graded
// corridors and the junction aprons are worked out across the whole set -- and
// the roads it takes out are retired, so their corridors are given back first.
class CityCmd : public Command {
public:
    CityCmd(CitySystem& cities, RoadSet& roads, CitySystem::Snapshot before,
            CitySystem::Snapshot after, std::vector<int> killed, std::vector<int> born,
            const char* label)
        : m_cities(&cities), m_roads(&roads), m_before(std::move(before)),
          m_after(std::move(after)), m_killed(std::move(killed)), m_born(std::move(born)),
          m_label(label) {}

    void redo(Document&) override {
        m_cities->restore(m_after);
        // Bring the new ones in before taking the old ones out, so the set is
        // never momentarily without a living road.
        for (int id : m_born)   m_roads->setAlive(id, true);
        for (int id : m_killed) m_roads->setAlive(id, false);
        m_cities->retire(m_killed);   // their corridors go back to the ground
        if (!m_born.empty() || !m_killed.empty()) m_cities->requestBuild();
    }
    void undo(Document&) override {
        m_cities->restore(m_before);
        for (int id : m_killed) m_roads->setAlive(id, true);
        for (int id : m_born)   m_roads->setAlive(id, false);
        m_cities->retire(m_born);
        if (!m_born.empty() || !m_killed.empty()) m_cities->requestBuild();
    }
    const char* name() const override { return m_label; }

    bool trivial() const {
        return m_killed.empty() && m_born.empty() && m_before == m_after;
    }

private:
    CitySystem*          m_cities;
    RoadSet*             m_roads;
    CitySystem::Snapshot m_before, m_after;
    std::vector<int>     m_killed, m_born;
    const char*          m_label;   // static string
};
