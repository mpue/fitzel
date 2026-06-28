#pragma once

#include <glm/glm.hpp>

namespace fitzel {

class Window;

// Per-frame input polling for a Window: keyboard state, mouse position and
// frame-to-frame mouse delta. Call update() once per frame before querying.
class Input {
public:
    explicit Input(Window& window);

    // Refresh cached state. Call once at the top of each frame.
    void update();

    bool isKeyDown(int key) const;          // GLFW_KEY_*
    bool isMouseButtonDown(int button) const; // GLFW_MOUSE_BUTTON_*

    glm::vec2 mousePosition() const { return m_mousePos; }
    glm::vec2 mouseDelta() const { return m_mouseDelta; }
    float scrollDelta() const { return m_scrollDelta; }

    // Lock & hide the cursor for FPS-style mouse look (or release it).
    void setCursorLocked(bool locked);
    bool isCursorLocked() const { return m_cursorLocked; }

    // Called by the GLFW scroll callback; not for application use.
    void addScroll(float amount);

private:
    Window* m_window = nullptr;

    glm::vec2 m_mousePos{0.0f};
    glm::vec2 m_mouseDelta{0.0f};
    float     m_scrollDelta   = 0.0f;
    float     m_pendingScroll = 0.0f;
    bool      m_firstMouse    = true;
    bool      m_cursorLocked  = false;
};

} // namespace fitzel
