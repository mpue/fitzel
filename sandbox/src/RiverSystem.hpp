#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/world/Terrain.hpp>   // TerrainEditField

#include "RiverGen.hpp"

// The scene's running water: any number of independent watercourses, each drawn
// as a path and built as a brook, a river or a canal (see RiverGen.hpp).
//
// This is the scene-level half. It owns the control points, samples them into a
// centreline, has RiverGen solve the descending profile, cuts the bed into the
// terrain deformation field, uploads the water surface and persists the paths.
// The geometry is never saved and never becomes an entity -- only points + rule
// are, and everything else is re-derived on load, exactly like the road's graded
// corridor, the roadside city and the fences.
//
// --- Why the carve cannot simply read the ground ----------------------------
// A river is the only thing here that both reads the terrain and writes it, and
// that is a loop: solve a profile against the ground, cut the ground down to it,
// solve again, and the bed sinks a little further every time. So this keeps its
// OWN copy of every delta it has written (`mine`) and solves against the ground
// as it would be if no watercourse had ever touched it (see natural()).
//
// That reconstruction is done by DIFFERENCING THE TWO EDIT FIELDS and adding the
// result to the bare procedural terrain -- never by subtracting the cut from a
// ground that already contains it. The two are the same on paper and are not the
// same in floats: `base + cut` rounds the low bits of `base` away, and taking
// `cut` off again cannot bring them back. What comes out is an answer that moves
// by an ulp whenever the cut changes, which sounds harmless and is not: the
// profile is full of thresholds (is this sample a fall, how far does the pool
// above it reach, is the ground here too steep to meander) and a last-bit wobble
// flips them. The visible result is a bed that jumps by METRES at a waterfall
// lip while every number in sight looks like rounding error. Differencing the
// fields first cancels exactly, because they hold the same numbers.
//
// The same copy is what makes the cut REVERSIBLE without a ghost: moving a path
// republishes the difference between the old `mine` and the new one, so the
// abandoned trench gives its ground back instead of staying as a scar, and a
// sculpt outside the new footprint survives untouched.
//
// --- Why there is no Build button --------------------------------------------
// The road has one because grading a corridor is expensive and a road is edited
// by dragging. So is a river -- but a channel whose water only moves when you
// press a button shows you a river hanging in the air for the whole of the drag,
// which is the one thing that makes the tool unusable. Instead the cut runs when
// an interaction ENDS (see carve), on the same bracket that pushes the undo step:
// one cut per gesture, not one per frame.
class RiverSystem {
public:
    // One authored watercourse.
    struct Path {
        std::string       name   = "Brook";
        rivergen::Kind    kind   = rivergen::Kind::Brook;
        // Which ready-made channel the style was last seeded from. Kept so the
        // panel can show it and re-apply it; the style may wander from it
        // afterwards and nothing checks that it still matches.
        rivergen::Preset  preset = rivergen::Preset::Brook;
        // Control points in world XZ, with a per-point offset in `bias` -- same
        // length as `points`, always.
        //
        // `bias` is metres the WATER SURFACE is pushed above the ground it would
        // otherwise hug: positive dams a pool, negative digs the channel in.
        // Deliberately not a free height: it enters the profile solve as a
        // preference, and the solve's descent guarantee still wins, so no amount
        // of dragging a handle can make the water flow uphill.
        std::vector<glm::vec2> points;
        std::vector<float>     bias;
        bool                   enabled = true;
        rivergen::Style        style;

        bool operator==(const Path& o) const {
            return name == o.name && kind == o.kind && preset == o.preset &&
                   points == o.points && bias == o.bias &&
                   enabled == o.enabled && style == o.style;
        }
        bool operator!=(const Path& o) const { return !(*this == o); }
    };

    // The BARE procedural terrain at world XZ -- no manual edits of any kind.
    // Injected by the owner; without it everything sits at 0.
    std::function<float(float, float)> baseAt;

    // ...and the world's manual height edits, this system's own cut included.
    // Read, never written, and only by natural(). Borrowed: the owner holds it
    // for the session and hands the same field to carve().
    //
    // Two things rather than one "how high is the ground here" callback, and
    // that is the whole point -- see the header comment above.
    const fitzel::TerrainEditField* edits = nullptr;

