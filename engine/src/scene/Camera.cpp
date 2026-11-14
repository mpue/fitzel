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
    // The head's nod rides on the look's pitch. Straight up or down is as far
    // as it goes: past the pole the view would come back over upside down.
    const float pitchR = glm::radians(std::clamp(m_pitch + m_headPitch, -90.0f, 90.0f));
    // On the pole cos() of the rounded angle comes out a hair below zero, which
    // would tip the view a hair the wrong way round: there it is zero.
    const float cosP   = std::max(std::cos(pitchR), 0.0f);

    glm::vec3 front;
    front.x = std::cos(yawR) * cosP;
    front.y = std::sin(pitchR);
    front.z = std::sin(yawR) * cosP;

    m_front = glm::normalize(front);
    // Right is level and square to the heading: what cross(front, worldUp)
    // normalizes to at every pitch off the pole -- and, taken from the yaw
    // rather than from that cross product, still that ON the pole, where front
    // is parallel to world up and the cross product is the zero vector. So a
    // view straight down (the editor's top view) is exactly straight down, with
    // screen up where the yaw says, instead of being held half a degree short
    // of the pole to keep a NaN out of the view matrix.
    m_right = glm::vec3(-std::sin(yawR), 0.0f, std::cos(yawR));
    m_up = glm::normalize(glm::cross(m_right, m_front));
    if (m_headRoll != 0.0f) { // the head's lean, about the line of sight
        const float c = std::cos(glm::radians(m_headRoll));
        const float s = std::sin(glm::radians(m_headRoll));
        const glm::vec3 r = m_right * c - m_up * s;
        m_up    = m_up * c + m_right * s;
        m_right = r;
    }
}

} // namespace fitzel
