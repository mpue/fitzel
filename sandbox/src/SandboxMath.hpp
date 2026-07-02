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
