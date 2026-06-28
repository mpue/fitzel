#include "fitzel/scene/Camera.hpp"

#include <algorithm>

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
    return glm::perspective(glm::radians(m_fovDegrees), aspect, m_near, m_far);
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
    m_right = glm::normalize(glm::cross(m_front, m_worldUp));
    m_up    = glm::normalize(glm::cross(m_right, m_front));
}

} // namespace fitzel
