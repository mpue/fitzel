#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "CityPlan.hpp"

// The towns as somewhere to go: what a character walking through one needs to
// know -- the places by name, and a way there on foot.
//
// Derived, never saved, from what the towns already derive (CityPlan's Town):
//   * The pavements. Every block has a walk round it along the middle of its
//     pavement (Town::walks). Those loops are joined into one network at the
//     block corners, straight across the street to the corner opposite -- a
//     crossing goes where the pavement's own side line would carry on, never
//     diagonally over a junction and never in the middle of a block.
//   * The places. Each civic block (school, church, town hall, ...), each park,
//     bus stop and building, with the street it stands on. Buildings get house
//     numbers along their street: odd on one side, even on the other, counting
//     from where the street starts.
// A place's `pos` is on the pavement in front of it -- where a figure walking
// there stops -- and `at` is the thing itself, for facing it.
namespace townnav {

struct Place {
    std::string name;      // "Rathaus", "Bushaltestelle Lindenstraße", "Lindenstraße 12"
    std::string kind;      // civic: "townhall", "school", ...; "park", "stop",
                           // "home", "flat", "office", "works"
    std::string street;    // the street it stands on ("" when none is named)
    int         number = 0;   // house number, 0 = none
    glm::vec3   pos{0.0f};    // on the pavement in front of it
    glm::vec2   at{0.0f};     // the place itself (x, z)
    int         town = 0;     // index into CitySystem::towns
};

// The walking network of every town together.
class Nav {
public:
    // `walks` as the towns derive them: closed loops on the pavement; `roads`
    // every road, to tell a crossing from a short cut over a junction.
    void build(const std::vector<std::vector<glm::vec3>>& walks,
               const std::vector<cityplan::RoadLine>& roads);
    bool empty() const { return m_pts.empty(); }
    int  nodeCount() const { return static_cast<int>(m_pts.size()); }
    int  crossingCount() const { return m_crossings; }

    // The nearest point of the network to (x, z); -1 when there is none.
    int nearest(glm::vec2 p) const;
    const glm::vec3& point(int i) const { return m_pts[static_cast<std::size_t>(i)]; }

    // A way on foot from the pavement nearest `from` to the pavement nearest
    // `to`, as points to walk through in order (first and last are those two
    // pavement points). Empty when they are not connected.
    std::vector<glm::vec3> path(glm::vec2 from, glm::vec2 to) const;

private:
    struct Edge { int to; float cost; };
    std::vector<glm::vec3>         m_pts;
    std::vector<std::vector<Edge>> m_edges;
    int                            m_crossings = 0;
};

// The street (by name) nearest (x, z) among `roads`, and how far it is; ""
// when no named road is within `maxDist`.
std::string streetAt(const std::vector<cityplan::RoadLine>& roads, glm::vec2 p,
                     float maxDist, float* dist = nullptr);

// Every place in town `index` (rule `rule`, derived as `town`), on `nav`.
std::vector<Place> places(int index, const cityplan::Rule& rule, const cityplan::Town& town,
                          const std::vector<cityplan::RoadLine>& roads, const Nav& nav);

} // namespace townnav
