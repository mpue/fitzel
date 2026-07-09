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

namespace fitzel { class Camera; }

// Records the fly camera into a spline path and plays it back. update() advances
// recording/playback each frame (driving the camera while playing); panel() draws
// the "Camera path" window contents. Save/Load persist to campath.txt.
class CameraPathRecorder {
public:
    // `allowPlay` gates playback (e.g. off while a vehicle owns the camera).
    void update(fitzel::Camera& cam, float dt, bool allowPlay);
    void panel(fitzel::Camera& cam);

private:
    void append(fitzel::Camera& cam, float t); // snapshot the camera at time t
    void save() const;
    void load();

    std::vector<CamKey> m_keys;
    bool  m_playing        = false;
    bool  m_recording      = false;
    float m_time           = 0.0f;  // current play/record/scrub time
    float m_speed          = 1.0f;  // playback speed multiplier
    bool  m_loop           = false;
    float m_keySpacing     = 2.0f;  // seconds granted to a manually added key
    float m_recordInterval = 0.15f; // seconds between samples while recording
    float m_recordAccum    = 0.0f;
};
