#pragma once

#include <vector>

#include <glm/glm.hpp>

// Small math / noise helpers used across the sandbox (vegetation placement,
// road clearance, viewport picking).

// Smooth 2D value noise (~0..1) for meadow patchiness in vegetation placement.
float vhash2(float x, float z);
float valNoise2(float x, float z);

// A point that ENDS one run of a polyline and starts the next, so several
// separate lines can travel as one flat vector<vec2> -- which is what the
// vegetation's tile workers need (they take a copy and read it from another
// thread, so a vector-of-vectors would cost them a second allocation each).
//
// Without it, joining two roads' centrelines end to end would invent a segment
// between them and mow a strip of grass along a road that is not there.
constexpr glm::vec2 kLineBreak{3.0e38f, 3.0e38f};
inline bool isLineBreak(const glm::vec2& p) { return p.x >= 1.0e38f; }

// Squared distance from (x,z) to a polyline in the XZ plane. Returns a huge
// value for an empty/degenerate line, so callers can test "< clearance^2"
// unconditionally. Used to keep vegetation off the road. Segments that touch a
// kLineBreak point are skipped -- see above.
float roadDistanceSq(const std::vector<glm::vec2>& line, float x, float z);

// Ray vs AABB (slab test). Returns the entry distance, or -1 on a miss.
float rayAABB(const glm::vec3& ro, const glm::vec3& rd,
              const glm::vec3& bmin, const glm::vec3& bmax);

// Ray vs triangle (Moller-Trumbore). Returns the hit distance along the ray, or
// -1 on a miss. Double-sided: picking a face you are looking at from inside a
// shape has to work, and a modelled mesh is routinely open.
float rayTriangle(const glm::vec3& ro, const glm::vec3& rd,
                  const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);

// --- Splines -----------------------------------------------------------------
// Centripetal Catmull-Rom (alpha = 0.5) through b and c. The uniform variant
// (CameraPath's catmull()) assumes evenly spaced control points; where authored
// points are not -- a tight corner right after a long straight is the usual case
// -- it overshoots and can loop back on itself. Knot spacing by sqrt(distance)
// removes that: the curve stays inside the control polygon and corners come out
// round rather than bulged. Falls back to the uniform form on coincident points.
glm::vec2 catmullCentripetal(const glm::vec2& p0, const glm::vec2& p1,
                             const glm::vec2& p2, const glm::vec2& p3, float t);

// Densely sample the centripetal spline through `pts` (world XZ). Sample density
// follows span length -- roughly one sample every two metres -- so control point
// i is *not* at a fixed sample stride: pass `ptSample` to get the sample index of
// each control point (one entry per point, plus a closing one for the last point
// or the loop seam). `closed` welds the line into a ring (needs >= 3 points).
//
// One definition of "how a spline curves", shared by the road and by the
// fence/wall/track paths, so the two can never drift into bending differently.
std::vector<glm::vec2> sampleSpline(const std::vector<glm::vec2>& pts, bool closed,
                                    std::vector<int>* ptSample = nullptr);

// --- Attitude ----------------------------------------------------------------
// The entity Euler triple (degrees, X/Y/Z as SceneTypes stores them) that renders
// as an AIRCRAFT attitude: yaw about world up, pitch about the craft's own right
// axis, roll about its own nose.
//
// It exists because those are not the same thing here. A scene rotation is
// composed by ImGuizmo as X, then Y, then Z -- which in world terms rolls about
// the world Z axis LAST. Feed a bank straight into rotation.z and a craft heading
// north banks correctly, one heading east tips its nose 22 degrees into the sky,
// and one heading west tips it into the ground. Pitch is unaffected (it is the
// innermost axis, so it is already the craft's own), and so is a craft that never
// rolls -- which is why the car was fine and the gliders were not.
//
// Near vertical (pitch within a fraction of a degree of straight up or down) yaw
// and roll stop being separable; the conversion picks the solution with no yaw,
// which is the standard gimbal-lock fallback and is what a loop's apex hits.
glm::vec3 attitudeEuler(float yawDeg, float pitchDeg, float rollDeg);

// Which way an entity with this Euler triple points, as a heading in radians in
// the convention every craft here uses: dir = (sin H, 0, cos H), i.e.
// atan2(nose.x, nose.z) for a +Z nose.
//
// NOT the same as `rotation.y`, and that is the whole reason it exists. The
// triple is only ever "whatever composes to the wanted orientation" (see
// attitudeEuler): as soon as an object rolls, the middle component stops being
// the heading -- a craft flying due east with 22 degrees of bank stores y = 68.
// Anything that wants a facing must ask the rotation, not one of its numbers.
//
// Falls back to the craft's own right axis when the nose is within a whisker of
// vertical, where a nose direction has no horizontal part to read a heading from.
float sceneHeading(const glm::vec3& eulerDeg);
