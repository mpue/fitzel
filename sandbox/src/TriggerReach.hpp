#pragma once

// What a Trigger can sense besides the player: the dynamic physics bodies in the
// scene. A body counts as inside once its BOX touches the trigger's sphere, not
// its centre -- a crate two metres across could otherwise lie half over a
// trigger and never set it off. The box is the one the collider is built from
// (centre, half-extents, rotation), so what triggers is what visibly arrives.

#include "Component.hpp"
#include "SceneTypes.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace triggerreach {

struct Body {
    int       id = -1;
    glm::vec3 center{0.0f};
    glm::vec3 half{0.0f};
    glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
};

// The dynamic bodies a trigger may sense this frame. Static colliders never
// arrive anywhere -- one standing in a trigger would hold its gate open for the
// whole run -- so they are left out, and so is the player's own craft (`skipA`,
// `skipB`): it is the player, and "physics objects only" must not fire on it.
inline std::vector<Body> collect(const std::vector<Entity>& entities, int skipA, int skipB) {
    std::vector<Body> out;
    for (const Entity& e : entities) {
        if (!e.activeInHierarchy || e.id == skipA || e.id == skipB) continue;
        const auto* ph = e.components.get<PhysicsComponent>();
        if (!ph || !ph->dynamic) continue;
        out.push_back({e.id, e.center, e.half, glm::quat(glm::radians(e.rotation))});
    }
    return out;
}

// Does the sphere (c, r) touch the body's box?
inline bool touches(const Body& b, const glm::vec3& c, float r) {
    const glm::vec3 local   = glm::inverse(b.rot) * (c - b.center);
    const glm::vec3 nearest = glm::clamp(local, -b.half, b.half);
    const glm::vec3 d       = local - nearest;
    return glm::dot(d, d) <= r * r;
}

// Is any body but `self` (the trigger's own object, if it is one) inside?
inline bool anyInside(const std::vector<Body>& bodies, const glm::vec3& c, float r, int self) {
    for (const Body& b : bodies)
        if (b.id != self && touches(b, c, r)) return true;
    return false;
}

} // namespace triggerreach
