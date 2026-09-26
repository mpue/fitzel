#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/graphics/Mesh.hpp>

#include "CityPlan.hpp"

class RoadSet;
namespace fitzel { struct TerrainEditField; }

// Every town in the scene (see CityPlan.hpp for what a town is), the way
// RiverSystem holds every watercourse: the rules are the scene data, the built
// towns are derived from them and never saved.
//
// Two things happen to a town, and they are deliberately separate calls:
//   * layStreets() puts its street grid into the RoadSet as ordinary roads,
//     tagged with the town's id, replacing whatever that town laid before. The
//     caller then Builds the roads (the terrain corridor is a build-sized job).
//   * update() re-derives the buildings of every town marked dirty -- on a rule
//     edit, and whenever the ground or the roads under a town moved.
class CitySystem {
public:
    std::vector<cityplan::Rule> towns;

    // --- Scene hooks, set once by main ---------------------------------------
    std::function<float(float, float)>                groundAt;
    std::function<bool(float, float)>                 isWater;
    // Every road in the scene as the derive step measures against.
    std::function<std::vector<cityplan::RoadLine>()>  roadLines;

    // Metres from the eye at which a chunk of town stops being drawn.
    float range = 1600.0f;

    // --- The list ------------------------------------------------------------
    // Append `r` with a fresh id; returns its index.
    int  add(cityplan::Rule r);
    // Drop town `i` (its streets are the caller's: see removeStreets).
    void erase(int i);
    int  count() const { return static_cast<int>(towns.size()); }

    // --- Deriving --------------------------------------------------------------
    void markDirty();          // every town: the ground or the roads moved
    void markDirty(int i);
    // Re-derive whatever is dirty and upload it. Once a frame, before the
    // material list goes to the GPU: the palettes are find-or-created in it.
    void update(std::vector<MaterialDef>& materials);

    struct Built {
        cityplan::Town            town;
        std::vector<fitzel::Mesh> meshes;   // parallel to town.district.batches
        bool                      dirty = true;
        double                    ms = 0.0; // last derive, for the panel
    };
    const std::vector<Built>& built() const { return m_built; }

    // Draw every chunk within `range` of `eye`: the mesh (world space, identity
    // model matrix), the material it wears, and whether it casts shadows.
    void forEachDraw(const glm::vec3& eye,
                     const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                              bool castsShadow)>& draw) const;
    // The solid parts, for Play's static colliders.
    void forEachCollider(const std::function<void(const city::Piece&)>& fn) const;
    // Where the forest must leave room: (x, z, radius) per building.
    std::vector<glm::vec3> clearings() const;
    // How far from a street's centreline a town's pavement reaches (0 = no town
    // paves): the grass has to stay out of that, or it grows through the slabs.
    float kerbReach() const;
    // The traffic-signal lamp materials and how bright each is at clock `t`
    // (seconds): kSignalGlow when lit, 0 when dark. The caller sets that on
    // the frame's GPU material -- the library's own stays dark, so a saved
    // project does not change with the moment it was saved in.
    void forEachSignalLamp(double t,
                           const std::function<void(const fitzel::AssetId&, float)>& fn) const;

    // --- Streets -----------------------------------------------------------------
    struct Laid {
        std::vector<int> removed, added;   // RoadSet ids, for the undo step
        int streets = 0, bridges = 0, breaks = 0;
    };
    // Replace town `i`'s streets in `roads` with a fresh set from its rule.
    // Requests a road Build (see buildRequested).
    Laid layStreets(int i, RoadSet& roads);
    // Take every road carrying `townId` out of the scene; returns their ids. If
    // that would leave the set without a living road, an empty one is added
    // first (appended to `added`, when given).
    std::vector<int> removeStreets(int townId, RoadSet& roads,
                                   std::vector<int>* added = nullptr);
    // How many living roads carry `townId`.
    int  streetCount(int townId, const RoadSet& roads) const;

    // Roads that just left the scene (a re-lay, a deleted town, an undo) and
    // whose graded corridor has to be given back before the next Build -- or
    // the old street stays behind as a trench beside the new one. Queued here
    // because CitySystem does not own the terrain; main drains the queue with
    // releaseRetired() right before it Builds.
    void retire(const std::vector<int>& roadIds);
    // Give back the ground of every queued road that is still out of the scene
    // (one brought back by an undo in the meantime is skipped -- the Build
    // re-cuts it). `keep` is what another system owns in a cell (the rivers'
    // bed). False when no cell changed; [outMin,outMax] covers every change.
    bool releaseRetired(RoadSet& roads, fitzel::TerrainEditField& edit,
                        const std::function<float(std::int64_t)>& keep,
                        glm::vec2& outMin, glm::vec2& outMax);

    // The roads want a Build (streets laid, or an undo put streets back or took
    // them away). main polls this once a frame and runs its Build.
    void requestBuild() { m_buildRequest = true; }
    bool consumeBuildRequest() { const bool b = m_buildRequest; m_buildRequest = false; return b; }

    // --- Undo --------------------------------------------------------------------
    struct Snapshot {
        std::vector<cityplan::Rule> towns;
        int nextId = 1;
        bool operator==(const Snapshot& o) const { return nextId == o.nextId && towns == o.towns; }
    };
    Snapshot snapshot() const { return {towns, m_nextId}; }
    void     restore(const Snapshot& s);

    // --- Scene persistence -------------------------------------------------------
    void save(nlohmann::json& j) const;
    void load(const nlohmann::json& j);
    void clear();

private:
    void sync();   // m_built parallel to towns

    std::vector<Built> m_built;
    int                m_nextId = 1;
    bool               m_buildRequest = false;
    std::vector<int>   m_retired;   // road ids whose corridor is to be given back
    civic::Palette     m_civic;     // the last civic palette a derive used (signal lamps)
    bool               m_hasCivic = false;
};
