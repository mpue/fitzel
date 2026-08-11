#pragma once

#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp> // fitzel::MeshData (CPU-side, no GPU)

// Vertical loops on the road: a stretch that curls up, over and back down.
//
// A loop is DERIVED from a pair of control points, exactly the way a bridge is
// (see RoadBridge.hpp) -- the user names two points and a radius, and the
// geometry is regenerated from the centreline on every build. Only the rule is
// saved.
//
// Why it is a separate feature rather than something the ribbon can do: the road
// is one height per XZ position (`prof` alongside a flat centreline), and a loop
// needs two -- the deck you drive up and the deck you come back over. That is not
// a limitation of the loft, it is what the data model IS, and the whole of the
// terrain grading, the vegetation mask, the roadside instancing and the lap
// progress read that flat centreline. So the loop leaves the model alone and adds
// geometry beside it, the same bargain the bridge decks already make.
//
// The shape is a circle that ADVANCES while it turns: the forward offset is
// R*sin(theta) plus a term linear in theta, so after a full turn the track has
// moved `L` metres down the road instead of landing back on its own entry. L
// comes from where the user put the two control points, so the loop is stretched
// or tightened by dragging them rather than by a second slider. It leaves the
// ground tangentially (dh/dtheta is 0 at theta = 0), which is what lets a craft
// riding "the highest surface below me" flow onto it without a lip to catch on.
//
// Pure geometry: it walks a centreline and returns frames and triangles. What
// happens to a craft on it lives in RaceSim (surface-relative gravity, and
// falling off when there is not enough speed to hold the top).
namespace roadloop {

// One loop the user asked for, named by the two control points at its ends
// (indices into roadPts, either order). Points move and vanish under the editor,
// so these are validated on every build, exactly like BridgeSpec.
struct Spec {
    int   a = 0, b = 0;
    float radius = 12.0f;  // metres; the height of the loop is twice this

    bool operator==(const Spec& o) const {
        return a == o.a && b == o.b && radius == o.radius;
    }
    bool operator!=(const Spec& o) const { return !(*this == o); }
};

// The track's frame at one station around a loop. `normal` is the side the craft
// drives on -- it points at the loop's axis, so at the top it points at the
// ground, which is the whole reason a loop needs its own gravity.
struct Frame {
    glm::vec3 pos{0.0f};      // centreline of the carriageway
    glm::vec3 tangent{0.0f};  // direction of travel
    glm::vec3 normal{0.0f};   // surface normal (toward the loop's axis)
    glm::vec3 side{0.0f};     // across the carriageway (tangent x normal)
    // The nose's elevation within the loop's vertical plane, in radians, running
    // 0 to about 2*pi WITHOUT wrapping. The craft's pitch is driven straight off
    // this, so it has to be the angle of the TANGENT (where the nose actually
    // points), not of the circle -- the advance term tilts one against the other.
    // Unwrapped, because an angle that jumped at the top would flip the model
    // through a full turn for one frame as the render interpolates across it.
    float     pitch = 0.0f;
};

// A built loop: its frames around the full turn, with arc length so a craft can
// be integrated along it in metres rather than in angle (which is not uniform --
// the advance term stretches the bottom of the turn).
struct Loop {
    std::vector<Frame> frames;
    std::vector<float> s;         // arc length at each frame, s[0] = 0
    float     length = 0.0f;      // total arc length
    float     radius = 12.0f;
    glm::vec3 lo{0.0f}, hi{0.0f}; // world AABB, for a cheap "am I near it" test
    int       samples() const { return static_cast<int>(frames.size()); }
};

// Build every loop the specs ask for. `center`/`prof` are the sampled centreline
// and its surface height; `ptSample` maps control point -> sample index (as
// sampleCenterlineXZ hands it back). Specs naming points that no longer exist are
// skipped rather than clamped -- silently looping somewhere else is worse than
// not looping at all.
std::vector<Loop> plan(const std::vector<glm::vec2>& center,
                       const std::vector<float>& prof,
                       const std::vector<int>& ptSample,
                       const std::vector<Spec>& specs);

// Append the carriageway ribbon of every loop to `md`. Winding matches the rest
// of the engine (CCW = front face) and the surface is two-sided in effect,
// because a loop is looked at from underneath for half of its length.
void build(const std::vector<Loop>& loops, float roadWidth, float texTile,
           fitzel::MeshData& md);

// The frame at arc length `arc` (wrapped into [0, length)).
Frame at(const Loop& lp, float arc);

// Find `p` on the loop: fills `outArc` with the arc length of the nearest frame
// and `outLateral` with the signed offset across the carriageway, and returns the
// distance from the surface along the normal (negative = below it). False when
// the point is nowhere near this loop.
//
// Nearest in 3D, deliberately: a loop passes over its own entry, so anything
// measured in plan view would answer with the wrong half of the turn.
bool locate(const Loop& lp, const glm::vec3& p, float halfWidth, float& outArc,
            float& outLateral, float& outHeight);

} // namespace roadloop
