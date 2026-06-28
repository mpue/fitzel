#pragma once

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace fitzel {

struct WindowConfig {
    int         width  = 1280;
    int         height = 720;
    std::string title  = "Fitzel";
    bool        vsync  = true;
};

// Owns a GLFW window plus its OpenGL 3.3 core context. Construction creates the
// window and loads OpenGL function pointers; destruction tears everything down.
// Move-only: a Window uniquely owns its native handle.
class Window {
public:
    explicit Window(const WindowConfig& config = {});
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    // Returns false once the user requested to close the window.
    bool isOpen() const;
    void requestClose();

    // Swap buffers and poll input events. Call once per frame.
    void swapBuffers();
    void pollEvents();

    // Framebuffer size in pixels (may differ from window size on HiDPI).
    void framebufferSize(int& width, int& height) const;
    float aspectRatio() const;

    // Seconds elapsed since the windowing system was initialized.
    double time() const;

    GLFWwindow* nativeHandle() const { return m_handle; }

private:
    GLFWwindow* m_handle = nullptr;
};

} // namespace fitzel
