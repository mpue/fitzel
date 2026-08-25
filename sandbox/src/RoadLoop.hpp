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
// moved a little way down the road instead of landing back on its own entry. It
// leaves the ground tangentially (dh/dtheta is 0 at theta = 0), which is what
// lets a craft riding "the highest surface below me" flow onto it without a lip
// to catch on.
//
// A cycloid crosses ITSELF, and no choice of radius avoids it. Its rising and
// falling branches reach the same HEIGHT at theta and 2*pi - theta (cos is even),
// so wherever they also meet in x they meet at the same POINT -- a real
// intersection in 3D, not an over-and-under. Solving `a*theta + R*sin(theta) =
// pi*a` puts that point at a height set purely by the ratio advance/R: ~3% of R
// at half a radius of advance, 10% at one radius, 47% at two and a half. At R =
// 12 m over 30 m of road -- an ordinary pair of road handles -- the branches ran
// through each other 5.6 m in the air. The crossing only disappears once the
// advance reaches the whole circumference, and by then the turn is not a loop at
// all but a hump: the surface normal still points UP at the top, so a craft drives
// over it rather than round it.
//
// So the branches are separated ACROSS the road instead: the turn sways out to one
// side on the way up and to the other on the way down, and the two pass beside
// each other where they used to pass through. The profile has to be ODD about the
// crest -- tilting the whole plane of the loop does nothing, because both branches
// are then displaced by the same amount at the same height. `sin(t)*|sin(t)|` is
// odd about pi, zero at both ends, and quadratic there, so the turn leaves the
// road and rejoins it running dead straight: no sideways kink at the mouth, which
// a plain sin(t) would have left.
//
// The sway is sized from where the crossing actually lands -- enough to clear one
// carriageway width plus a metre, capped, and faded out entirely for a crossing
// that sits in the feet of the turn (under ~0.75 m), because down there the two
// branches are nearly tangent and meeting is what the base of a loop LOOKS like.
// A short turn is therefore still perfectly planar; only one that reaches down the
// road far enough to stab itself pays for it with a twist.
//
// Pure geometry: it walks a centreline and returns frames and triangles. What
// happens to a craft on it lives in RaceSim (surface-relative gravity, and
// falling off when there is not enough speed to hold the top).
namespace roadloop {

// A crossing at or below this height is left alone: it sits in the feet of the
// turn, where the two branches are nearly tangent and meeting reads as the base
// of a loop rather than as a fault. As a FRACTION of the radius, because the
// whole law is scale-free -- where the branches meet depends only on the ratio
// advance/R -- so a threshold in metres would leave a big turn weaving over a
// crossing that is proportionally as harmless as the one a small turn is allowed
// to keep. The sway fades in over the next kCrossFade of the radius.
constexpr float kCrossFree = 0.04f;   // of the radius
constexpr float kCrossFade = 0.06f;   // of the radius
// Ceiling on the sway, in carriageway widths. A turn that would need more than
// this to clear itself gets what this allows -- weaving half the track's width
// out into the scenery to fix a graze is a worse answer than the graze.
constexpr float kMaxSway = 1.5f;

// One loop the user asked for, named by the two control points at its ends
// (indices into roadPts, either order). Points move and vanish under the editor,
// so these are validated on every build, exactly like BridgeSpec.
//
// The pair marks the ground the turn covers: it leaves the road at the first point
// and comes back down on the second, and the ribbon between them is left out
// because the turn IS the road there.
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
    // The road's own direction under this station: horizontal, and pointing the
    // way the turn ADVANCES rather than the way the nose does. The two part
    // company over the top, where the tangent points back down the road while the
    // turn is still going forwards -- so anything that means "which way is this
    // craft heading" has to read this, not the tangent.
    glm::vec3 ahead{0.0f, 0.0f, 1.0f};
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
    // The centreline samples this turn covers on the ground, [sa, sb]. That stretch
    // is not road the loop flies over: it IS the loop, and the flat ribbon leaves
    // it out (see RoadSystem::loft). Without that the carriageway ran straight on
    // underneath and the turn read as a hoop propped up beside the road, not as
    // the road standing on end.
    int       sa = -1, sb = -1;
    // The two control points that named this turn, so the editor can point back
    // at the Spec it came from -- plan() skips specs it cannot honour, so the
    // built list does not line up with the spec list index for index.
    int       pa = -1, pb = -1;
    // How far the turn leans across the road to let its two branches pass (0 = it
    // never needed to), and whether it turns over at all: past an advance of
    // 2*pi*R the shape stops inverting and is a hump, which the editor should say
    // out loud rather than let the author find out at speed.
    float     sway    = 0.0f;
    bool      inverts = true;
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
                       const std::vector<Spec>& specs, float roadWidth);

// Append the carriageway ribbon of every loop to `md`. Winding matches the rest
// of the engine (CCW = front face) and the surface is two-sided in effect,
// because a loop is looked at from underneath for half of its length.
void build(const std::vector<Loop>& loops, float roadWidth, float texTile,
           fitzel::MeshData& md);

// The frame at arc length `arc`, CLAMPED to [0, length] -- not wrapped. A turn
// that advances is not a closed curve: its two ends are metres of road apart, so
// wrapping the end round to the start does not mean "the same place again", it
// means teleporting whatever asked back to the mouth. (It did. Riding to the end
// asks with exactly `length`, and length/length wrapped to 0, so every completed
// loop put the craft back at its entrance -- once as a jump, and before the mount
// test learned to tell the ends apart, for ever.)
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