    // The project's material library, for the two shared materials the stones
    // and the reeds wear. Injected like groundAt rather than passed to update():
    // the cut runs from an undo bracket that main sets up long before the scene
    // document exists, and threading the library down to that point would mean
    // moving half of main's construction order to suit this one call. Null (or
    // never set) simply means no dressing is built.
    std::vector<MaterialDef>* materials = nullptr;

    // --- Authoring -----------------------------------------------------------
    std::vector<Path> paths;

    int   addPath(rivergen::Preset p, const std::string& name = "");
    void  applyPreset(int path, rivergen::Preset p);
    void  removePath(int i);
    void  insertPoint(int path, int at, glm::vec2 p, float bias = 0.0f);
    void  erasePoint(int path, int at);
    float biasOf(int path, int i) const;
    void  setBias(int path, int i, float bias);
    // Mark one path (or all of them) for re-solving on the next update().
    void  touch(int path = -1);
    // True when a path has changed since the last carve, so the owner knows the
    // terrain is out of date without having to track the edits itself.
    bool  carveDirty() const { return m_carveDirty; }

    struct Snapshot { std::vector<Path> paths; };
    Snapshot snapshot() const { return {paths}; }
    void     restore(const Snapshot& s) { paths = s.paths; touch(); }

    // --- Derived geometry ----------------------------------------------------
    // Re-solve whatever is dirty and rebuild its surface. Cheap when nothing is.
    // Does NOT touch the terrain -- see carve.
    void update();

    // Cut every watercourse's bed into `edit` and hand back the world-space
    // rectangle whose terrain changed (already padded for the chunk rebuild).
    // Returns false when nothing moved.
    //
    // ALL paths are re-cut, not just the dirty one: two channels that meet share
    // cells, and cutting one of them against a ground the other had already
    // lowered is how a confluence drifts a little deeper every time it is
    // touched. Re-cutting from the pristine ground every time costs a scan and
    // buys a result that does not depend on the order the author edited in.
    // `paint` gets the bank layer laid over the bed and margins, on the same
    // difference discipline as the height field and for the same reason -- a
    // course that moves has to take its gravel with it.
    bool carve(fitzel::TerrainEditField& edit, fitzel::TerrainPaintField& paint,
               glm::vec2& outMin, glm::vec2& outMax);

    // Give the terrain back entirely (used when the scene is cleared). Same
    // publish contract as carve.
    bool release(fitzel::TerrainEditField& edit, fitzel::TerrainPaintField& paint,
                 glm::vec2& outMin, glm::vec2& outMax);

    // This system's own delta at one cell of the host's grid, 0 where no
    // watercourse has touched the ground.
    //
    // It exists so the scene writer can leave the cut OUT of the saved terrain
    // edits. A bed is derived geometry like every other thing in this editor --
    // the file holds the path and the rule, and the channel is re-cut on load
    // (see forget). Writing it out instead would save it twice, and the second
    // copy would be the one a re-solve then dug into.
    float mineAt(std::int64_t key) const;
    // The same for the bank paint. Gravel laid by a channel is derived from the
    // channel; the file holds neither.
    glm::vec4 minePaintAt(std::int64_t key) const;

    // Drop the record of what was cut WITHOUT giving the ground back. For the
    // scene load, where the whole field is replaced in one go and the cut is
    // about to be re-derived from the paths.
    void forget();

    // One built watercourse, parallel to `paths`.
    struct Run {
        rivergen::Course                 course;
        fitzel::Mesh                     mesh;   // the water surface, world space
        glm::vec3                        lo{0.0f}, hi{0.0f};
        std::vector<rivergen::SprayPoint> spray;
        int                              verts = 0;
        // Stones and reeds: merged per material, world space, drawn through the
        // renderer like the roadside city rather than through the water shader.
        std::vector<rivergen::Batch>     dress;        // AABBs + materials
        std::vector<fitzel::Mesh>        dressMeshes;  // parallel to `dress`
    };
    const std::vector<Run>& runs() const { return m_runs; }

    // The solved centreline of path `i` at the water surface -- what the editor
    // draws. Empty for < 2 points or before the first update().
    const std::vector<glm::vec3>& line(int i) const;
    // Sample index of each control point in line(i), for marking the stretch a
    // point owns. Already in downstream order, like the course.
    const std::vector<int>& pointSamples(int i) const;
    // The course of path `i`, for the panel's report (drop, cut, falls, uphill).
    const rivergen::Course& course(int i) const;

