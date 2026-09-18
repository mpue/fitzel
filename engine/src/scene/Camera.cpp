#include "fitzel/scene/Camera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace fitzel {

Camera::Camera(glm::vec3 position, float yawDegrees, float pitchDegrees)
    : m_position(position), m_yaw(yawDegrees), m_pitch(pitchDegrees) {
    updateVectors();
}

glm::mat4 Camera::viewMatrix() const {
    return glm::lookAt(m_position, m_position + m_front, m_up);
}

glm::mat4 Camera::projectionMatrix(float aspect) const {
    return projectionMatrix(aspect, m_near, m_far);
}

glm::mat4 Camera::projectionMatrix(float aspect, float nearZ, float farZ) const {
    if (m_ortho) {
        const float h = m_orthoHalfH, w = m_orthoHalfH * aspect;
        return glm::ortho(-w, w, -h, h, nearZ, farZ);
    }
    return glm::perspective(glm::radians(m_fovDegrees), aspect, nearZ, farZ);
}

float Camera::metresPerPixel(float dist, float viewportH) const {
    const float span = m_ortho ? 2.0f * m_orthoHalfH
                               : 2.0f * dist * std::tan(glm::radians(m_fovDegrees) * 0.5f);
    return span / std::max(viewportH, 1.0f);
}

void Camera::processKeyboard(Direction dir, float deltaSeconds) {
    const float velocity = moveSpeed * deltaSeconds;
    switch (dir) {
        case Direction::Forward:  m_position += m_front * velocity;   break;
        case Direction::Backward: m_position -= m_front * velocity;   break;
        case Direction::Left:     m_position -= m_right * velocity;   break;
        case Direction::Right:    m_position += m_right * velocity;   break;
        case Direction::Up:       m_position += m_worldUp * velocity; break;
        case Direction::Down:     m_position -= m_worldUp * velocity; break;
    }
}

void Camera::processMouse(float deltaX, float deltaY, bool constrainPitch) {
    m_yaw   += deltaX * mouseSens;
    m_pitch += deltaY * mouseSens;

    if (constrainPitch) {
        m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);
    }
    updateVectors();
}

void Camera::processScroll(float deltaY) {
    m_fovDegrees = std::clamp(m_fovDegrees - deltaY, 1.0f, 90.0f);
}

void Camera::updateVectors() {
    const float yawR   = glm::radians(m_yaw);
    const float pitchR = glm::radians(m_pitch);

    glm::vec3 front;
    front.x = std::cos(yawR) * std::cos(pitchR);
    front.y = std::sin(pitchR);
    front.z = std::sin(yawR) * std::cos(pitchR);

    m_front = glm::normalize(front);
    // Straight up or straight down is a pole: front is parallel to world up, the
    // cross product is the zero vector, and normalizing it yields NaN -- which
    // spreads through the view matrix into every pass that frame and paints the
    // screen white. A chase camera aimed at the craft it sits directly under
    // gets there on its own, so the camera has to survive it: keep the previous
    // right vector, which still points sideways and is what the pitch was
    // swinging around anyway.
    const glm::vec3 r  = glm::cross(m_front, m_worldUp);
    const float     rl = glm::length(r);
    if (rl > 1e-6f) m_right = r / rl;
    m_up = glm::normalize(glm::cross(m_right, m_front));
}

} // namespace fitzel
