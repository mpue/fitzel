#include "fitzel/core/Input.hpp"

#include "fitzel/core/Window.hpp"

#include <cmath>

#include <GLFW/glfw3.h>

namespace fitzel {

namespace {

// Scroll arrives via a GLFW callback; accumulate it onto the Input that owns
// the window (retrieved through the window user pointer set in the ctor).
void scrollCallback(GLFWwindow* handle, double /*xoffset*/, double yoffset) {
    if (auto* input = static_cast<Input*>(glfwGetWindowUserPointer(handle))) {
        input->addScroll(static_cast<float>(yoffset));
    }
}

} // namespace

Input::Input(Window& window) : m_window(&window) {
    GLFWwindow* handle = m_window->nativeHandle();
    glfwSetWindowUserPointer(handle, this);
    glfwSetScrollCallback(handle, scrollCallback);

    double x = 0.0, y = 0.0;
    glfwGetCursorPos(handle, &x, &y);
    m_mousePos = {static_cast<float>(x), static_cast<float>(y)};
}

void Input::update() {
    GLFWwindow* handle = m_window->nativeHandle();

    double x = 0.0, y = 0.0;
    glfwGetCursorPos(handle, &x, &y);
    const glm::vec2 pos{static_cast<float>(x), static_cast<float>(y)};

    if (m_firstMouse) {
        m_mousePos   = pos;
        m_firstMouse = false;
    }

    // Screen-space y grows downward; negate so "mouse up" looks up.
    m_mouseDelta = {pos.x - m_mousePos.x, m_mousePos.y - pos.y};
    m_mousePos   = pos;

    m_scrollDelta = m_pendingScroll;
    m_pendingScroll = 0.0f;

    // Gamepad: snapshot the first controller if it maps to a gamepad (Xbox pads
    // are covered by GLFW's built-in mapping DB). Copy into plain arrays so the
    // header stays GLFW-free.
    GLFWgamepadstate gp;
    m_padPresent = glfwJoystickIsGamepad(GLFW_JOYSTICK_1) &&
                   glfwGetGamepadState(GLFW_JOYSTICK_1, &gp);
    if (m_padPresent) {
        for (int i = 0; i < 6; ++i)  m_padAxes[i]    = gp.axes[i];
        for (int i = 0; i < 15; ++i) m_padButtons[i] = gp.buttons[i];
    } else {
        for (float& a : m_padAxes)          a = 0.0f;
        for (unsigned char& b : m_padButtons) b = 0;
    }
}

float Input::gamepadAxis(int axis) const {
    if (!m_padPresent || axis < 0 || axis >= 6) return 0.0f;
    return m_padAxes[axis];
}

bool Input::gamepadButton(int button) const {
    if (!m_padPresent || button < 0 || button >= 15) return false;
    return m_padButtons[button] == GLFW_PRESS;
}

float Input::gamepadStick(int axis, float deadzone) const {
    const float v  = gamepadAxis(axis);
    const float av = std::fabs(v);
    if (av <= deadzone) return 0.0f;
    // Rescale [deadzone,1] -> [0,1] so control is smooth just past the dead-zone.
    const float t = (av - deadzone) / (1.0f - deadzone);
    return (v < 0.0f ? -t : t);
}

float Input::gamepadTrigger(int axis) const {
    if (!m_padPresent) return 0.0f;      // rest value is -1, so guard explicitly
    return (gamepadAxis(axis) + 1.0f) * 0.5f;
}

bool Input::isKeyDown(int key) const {
    return glfwGetKey(m_window->nativeHandle(), key) == GLFW_PRESS;
}

bool Input::isMouseButtonDown(int button) const {
    return glfwGetMouseButton(m_window->nativeHandle(), button) == GLFW_PRESS;
}

void Input::setCursorLocked(bool locked) {
    m_cursorLocked = locked;
    glfwSetInputMode(m_window->nativeHandle(), GLFW_CURSOR,
                     locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    m_firstMouse = true; // avoid a jump on the next update
}

void Input::addScroll(float amount) {
    m_pendingScroll += amount;
}

} // namespace fitzel
