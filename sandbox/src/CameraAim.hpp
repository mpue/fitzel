#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// The rotation (Euler degrees, as Entity::rotation holds it) that makes a camera
// entity look the way a view does.
//
// A camera entity looks down its own -Z with +Y up, turned by
// quat(radians(rotation)) -- see CameraSystem. So the rotation wanted is the one
// that takes -Z onto the view's forward and +Y onto its up. Built from the
// view's BASIS rather than its yaw/pitch, because the editor camera does not
// always have a yaw/pitch to give: a standard view (top, front, ...) sets the
// basis outright, and so does looking through a rolled camera. seqcheck holds
// the result against the view it came from.
namespace camaim {

inline glm::vec3 rotationFor(const glm::vec3& front, const glm::vec3& up) {
    const glm::vec3 f = glm::normalize(front);
    // Up made square to forward first: quatLookAt assumes it is, and a view's
    // up that leans a hair off ninety degrees would otherwise tilt the roll.
    glm::vec3 u = up - f * glm::dot(up, f);
    if (glm::dot(u, u) < 1e-8f) u = std::abs(f.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
    const glm::quat q = glm::quatLookAt(f, glm::normalize(u));
    return glm::degrees(glm::eulerAngles(q));
}

} // namespace camaim
