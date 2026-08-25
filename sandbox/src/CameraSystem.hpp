#pragma once

#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

struct Entity;

// The scene's cameras: every entity carrying a CameraComponent, resolved once a
// frame into the pose it looks from.
//
// WHY THIS EXISTS. The sandbox grew up around a single fitzel::Camera that
// everyone wrote into -- the editor's fly camera, the FPS walk, the showroom, the
// path recorder, an active Camera entity, and the racing sim's chase cam, which
// set the camera's position and angles from inside the flight model. That works
// exactly as long as there is one eye. The moment a second view is wanted, every
// one of those writers has to be told which camera it is writing to, and the
// flight model -- which has no business knowing what a viewport is -- has to be
// handed one. Split screen died on that.
//
// So the ownership is turned around: a camera is an ENTITY, the sim moves only
// the craft, and this computes where each camera ends up. A view is then a
// camera entity, two views are two camera entities, and nothing in the sim
// changes to get from one to the other.
//
// WHOSE CAMERA IT IS, IS THE HIERARCHY'S ANSWER. A follow camera trails its
// parent -- hang one on a craft and it is that craft's camera. There is no target
// field to point somewhere else, and no seat number: the object that owns the
// camera is the object it hangs on. Two craft with a camera child each are two
// players, without anything anywhere having to decide which is which.
namespace camerasys {

// Where a camera looks from this frame. A basis rather than yaw/pitch, because
// yaw/pitch cannot express a camera rolled with a craft going round a loop.
struct Pose {
    glm::vec3 position{0.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float     fov = 60.0f;
};

// A follow camera's settings, lifted off the component so a shot can also be
// asked for around an object that carries NO camera of its own -- watching a
// rival, whose craft the author never hung an eye on. Same numbers, same names.
struct FollowShot {
    glm::vec3 offset{0.0f, 3.0f, -9.0f}; // where the eye sits, in the target's frame
    float lookHeight = 1.4f;
    float stiffness  = 5.0f;
    float rollWith   = 0.0f;
    float fov        = 60.0f;
};

class CameraSystem {
public:
    // Resolve every camera entity in the scene, easing follow cameras toward
    // where they want to be over `dt` seconds of render time.
    //
    // THE RENDER CLOCK, NOT THE SIM CLOCK, and that is not the usual mistake.
    // What this smooths is a pose that has ALREADY been interpolated for this
    // frame: the craft moves a little every frame, whatever the fixed step is
    // doing underneath. Easing in whole sim steps instead means the camera
    // catches up in one, two or (on a fast frame) no portions while the craft
    // slides on smoothly -- and that unevenness reads as the craft hopping in
    // frame, which is exactly what it looked like.
    //
    // The reason a render-clock rate is usually wrong -- that it makes the
    // result depend on the frame rate -- is dealt with by the smoothing itself:
    // 1 - exp(-rate * dt) is the exact solution of the same exponential decay
    // over dt, so a camera catches up by the same fraction per SECOND on any
    // machine. (The naive rate * dt is what would drift.)
    //
    // Call AFTER whatever moves the scene (sim, physics, scripts, animation) and
    // before the frame is drawn, or every camera trails by one frame.
    void update(const std::vector<Entity>& entities, float dt);

    // The pose of camera entity `id`, or false if it has no camera component,
    // was not resolved this frame, or is not active.
    bool pose(int id, Pose& out) const;

    // The shot a follow camera with these settings would give around `target`
    // this frame -- for an object with no camera of its own, which is what
    // watching a rival is. Every camera entity goes through this same routine, so
    // spectating cannot drift into looking like a different game.
    //
    // `key` names the smoothing state (the eye eases, so it has to remember where
    // it was). Pass a NEGATIVE key: camera entity ids are non-negative and the two
    // must not collide. State under a negative key is the caller's to keep --
    // update() will not tidy it away as it does a camera entity's, because there
    // is no entity that could have gone missing. reset() drops it.
    Pose follow(int key, const Entity& target, const FollowShot& shot, float dt);

    // Forget every camera's smoothed position. For Play start/stop and scene
    // loads: a camera that kept its eased eye across a restart would swoop in
    // from wherever the last run left it.
    void reset();

private:
    // A follow camera's smoothed eye. Kept here rather than on the component
    // because it is live state, not authored state -- the same reason particle
    // pools live in ParticleSystem: a prefab, an undo step or Play's snapshot
    // copies the component around, and none of them should drag a camera's
    // easing history along.
    struct Chase {
        glm::vec3 eye{0.0f};
        glm::vec3 front{0.0f, 0.0f, -1.0f};
        // Where the craft was when `eye` was last written. The easing runs on the
        // eye's OFFSET FROM THE CRAFT, so it needs the anchor that offset was
        // measured against -- see the note on the smoothing in CameraSystem.cpp.
        glm::vec3 anchor{0.0f};
        bool      seeded = false;   // false = snap on the first frame, don't ease
    };

    std::unordered_map<int, Chase> m_chase;  // camera entity id -> eased state
    std::unordered_map<int, Pose>  m_pose;   // camera entity id -> this frame
};

} // namespace camerasys
