#include "CameraSystem.hpp"

#include <cmath>
#include <iterator>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Component.hpp"
#include "MultiShot.hpp"     // the multishot camera's director
#include "SandboxMath.hpp"   // sceneHeading()
#include "SceneTypes.hpp"

namespace camerasys {

namespace {

const Entity* findById(const std::vector<Entity>& entities, int id) {
    if (id < 0) return nullptr;
    for (const Entity& e : entities)
        if (e.id == id) return &e;
    return nullptr;
}

} // namespace

// Out of line purely so the header can forward-declare multishot::Director: a
// unique_ptr member needs the full type where the destructor is written, and
// nowhere else.
CameraSystem::CameraSystem() = default;
CameraSystem::~CameraSystem() = default;

void CameraSystem::setGround(std::function<float(float, float)> ground) {
    m_ground = std::move(ground);
}

void CameraSystem::update(const std::vector<Entity>& entities, float dt) {
    m_pose.clear();

    for (const Entity& e : entities) {
        const auto* cc = e.components.get<CameraComponent>();
        if (!cc || !e.activeInHierarchy) continue;

        Pose p;
        p.fov = cc->fov;

        if (cc->mode == CameraComponent::Multishot) {
            // A multishot camera SHOOTS an object rather than riding one, so it
            // is the one camera that names its subject instead of reading it off
            // the hierarchy (see CameraComponent::shotTarget). The parent is the
            // fallback, so parenting one to a car still does the obvious thing.
            const Entity* subj = findById(entities, cc->shotTarget >= 0
                                                        ? cc->shotTarget : e.parent);
            // No subject means no shot. Left out of the frame's set exactly like
            // a follow camera with no parent: a camera that quietly starts
            // orbiting the world origin is worse than one that stops being a
            // view, because the second is visibly a mistake.
            if (!subj || !subj->activeInHierarchy) { m_shots.erase(e.id); continue; }

            multishot::Subject sub;
            sub.center = subj->center;
            sub.half   = subj->half;
            // The heading from the rotation, not from rotation.y -- the same trap
            // the chase camera fell into, and the same fix (see sceneHeading).
            sub.yaw    = sceneHeading(subj->rotation);

            multishot::Settings st = cc->shots;
            st.fov = cc->fov;   // the camera's own lens is the base every shot scales

            auto& dir = m_shots[e.id];
            if (!dir) dir = std::make_unique<multishot::Director>();
            m_pose[e.id] = dir->update(sub, st, dt, m_ground);
            m_chase.erase(e.id);   // not a follow camera: no eased eye to keep
            continue;
        }
        m_shots.erase(e.id);       // ...and the other way round

        if (cc->mode != CameraComponent::Follow) {
            // Static: the entity's own transform IS the pose. -Z is forward, the
            // convention the camera gizmo already draws its frustum along.
            const glm::quat q(glm::radians(e.rotation));
            p.position = e.center;
            p.front    = glm::normalize(q * glm::vec3(0.0f, 0.0f, -1.0f));
            p.up       = glm::normalize(q * glm::vec3(0.0f, 1.0f, 0.0f));
            m_pose[e.id] = p;
            m_chase.erase(e.id);   // no eased state to keep while it stands still
            continue;
        }

        // Follow: trail the parent. No parent means nothing to follow -- leave
        // the camera out of the frame's set rather than invent a target, so a
        // camera dragged out of its craft in the hierarchy stops being a view
        // instead of silently watching the world origin.
        const Entity* target = findById(entities, e.parent);
        if (!target) continue;

        FollowShot shot;
        shot.offset     = e.localCenter;
        shot.lookHeight = cc->lookHeight;
        shot.stiffness  = cc->stiffness;
        shot.rollWith   = cc->rollWith;
        shot.fov        = cc->fov;
        m_pose[e.id] = follow(e.id, *target, shot, dt);
    }

    // Cameras that went away (deleted, deactivated, unparented) must not keep
    // their eased eye: coming back should stand the shot up fresh, not resume a
    // catch-up from wherever the scene was when they left. Negative keys are
    // spectator shots -- they have no camera entity that could have gone away,
    // so they are not swept here (see follow()).
    for (auto it = m_chase.begin(); it != m_chase.end();)
        it = (it->first >= 0 && m_pose.count(it->first) == 0)
                 ? m_chase.erase(it) : std::next(it);
    // Same for the running edits: a camera that was deleted or switched off has
    // no shot in flight, and coming back should open on shot one rather than
    // resume something the viewer never saw the start of.
    for (auto it = m_shots.begin(); it != m_shots.end();)
        it = (m_pose.count(it->first) == 0) ? m_shots.erase(it) : std::next(it);
}

Pose CameraSystem::follow(int key, const Entity& target, const FollowShot& shot,
                          float dt) {
    Pose p;
    p.fov = shot.fov;
    // WHICH FRAME THE OFFSET IS APPLIED IN depends on how far the followed
    // object is from level, and it has to, because the two ends of that
    // range want opposite things.
    //
    // Level flying: the HEADING frame, yaw only. Rolling the camera with a
    // banking craft is a ride, not a view, and pitching it with every dip of
    // the nose makes the horizon seasick.
    //
    // Round a vertical loop: the craft's OWN frame. "Behind, in yaw" stops
    // meaning anything once the craft points straight up, and at the top it
    // is inverted -- a camera left level watches its craft go over from
    // outside, which reads as the track moving rather than the craft. This
    // is what the flight model used to do with the loop's surface normal;
    // the craft's transform carries the same information, so the loop does
    // not have to be a special case out here.
    //
    // Between the two: one blend factor for the offset frame, the up vector
    // and the look-at point, so they can never disagree. Geometry alone puts
    // it at 0 until the craft is past ~53 degrees off level -- normal banking
    // is well inside that, so nothing changes for ordinary racing.
    //
    // `rollWith` then raises a FLOOR under that, for authors who want the
    // ride rather than the view: at 1 the shot is in the craft's frame the
    // whole time and banks with it. A floor rather than a replacement,
    // because the loop case above is not a preference -- it is the only thing
    // that works when the nose points at the sky.
    const glm::vec3 wUp{0.0f, 1.0f, 0.0f};
    const glm::quat q(glm::radians(target.rotation));
    const glm::vec3 craftUp = glm::normalize(q * wUp);
    const float level = glm::dot(craftUp, wUp);   // 1 level, 0 vertical, -1 inverted
    const float roll  = glm::max(1.0f - glm::smoothstep(0.0f, 0.6f, level),
                                 glm::clamp(shot.rollWith, 0.0f, 1.0f));

    // The heading frame, from the rotation itself rather than from
    // rotation.y. Those are the same number only while the craft is level:
    // a banking craft's Euler triple spreads the turn across all three
    // components (see sceneHeading), and reading .y off one swung the chase
    // shot out sideways every time the craft leaned.
    const float yaw = sceneHeading(target.rotation);
    const glm::vec3 off = shot.offset;
    const glm::vec3 offFlat(off.x * std::cos(yaw) + off.z * std::sin(yaw),
                            off.y,
                            -off.x * std::sin(yaw) + off.z * std::cos(yaw));
    const glm::vec3 offFull = q * off;
    // Where the eye wants to sit RELATIVE TO THE CRAFT. Kept as an offset
    // rather than a world point on purpose -- see the easing below.
    const glm::vec3 desired = glm::mix(offFlat, offFull, roll);

    // Up, and the point aimed at, in the same blended frame -- on a loop the
    // craft's "above" is out along the track's surface, not toward the sky.
    glm::vec3 up = glm::mix(wUp, craftUp, roll);
    up = (glm::length(up) > 1e-4f) ? glm::normalize(up) : wUp;
    const glm::vec3 aim = target.center + up * shot.lookHeight;

    Chase& ch = m_chase[key];
    if (!ch.seeded) {
        // First sight: stand where the shot wants to be. Easing up from a
        // default-constructed eye would open every run with a swoop in from
        // the world origin.
        ch.eye = target.center + desired;
        ch.seeded = true;
    } else {
        // Exponential catch-up, run on the eye's OFFSET FROM THE CRAFT rather
        // than on its world position. Both settle to the same shot; the
        // difference is what happens when frame times are uneven, and they
        // always are.
        //
        // Chasing a world point, the eye trails a moving craft by roughly
        // speed/stiffness -- and that trail is a function of dt, so it
        // breathes in and out with every long or short frame. Measured on a
        // real 80 m/s run whose frames ranged 8..35 ms, that came to 30 cm of
        // back-and-forth in a single frame: the craft twitching against a
        // world that was itself perfectly steady.
        //
        // Expressed as an offset, the craft's own motion drops out of the
        // arithmetic entirely. What decays is only the DIFFERENCE between
        // where the eye sits on the craft and where it wants to sit, so a
        // craft at constant speed gives a dead-still shot at any frame rate,
        // while acceleration, braking and turning still swing the camera out
        // exactly as before. A respawn now carries the camera with the craft
        // instead of leaving it to swoop across the map, for the same reason.
        const float decay = std::exp(-glm::max(shot.stiffness, 0.0f) *
                                     glm::max(dt, 0.0f));
        const glm::vec3 was = ch.eye - ch.anchor;   // offset it had last frame
        ch.eye = target.center + desired + (was - desired) * decay;
    }
    ch.anchor = target.center;

    // Aim at the target. A camera sitting exactly on its aim point has no
    // direction to look in -- keep the last one rather than normalizing a
    // zero vector, which is how a view matrix ends up full of NaN.
    const glm::vec3 d = aim - ch.eye;
    if (glm::length(d) > 1e-4f) ch.front = glm::normalize(d);

    p.position = ch.eye;
    p.front    = ch.front;
    p.up       = up;
    return p;
}

bool CameraSystem::pose(int id, Pose& out) const {
    const auto it = m_pose.find(id);
    if (it == m_pose.end()) return false;
    out = it->second;
    return true;
}

void CameraSystem::reset() {
    m_chase.clear();
    m_pose.clear();
    m_shots.clear();
}

multishot::Director* CameraSystem::director(int id) {
    const auto it = m_shots.find(id);
    return (it == m_shots.end()) ? nullptr : it->second.get();
}

const multishot::Director* CameraSystem::director(int id) const {
    const auto it = m_shots.find(id);
    return (it == m_shots.end()) ? nullptr : it->second.get();
}

} // namespace camerasys
