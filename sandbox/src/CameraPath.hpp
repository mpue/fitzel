#pragma once

#include <vector>

#include <glm/glm.hpp>

// Camera path recorder/player: a spline through recorded camera keyframes.

// A keyframe of camera state at a point in time along the path.
struct CamKey {
    float     t;     // seconds from the path start
    glm::vec3 pos;
    float     yaw;   // degrees (unwrapped across keys for smooth interpolation)
    float     pitch;
    float     fov;
};

// Centripetal-ish Catmull-Rom: a smooth curve passing through b and c, using the
// neighbours a and d for tangents. Works for float and glm::vec3 alike.
template <typename T>
T catmull(const T& a, const T& b, const T& c, const T& d, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * b) + (-a + c) * t +
                   (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
                   (-a + 3.0f * b - 3.0f * c + d) * t3);
}

// Sample the path at time `time`, writing the interpolated camera pose. Position
// and pose channels are Catmull-Rom smoothed; the path is clamped at both ends.
void samplePath(const std::vector<CamKey>& k, float time,
                glm::vec3& pos, float& yaw, float& pitch, float& fov);
