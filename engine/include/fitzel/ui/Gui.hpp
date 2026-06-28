#pragma once

namespace fitzel {

class Window;

// Owns the Dear ImGui context plus its GLFW/OpenGL3 backends. Construct *after*
// fitzel::Input so ImGui chains the existing input callbacks rather than
// replacing them. RAII: the context is torn down on destruction. Move-only.
//
// Per frame: call beginFrame(), submit your ImGui:: widgets, then endFrame()
// after your own rendering so the UI draws on top.
class Gui {
public:
    explicit Gui(Window& window);
    ~Gui();

    Gui(const Gui&)            = delete;
    Gui& operator=(const Gui&) = delete;
    Gui(Gui&& other) noexcept;
    Gui& operator=(Gui&& other) noexcept;

    void beginFrame();
    void endFrame();

    // True when ImGui is using the mouse/keyboard, so the app should ignore it
    // (e.g. don't move the camera while interacting with a panel).
    bool wantsMouse() const;
    bool wantsKeyboard() const;

private:
    bool m_active = false;
};

} // namespace fitzel
