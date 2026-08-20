#pragma once

#include <vector>

#include <glm/glm.hpp>

// Small math / noise helpers used across the sandbox (vegetation placement,
// road clearance, viewport picking).

// Smooth 2D value noise (~0..1) for meadow patchiness in vegetation placement.
float vhash2(float x, float z);
float valNoise2(float x, float z);

// Squared distance from (x,z) to a polyline in the XZ plane. Returns a huge
// value for an empty/degenerate line, so callers can test "< clearance^2"
// unconditionally. Used to keep vegetation off the road.
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
