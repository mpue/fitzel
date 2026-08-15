#include "CameraSystem.hpp"

#include <cmath>
#include <iterator>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Component.hpp"
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

void CameraSystem::update(const std::vector<Entity>& entities, float dt) {
    m_pose.clear();

    for (const Entity& e : entities) {
        const auto* cc = e.components.get<CameraComponent>();
        if (!cc || !e.activeInHierarchy) continue;

        Pose p;
        p.fov = cc->fov;

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

        // The offset is the camera's own local position, applied in the parent's
        // HEADING frame: yaw only, deliberately. Taking the parent's full
        // orientation would roll the camera with a banking craft and pitch it
        // with a diving one, which is a ride, not a view.
        const float yaw = glm::radians(target->rotation.y);
        const glm::vec3 off = e.localCenter;
        const glm::vec3 wanted =
            target->center +
            glm::vec3(off.x * std::cos(yaw) + off.z * std::sin(yaw),
                      off.y,
                      -off.x * std::sin(yaw) + off.z * std::cos(yaw));
        const glm::vec3 aim = target->center +
                              glm::vec3(0.0f, cc->lookHeight, 0.0f);

        Chase& ch = m_chase[e.id];
        if (!ch.seeded) {
            // First sight: stand where the shot wants to be. Easing up from a
            // default-constructed eye would open every run with a swoop in from
            // the world origin.
            ch.eye = wanted;
            ch.seeded = true;
        } else {
            // Exponential catch-up: the fraction of the remaining distance
            // covered in dt seconds. Same result at 30 fps as at 300, and it
            // moves every frame, which is what keeps it in step with a craft
            // whose pose is interpolated every frame.
            const float k = 1.0f - std::exp(-glm::max(cc->stiffness, 0.0f) *
                                            glm::max(dt, 0.0f));
            ch.eye += (wanted - ch.eye) * k;
        }

        // Aim at the target. A camera sitting exactly on its aim point has no
        // direction to look in -- keep the last one rather than normalizing a
        // zero vector, which is how a view matrix ends up full of NaN.
        const glm::vec3 d = aim - ch.eye;
        if (glm::length(d) > 1e-4f) ch.front = glm::normalize(d);

        p.position = ch.eye;
        p.front    = ch.front;
        // Level with the world, not with the craft. A camera that follows a
        // craft round a vertical loop wants the craft's own up instead -- that
        // is the sim's `loopUp`, which does not exist out here yet; until it
        // does, a loop is the one manoeuvre this gets wrong.
        p.up = glm::vec3(0.0f, 1.0f, 0.0f);
        m_pose[e.id] = p;
    }

    // Cameras that went away (deleted, deactivated, unparented) must not keep
    // their eased eye: coming back should stand the shot up fresh, not resume a
    // catch-up from wherever the scene was when they left.
    for (auto it = m_chase.begin(); it != m_chase.end();)
        it = (m_pose.count(it->first) == 0) ? m_chase.erase(it) : std::next(it);
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
}

} // namespace camerasys
