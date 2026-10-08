#include "fitzel/core/Input.hpp"

#include "fitzel/core/Window.hpp"

#include "fitzel/asset/Vfs.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include <GLFW/glfw3.h>

namespace fitzel {

namespace {

// GLFW's built-in mapping table knows the common pads only. The SDL community
// database (assets/gamecontrollerdb.txt, see cmake/Dependencies.cmake) knows
// some two thousand more; SDL_GAMECONTROLLERCONFIG adds a player's own lines
// the way SDL games take them. Later lines replace earlier ones for one GUID,
// so the player's win. Once per process, before the first pad is looked at --
// not in the constructor, which can run before an exported game's archive is
// mounted.
void loadGamepadMappings() {
    static bool done = false;
    if (done) return;
    done = true;
#ifndef __EMSCRIPTEN__   // the browser maps pads itself (Gamepad API "standard")
    const std::string db = vfs::readText("assets/gamecontrollerdb.txt");
    if (!db.empty() && !glfwUpdateGamepadMappings(db.c_str()))
        std::fprintf(stderr, "[Fitzel] gamecontrollerdb.txt: not every mapping was taken\n");
    if (const char* own = std::getenv("SDL_GAMECONTROLLERCONFIG"))
        glfwUpdateGamepadMappings(own);
#endif
}

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
// A wired Microsoft pad that no table maps. On Linux a GUID carries the pad's
// firmware version, so an Xbox One/Series/Elite pad on a newer firmware than
// the database's is a stranger to it -- while the kernel's xpad driver gives
// every one of them the same layout. Recognised by bus (USB, 0x0003) and vendor
// (0x045e) in the GUID plus that layout's shape, then mapped like its siblings.
void mapUnknownXboxPad(int jid) {
    const char* guid = glfwGetJoystickGUID(jid);
    if (!guid) return;
    const std::string g(guid);
    if (g.size() != 32 || g.compare(0, 4, "0300") != 0 || g.compare(8, 4, "5e04") != 0)
        return;
    int axes = 0, buttons = 0, hats = 0;
    glfwGetJoystickAxes(jid, &axes);
    glfwGetJoystickButtons(jid, &buttons);
    glfwGetJoystickHats(jid, &hats);
    if (axes != 6 || buttons < 11 || hats < 1) return;   // not xpad's layout
    const std::string mapping =
        g + ",Xbox controller (xpad)," +
        "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,"
        "guide:b8,leftstick:b9,rightstick:b10,leftx:a0,lefty:a1,lefttrigger:a2,"
        "rightx:a3,righty:a4,righttrigger:a5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,"
        "dpleft:h0.8,platform:Linux,";
    if (glfwUpdateGamepadMappings(mapping.c_str()))
        std::fprintf(stderr, "[Fitzel] gamepad: %s had no mapping, using the xpad layout\n",
                     glfwGetJoystickName(jid));
}
#endif

// The slot to read: the one read last frame while it is still a gamepad (no
// jumping between two pads), else the first that is. A joystick GLFW cannot
// map is looked at once per connection -- the Linux Xbox fallback above -- and
// otherwise left alone; something else on slot 1 (a wheel, a laptop's
// accelerometer) no longer hides the pad on slot 2.
int findGamepad(int current) {
    if (current >= 0 && glfwJoystickIsGamepad(current)) return current;
    static std::set<std::string> tried;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        if (!glfwJoystickPresent(jid)) continue;
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
        if (!glfwJoystickIsGamepad(jid)) {
            const char* guid = glfwGetJoystickGUID(jid);
            if (guid && tried.insert(guid).second) mapUnknownXboxPad(jid);
        }
#endif
        if (glfwJoystickIsGamepad(jid)) {
            std::fprintf(stderr, "[Fitzel] gamepad: %s (slot %d)\n",
                         glfwGetGamepadName(jid), jid + 1);
            return jid;
        }
    }
    (void)tried;
    return -1;
}

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

    // Gamepad: snapshot the first controller that maps to a gamepad (see
    // findGamepad). Copy into plain arrays so the header stays GLFW-free.
    loadGamepadMappings();
    m_padSlot = findGamepad(m_padSlot);
    GLFWgamepadstate gp;
    m_padPresent = m_padSlot >= 0 && glfwGetGamepadState(m_padSlot, &gp);
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
