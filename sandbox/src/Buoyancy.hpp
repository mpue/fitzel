#pragma once

#include <algorithm>

#include <glm/glm.hpp>

// What water does to one point of a floating body: lift it as far as it is
// under, and drag it towards moving with the water.
//
// A body floats on a handful of these -- its bottom corners and its middle --
// each carrying an equal share of its weight. Pushed at the POINT rather than at
// the centre, so a boat lifted at the bow by a wave pitches, and one with a
// wave under its side rolls: the whole difference between a thing floating and
// a thing being held at the water's height.
//
// THE AUTHOR SAYS HOW DEEP IT SITS, NOT HOW DENSE IT IS. Density means nothing
// to someone placing a crate; "it sits a third of a metre deep" means exactly
// what it looks like. A point that deep carries exactly its share of the
// weight, so a body at rest settles at that depth whatever its mass.
//
// Pure arithmetic, no physics engine: main applies the result, oceancheck
// floats a box on it.
namespace ocean {

struct FloatPoint {
    glm::vec3 position{0.0f};   // world
    glm::vec3 velocity{0.0f};   // of the body at that point
};

struct FloatParams {
    float sitDepth = 0.3f;      // m under the surface at rest
    float maxDepth = 1.0f;      // m: deeper than this adds no more lift (the body is under)
    float drag     = 1.2f;      // 1/s: how quickly it comes to move with the water
};

// The force on one point carrying `weightShare` (its share of m*g, newtons).
// `surfaceY` and `waterVel` are the water's at that point. `submerged` comes
// back 0..1, for whoever wants to know how wet it is (splashes, wakes).
inline glm::vec3 floatForce(const FloatPoint& p, float weightShare, float surfaceY,
                            glm::vec3 waterVel, const FloatParams& fp, float& submerged) {
    const float under = surfaceY - p.position.y;
    if (under <= 0.0f) { submerged = 0.0f; return glm::vec3(0.0f); }
    const float d = std::min(under, std::max(fp.maxDepth, fp.sitDepth));
    submerged = std::clamp(d / std::max(fp.maxDepth, 1e-3f), 0.0f, 1.0f);
    // Lift in proportion to depth: at sitDepth exactly the point's share of
    // the weight -- the body's resting draught, whatever it weighs.
    const glm::vec3 lift(0.0f, weightShare * d / std::max(fp.sitDepth, 1e-3f), 0.0f);
    // Water drags the point toward its own motion, more the deeper it is in.
    // Scaled by the point's mass (weight / g) so heavy and light bodies settle
    // alike.
    const float  mass = weightShare / 9.81f;
    const float  wet  = std::clamp(d / std::max(fp.sitDepth, 1e-3f), 0.0f, 1.5f);
    const glm::vec3 drag = (waterVel - p.velocity) * (fp.drag * mass * wet);
    return lift + drag;
}

} // namespace ocean
