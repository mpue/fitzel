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
// a fence, a wall or a railway track (see SplineGen.hpp for what those are).
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
    void  setLift(int path, int i, float lift);
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
        bool                   dirty = true;
    };
    std::vector<Run>   m_runs;
    std::vector<Built> m_built;

    void rebuild(int i, std::vector<MaterialDef>& materials);
};
