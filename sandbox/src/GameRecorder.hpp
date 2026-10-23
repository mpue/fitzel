#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "SceneTypes.hpp"   // Entity

// --- Game recorder ------------------------------------------------------------
//
// What a Play run looked like, kept so it can be watched again and turned into
// a video. Not an input recording: the sims here are not deterministic (the
// physics steps on the frame's dt, scripts read the wall clock), so replaying
// the keys would replay a different game. What is kept instead is the RESULT,
// frame by frame -- every entity's local transform, the active flag, a skinned
// figure's clip and time, the eye, the clock and the weather. Played back in
// the editor, that puts the scene exactly where the game had it without running
// a line of physics or script.
//
// A take is sparse: frame 0 holds every entity, each later frame only those
// whose pose changed since the frame before. A scene of a thousand objects in
// which a car and two figures move costs three entries a frame, not a thousand.
// Entities a script spawned during the run are kept as their scene JSON (the
// same writer scene files use), so the replay can put them back; ones that went
// away are marked gone.
namespace gamerec {

// The eye. A basis rather than yaw/pitch: a chase camera rolls with its craft,
// and the walker tilts the head, neither of which yaw/pitch can say.
struct CamState {
    glm::vec3 pos{0.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float     fov = 60.0f;
};

// One entity in one frame.
struct Pose {
    glm::vec3 pos{0.0f};     // localCenter
    glm::vec3 rot{0.0f};     // localRotation, degrees
    bool      active = true;
    // The skinned pose, if the figure has one: 0 none, 1 its Animation
    // component, 2 its animation graph (which also fades between two clips).
    std::int8_t  skin = 0;
    std::int16_t clip = -1;
    float        time = 0.0f;
    std::int16_t fromClip  = -1;
    float        fromTime  = 0.0f;
    float        fromShare = 0.0f;

    bool operator==(const Pose&) const = default;
};

struct Change {
    int  id   = 0;
    Pose pose;
    bool gone = false;   // left the scene this frame (pose unused)
};

struct Frame {
    double t = 0.0;          // game seconds since the take began
    CamState cam;
    float timeOfDay = 0.0f;  // hours
    float storm     = 0.0f;  // 0..1
    std::vector<Change> changes;
};

// An entity that was not there when the take began.
struct Spawn {
    int         id = 0;
    std::string json;        // projectio::writeEntityJson, dumped
};

struct Take {
    std::string        scene;    // the scene file it was recorded in (shown, not enforced)
    std::vector<Frame> frames;
    std::vector<Spawn> spawns;

    bool   empty() const { return frames.empty(); }
    double duration() const { return frames.empty() ? 0.0 : frames.back().t; }
    std::size_t bytes() const;   // roughly what it holds in memory
};

// id -> pose of every entity in the scene at one moment. Missing: not there.
using State = std::unordered_map<int, Pose>;

// The file: little-endian binary, "FREC" + version. False with `err` set when
// it cannot be written / is not a take this build reads.
bool save(const Take& take, const std::string& path, std::string& err);
bool load(Take& take, const std::string& path, std::string& err);

// --- Recording ---------------------------------------------------------------
class Recorder {
public:
    void begin(const std::string& scene);
    // One frame, after everything that moves has moved. `dt` is the game time
    // the frame advanced. `spawnJson` serializes an entity seen for the first
    // time after frame 0.
    void capture(float dt, const CamState& cam, float timeOfDay, float storm,
                 const std::vector<Entity>& entities,
                 const std::function<std::string(const Entity&)>& spawnJson);
    // Stops and hands the take over (empty if nothing was captured).
    Take end();

    bool        active() const { return m_on; }
    double      time() const { return m_t; }
    std::size_t frames() const { return m_take.frames.size(); }
    std::size_t bytes() const { return m_take.bytes(); }

private:
    Take   m_take;
    State  m_last;
    double m_t  = 0.0;
    bool   m_on = false;
};

// --- Playback ----------------------------------------------------------------
// Random access into a take: the state at any time, interpolated between the
// two frames either side. Forward steps apply one frame's changes; a step back
// starts again from the nearest cached state (one every kKeyEvery frames).
class Cursor {
public:
    static constexpr int kKeyEvery = 120;

    void reset(const Take* take);
    bool valid() const { return m_take && !m_take->frames.empty(); }

    // Fills `out` with every entity's pose at `t` (clamped to the take).
    void sample(double t, State& out, CamState& cam, float& timeOfDay, float& storm);

    // The frame the last sample was at or after, for display.
    int index() const { return m_index; }

private:
    void seek(int i);   // m_state := state after frame i

    const Take*        m_take = nullptr;
    std::vector<State> m_keys;   // state after frame k * kKeyEvery
    State              m_state;
    int                m_index = -1;
};

// Interpolation, exposed for the check. Rotations go through quaternions: a
// physics body's Euler triple can flip to an equivalent one between frames,
// and lerping the numbers would spin it round the long way.
Pose     lerp(const Pose& a, const Pose& b, float alpha);
CamState lerp(const CamState& a, const CamState& b, float alpha);

// --- Entity glue ---------------------------------------------------------------
Pose poseOf(const Entity& e);
// The transform and the active flag (before resolveHierarchy).
void applyTransform(Entity& e, const Pose& p);
// The skinned pose (after the editor has cleared graph poses, before skinning).
// Holds the Animation component's clock still so the skinning pass does not
// run it on.
void applySkin(Entity& e, const Pose& p);

// --- Clock ---------------------------------------------------------------------
// The frame clock, which a video export takes over: while `fixed`, every frame
// is exactly 1/fps long whatever the frame really cost, so the exposure,
// the clouds and the water move as they would at that rate. Afterwards it
// carries on from where the fixed clock got to -- never backwards, which would
// hand the frame a negative dt.
struct Clock {
    double offset = 0.0;
    double virt   = 0.0;
    double step   = 0.0;
    bool   fixed  = false;

    double now(double wall) {
        if (fixed) return virt += step;
        return wall + offset;
    }
    void beginFixed(double wall, double fps) {
        virt  = wall + offset;
        step  = 1.0 / std::max(fps, 1.0);
        fixed = true;
    }
    void endFixed(double wall) {
        if (!fixed) return;
        offset = virt - wall;
        fixed  = false;
    }
};

} // namespace gamerec
