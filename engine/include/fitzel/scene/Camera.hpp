#pragma once

#include <glm/glm.hpp>

namespace fitzel {

// A first-person fly camera. Holds position/orientation and produces the
// view/projection matrices. Movement and look are driven by the application
// (see Input) through processKeyboard()/processMouse().
class Camera {
public:
    enum class Direction { Forward, Backward, Left, Right, Up, Down };

    explicit Camera(glm::vec3 position = {0.0f, 0.0f, 3.0f},
                    float yawDegrees = -90.0f, float pitchDegrees = 0.0f);

    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix(float aspect) const;

    void processKeyboard(Direction dir, float deltaSeconds);
    void processMouse(float deltaX, float deltaY, bool constrainPitch = true);
    void processScroll(float deltaY);

    const glm::vec3& position() const { return m_position; }
    void setPosition(const glm::vec3& p) { m_position = p; }

    const glm::vec3& front() const { return m_front; }
    const glm::vec3& up()    const { return m_up; }
    const glm::vec3& right() const { return m_right; }

    float fov()       const { return m_fovDegrees; }
    void  setFov(float degrees) { m_fovDegrees = degrees; }
    float nearPlane() const { return m_near; }
    float farPlane()  const { return m_far; }
    void  setFarPlane(float far) { m_far = far; }

    float yaw()   const { return m_yaw; }
    float pitch() const { return m_pitch; }
    void  setYaw(float deg)   { m_yaw = deg;   updateVectors(); }
    void  setPitch(float deg) { m_pitch = deg; updateVectors(); }

    float moveSpeed   = 3.0f;   // units per second
    float mouseSens   = 0.1f;   // degrees per pixel

private:
    void updateVectors();

    glm::vec3 m_position;
    glm::vec3 m_front{0.0f, 0.0f, -1.0f};
    glm::vec3 m_up{0.0f, 1.0f, 0.0f};
    glm::vec3 m_right{1.0f, 0.0f, 0.0f};
    const glm::vec3 m_worldUp{0.0f, 1.0f, 0.0f};

    float m_yaw;
    float m_pitch;
    float m_fovDegrees = 60.0f;
    float m_near       = 0.1f;
    float m_far        = 600.0f;
};

} // namespace fitzel
