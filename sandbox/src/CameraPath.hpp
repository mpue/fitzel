#pragma once

#include <string>
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
// the "Camera path" window contents.
//
// THE PATH IS SCENE DATA. It used to live only in a campath.txt beside the
// executable, which is fine for previewing a move in the editor and useless for
// anything else: one file for every project, nothing in it travels with a scene,
// and a shipped game never sees it at all. It is written into the scene now (see
// blob() / setBlob()), and the file buttons in the panel are an import/export
// between projects rather than the place it is kept.
//
// That is what makes `playOnStart` mean anything: an opening flythrough has to be
// in the level, not in a text file in the build folder.
class CameraPathRecorder {
public:
    // Run the game's opening move, if this scene has one. Called by Play (and by
    // the shipped player's boot) -- a no-op with the flag off or with fewer than
    // two keys, which is what a path that is not a path looks like.
    void beginAutoPlay();

    // Give up an auto-started playback because the player wants to move. Only
    // an AUTO-started one: a preview the author pressed Play on in the panel is
    // not something the movement keys should be able to cancel, or scrubbing a
    // path with the fly camera would be impossible.
    void interrupt();

    // True while the opening move is running. The caller uses it to know the
    // camera is not the player's this frame.
    bool autoPlaying() const { return m_playing && m_auto; }

    // `allowPlay` gates playback (e.g. off while a vehicle owns the camera).
    void update(fitzel::Camera& cam, float dt, bool allowPlay);
    void panel(fitzel::Camera& cam);

    // --- Scene data -----------------------------------------------------------
    // The keyframes as one compact space-separated blob (7 floats each), the same
    // scheme the painted grass and the terrain edits use -- one JSON string
    // instead of a line per number.
    std::string blob() const;
    void        setBlob(const std::string& s);

    // How the move behaves. Public because they are saved with the scene, and a
    // scene setting the editor cannot reach is a setting nobody can change.
    bool  playOnStart = false; // run it the moment the game starts
    bool  loop        = false; // ...and again, and again (an attract screen)
    float speed       = 1.0f;  // playback speed multiplier

private:
    void append(fitzel::Camera& cam, float t); // snapshot the camera at time t
    void save() const;
    void load();

    std::vector<CamKey> m_keys;
    bool  m_playing        = false;
    // Whether the run under way was started by the game rather than by the
    // panel. Only an auto one gives the camera back on input, and only an auto
    // one is a cinematic; the other is an author looking at their own move.
    bool  m_auto           = false;
    bool  m_recording      = false;
    float m_time           = 0.0f;  // current play/record/scrub time
    float m_keySpacing     = 2.0f;  // seconds granted to a manually added key
    float m_recordInterval = 0.15f; // seconds between samples while recording
    float m_recordAccum    = 0.0f;
};
