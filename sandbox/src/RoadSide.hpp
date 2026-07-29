#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// Side objects for RoadSystem: a spline instancer that repeats a model asset
// along the road at a lateral offset -- guard rails, curbs, delineator posts.
//
// A "line" is a rule, not a pile of objects: "this model, this far beyond the
// edge, every N metres, on this side". The instances are DERIVED from the road,
// the same way the graded corridor is -- they are regenerated from the line +
// the current centreline whenever the road is (re)built, never authored or saved
// one by one. Editing the road drags them along; editing the line reshapes the
// whole run at once. That is deliberate: it keeps a rail glued to its road, keeps
// hundreds of segments out of the scene hierarchy, and means the user tunes a
// handful of numbers instead of nudging each post -- which is the only workable
// way to place a rail without precise per-object dragging.
//
// This module is pure geometry: it walks a centreline and returns transforms. It
// knows nothing about models, materials or the GPU -- the caller resolves the
// model reference and renders/collides each instance (see main). That mirrors how
// RoadBridge stays render-free.
namespace roadside {

// What a line is, which only presets sensible defaults -- the fields below are
// what actually drive placement, so a "post" with tight spacing is just a rail.
enum class Kind { GuardRail, Curb, Post };

inline const char* kindName(Kind k) {
    switch (k) {
        case Kind::GuardRail: return "Guard rail";
        case Kind::Curb:      return "Curb";
        case Kind::Post:      return "Post";
    }
    return "Side object";
}

// Which side(s) of the road a line populates.
enum Side { Left = 0, Right = 1, Both = 2, Alternate = 3 };

// One placement rule. `model` is an asset reference (file name, relative path or
// GUID -- resolved by the caller). Everything else is metric so it reads the same
// whatever the sample density.
struct Line {
    bool        enabled = true;
    Kind        kind    = Kind::GuardRail;
    std::string model;              // Model asset ("" = the line does nothing)
    int         side    = Side::Right;
    float       offset  = 0.6f;     // metres beyond the road half-width
    float       spacing = 4.0f;     // metres between instances (== segment length
                                    // for a continuous rail/curb)
    float       lift    = 0.0f;     // metres raised above the ground it stands on
    float       sink    = 0.05f;    // metres pushed into the ground (hides the seam)
    float       yaw     = 0.0f;     // extra yaw in degrees (0 = face along the road)
    float       scale   = 1.0f;     // uniform model scale
    bool        faceRoad = true;    // flip the far side 180 so a one-sided profile
                                    // (rail beam, curb face) points at the road
    // Knockable: in Play the instance is a DYNAMIC body (mass kg) instead of a
    // static one, so a vehicle bowls it over and it flies off -- delineator posts,
    // bollards, cones. Off for rails/curbs, which are meant to stop a car dead.
    // Reset when Play stops (the instances are re-derived static). Heavier = harder
    // to shift; too light jitters.
    bool        knockable = false;
    float       mass      = 8.0f;

    // Value equality, so the road's undo snapshot can tell a real edit from a
    // no-op (see RoadShapeCmd::trivial).
    bool operator==(const Line& o) const {
        return enabled == o.enabled && kind == o.kind && model == o.model &&
               side == o.side && offset == o.offset && spacing == o.spacing &&
               lift == o.lift && sink == o.sink && yaw == o.yaw &&
               scale == o.scale && faceRoad == o.faceRoad &&
               knockable == o.knockable && mass == o.mass;
    }
    bool operator!=(const Line& o) const { return !(*this == o); }
};

// The defaults a Kind seeds when a line is created: rails/curbs run continuously
// (spacing ~ a segment length, hard against the edge), posts stand apart.
Line preset(Kind k);

// A single placed instance: a world position, a yaw (radians, about +Y) and a
// uniform scale. `pos` already has lift/sink and the terrain drape applied.
struct Instance {
    glm::vec3 pos;
    float     yaw;   // radians
    float     scale;
};

// Walk `center` (world XZ, the road centreline) and emit the instances of one
// line. `halfWidth` is the road's half-width; `groundAt(x,z)` returns the height
// the object stands on (the caller passes the terrain sampler). Returns empty for
// a disabled line, an empty model, or a centreline shorter than two points.
std::vector<Instance> generate(const Line& line, const std::vector<glm::vec2>& center,
                               float halfWidth,
                               const std::function<float(float, float)>& groundAt);

} // namespace roadside
