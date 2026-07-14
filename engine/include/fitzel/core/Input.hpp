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

    // --- Gamepad (first connected controller, e.g. an Xbox pad) --------------
    // State is refreshed in update(). Indices are GLFW_GAMEPAD_AXIS_* /
    // GLFW_GAMEPAD_BUTTON_* (the header stays GLFW-free; callers pass those).
    bool  hasGamepad() const { return m_padPresent; }
    // Raw axis in -1..1 (0 when no pad or out of range). Sticks rest near 0;
    // triggers rest at -1.
    float gamepadAxis(int axis) const;
    bool  gamepadButton(int button) const;
    // Stick axis with a radial dead-zone applied and rescaled to 0..1 outside it
    // (so there is no drift at rest and full range still reaches 1).
    float gamepadStick(int axis, float deadzone = 0.18f) const;
    // Trigger axis remapped from its -1..1 (rest -1) range to 0..1 (0 = no pad).
    float gamepadTrigger(int axis) const;

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

    // Gamepad snapshot (copied out of GLFW each update so the header needs no
    // GLFW types). 6 axes / 15 buttons match GLFW_GAMEPAD_AXIS/BUTTON_LAST.
    bool          m_padPresent = false;
    float         m_padAxes[6]    = {0.0f};
    unsigned char m_padButtons[15] = {0};
};

} // namespace fitzel
