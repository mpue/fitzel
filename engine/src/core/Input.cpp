#include "fitzel/core/Input.hpp"

#include "fitzel/core/Window.hpp"

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
