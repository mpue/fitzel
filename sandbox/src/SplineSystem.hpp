#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/graphics/Mesh.hpp>

#include "SceneTypes.hpp"   // MaterialDef
#include "SplineGen.hpp"

// The scene's spline structures: any number of independent paths, each carrying
// a fence, a wall, a railway track or a bridge (see SplineGen.hpp / BridgeGen.hpp) --
// or nothing at all, a bare path that objects are placed along (SplinePlace.hpp).
//
// This is the scene-level half: it owns the control points, samples them into a
// draped centreline, calls the generator, uploads what comes back and persists
// the paths. The geometry itself is never saved and never becomes an entity --
// only points + rule are, and everything else is re-derived on load, exactly like
// the road's graded corridor and the roadside city.
//
// Unlike the road there is no Build step. A fence does not touch the terrain, so
// there is nothing to commit and nothing to undo about the ground: a path is
// rebuilt the frame after it changes (see update). That is deliberate, and it is
// the difference between laying a fence by clicking four times and laying one by
// clicking four times and then hunting for a button.
class SplineSystem {
public:
    // One authored path.
    struct Path {
        std::string             name = "Fence";
        splinegen::Kind         kind = splinegen::Kind::Fence;
        // Which ready-made structure the style was last seeded from. Kept only so
        // the panel can show it and re-apply it; the style is free to wander from
        // it afterwards and nothing here checks that it still matches.
        splinegen::Preset       preset = splinegen::Preset::PostRail;
        // Control points in world XZ, with a per-point height offset above the
        // ground in `lifts` -- same length as `points`, always (insert/erase
        // below are the only places either is resized). 0 means "follow the
        // terrain", which is what an older scene loads as.
        std::vector<glm::vec2>  points;
        std::vector<float>      lifts;
        bool                    closed  = false;
        bool                    enabled = true;
        splinegen::Style        style;

        bool operator==(const Path& o) const {
            return name == o.name && kind == o.kind && preset == o.preset &&
                   points == o.points && lifts == o.lifts && closed == o.closed &&
                   enabled == o.enabled && style == o.style;
        }
        bool operator!=(const Path& o) const { return !(*this == o); }
    };

    // Terrain height at world XZ. Injected by the owner (main hands it the
    // streamer) so this stays free of the terrain, the same way roadside does.
    // A path drapes on whatever this returns; without it everything sits at y=0.
    std::function<float(float, float)> groundAt;
    // The surface of the road at world XZ, if there is one there: a track laid
    // into the streets (Style::embed) takes ITS height -- the asphalt's top, a
    // bridge's deck -- instead of the ground's. Unset: nothing is a road.
    std::function<bool(float x, float z, float& y)> roadSurfaceAt;
    // The middle of the road under `p` (false = not on a road). A point of a
    // track laid into the streets snaps to it as it is placed or dragged.
    std::function<bool(glm::vec2 p, glm::vec2& centre)> snapToRoad;

    // --- Authoring -----------------------------------------------------------
    std::vector<Path> paths;

    // Add a path built to `p` -- its kind, its style and (unless one is given) its
    // name all come from the preset. Returns its index.
    int  addPath(splinegen::Preset p, const std::string& name = "");
    // Re-seed an existing path's style (and kind) from a preset, keeping its
    // points. What the panel's preset picker calls.
    void applyPreset(int path, splinegen::Preset p);
    void removePath(int i);
    void insertPoint(int path, int at, glm::vec2 p, float lift = 0.0f);
    void erasePoint(int path, int at);
    float liftOf(int path, int i) const;
    // Where control point `i` is in the world: on the ground plus its lift --
    // or, for a bridge, on the deck. What the editor puts the handle on.
    glm::vec3 pointWorld(int path, int i) const;
    void  setLift(int path, int i, float lift);
    // Where a point placed at `p` on path `path` goes: onto the road's middle for
    // a track laid into the streets, else exactly `p`.
    glm::vec2 snapped(int path, glm::vec2 p) const;
    // Mark one path (or all of them) for regeneration on the next update().
    void touch(int path = -1);

    // Everything an edit can change: the state an undo step has to put back.
    // The paths are a few hundred points all told, so snapshotting them whole is
    // the same trade RoadSystem::Shape makes.
    struct Snapshot { std::vector<Path> paths; };
    Snapshot snapshot() const { return {paths}; }
    void     restore(const Snapshot& s) { paths = s.paths; touch(); }

    // --- Derived geometry ----------------------------------------------------
    // Regenerate whatever is dirty. Cheap when nothing is: the common frame does
    // no work at all. `materials` is the project's library -- the generator
    // find-or-creates the shared palette materials in it, so a colour edit lands
    // without a separate apply step.
    void update(std::vector<MaterialDef>& materials);

    // One built path, parallel to `paths`.
    struct Run {
        splinegen::Result        geo;     // batch AABBs + materials (data released)
        std::vector<fitzel::Mesh> meshes; // parallel to geo.batches
    };
    const std::vector<Run>& runs() const { return m_runs; }

    // The sampled, draped centreline of path `i` -- what the editor draws as the
    // path's line, and what the generator swept along. Empty for < 2 points or
    // before the first update().
    const std::vector<glm::vec3>& line(int i) const;

    // Sample index of each control point in line(i) (one per point, plus a
    // closing entry), so the editor can mark the stretch a point owns.
    const std::vector<int>& pointSamples(int i) const;

    // Which samples of line(i) lie on a road (a track laid into the streets
    // only; empty otherwise). The trams need to know how high the rail is.
    const std::vector<char>& onRoad(int i) const;
    // Bumped whenever any path is rebuilt -- for what follows the paths (the
    // trams running on the tram tracks).
    int revision() const { return m_revision; }

    // --- Scene persistence ---------------------------------------------------
    // Runtime, not editor: the player loads scenes too, and re-derives every run
    // from the same code path the editor does.
    void save(nlohmann::json& j) const;
    void load(const nlohmann::json& j);
    void clear();

private:
    struct Built {
        std::vector<glm::vec3> line;
        std::vector<int>       ptSample;
        std::vector<char>      onRoad;
        bool                   dirty = true;
    };
    std::vector<Run>   m_runs;
    std::vector<Built> m_built;
    int                m_revision = 0;

    void rebuild(int i, std::vector<MaterialDef>& materials);
};