    // Where the handle for control point `point` of `path` belongs: on the
    // WATER, not on the bare ground. A channel whose bed was cut two metres down
    // has its surface two metres down with it, and a handle left up on the
    // original hillside is a handle for a river that is not there. False before
    // the first update(), or for a course with fewer than two points.
    bool handleHeight(int path, int point, float& outY) const;

    // --- Play ----------------------------------------------------------------
    // Is (x,z) in the water, and if so how high does it stand, how deep is it
    // and which way is it pushing? `outFlow` is metres per second in the XZ
    // plane -- the current at the surface, falling off toward the banks the way
    // a real channel's does. Cheap: projects onto the cached station grid.
    //
    // Anything wanting to float, be swept, or make a noise about it asks this and
    // nothing else, so the buoyancy, the sound and the spray can never disagree
    // about where the water is.
    bool sample(const glm::vec2& xz, float& outSurface, float* outDepth = nullptr,
                glm::vec2* outFlow = nullptr, float* outWhite = nullptr) const;

    // --- Keeping out ---------------------------------------------------------
    // Where nothing should grow: the wetted channel, as a list of discs sampled
    // along every course. `margin` widens each one, for callers that want a bit
    // of dry ground either side as well.
    //
    // Discs rather than a polyline because a channel has a WIDTH that changes
    // along it -- and because the vegetation's tile workers take a COPY of
    // whatever they are given and then read it from another thread, so it has to
    // be small, flat, and free of any pointer back into this system.
    std::vector<glm::vec3> wetDiscs(float margin = 0.0f) const;  // xy centre, z radius

    // --- Being heard ---------------------------------------------------------
    // The few places nearest `at` where this water can be heard, loudest first.
    // Emitters rather than sounds: how many voices there are to spare is the
    // caller's business (see WorldAudio::AmbiencePoint).
    //
    // A calm stretch contributes its nearest point and a fall contributes its
    // own, because those are heard at completely different distances -- a
    // waterfall two hundred metres off is louder than the brook at your feet,
    // and a list that only ever offered the nearest water could not say so.
    struct Audible {
        glm::vec3 pos{0.0f};
        float     gain  = 0.0f;   // 0..1 at the source
        float     pitch = 1.0f;   // a brook chatters, a river rumbles
        float     range = 70.0f;  // metres it carries
    };
    std::vector<Audible> audible(const glm::vec3& at, int want,
                                 float maxDist = 260.0f) const;

    // --- Scene persistence ---------------------------------------------------
    void save(nlohmann::json& j) const;
    void load(const nlohmann::json& j);
    void clear();

    // Totals for the panel.
    int   totalVerts() const;
    float totalLength() const;

private:
    struct Built { bool dirty = true; };

    std::vector<Run>   m_runs;
    std::vector<Built> m_built;
    bool               m_carveDirty = true;

    // Every delta this system has written, on the host field's cell grid. The
    // ground a profile is solved against is `groundAt() - mine.sample()`; see the
    // header comment for why that subtraction is the whole design.
    fitzel::TerrainEditField  m_mine;
    // ...and the same record for the bank paint, so gravel laid down by a course
    // that has since moved is taken back rather than left behind.
    fitzel::TerrainPaintField m_paint;
    // The world rectangle `m_mine` covers, so a cut that moves can ask for the
    // ground it LEFT to be rebuilt as well as the ground it took.
    glm::vec2 m_lo{0.0f}, m_hi{0.0f};
    bool      m_hasFootprint = false;

    float natural(float x, float z) const;
    void  rebuild(int i);
    // Stamp one path's section into `dst` (a fresh `mine`), extending [lo,hi].
    void  cutInto(const Run& r, const Path& p, fitzel::TerrainEditField& dst,
                  fitzel::TerrainPaintField& dstPaint,
                  glm::vec2& lo, glm::vec2& hi) const;
    // Publish the difference between the old `mine` and `next` into the host.
    bool  publish(fitzel::TerrainEditField& edit, fitzel::TerrainPaintField& paint,
                  const fitzel::TerrainEditField& next,
                  const fitzel::TerrainPaintField& nextPaint,
                  glm::vec2 lo, glm::vec2 hi, glm::vec2& outMin, glm::vec2& outMax);
};
