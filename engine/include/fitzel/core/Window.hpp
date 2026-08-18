#pragma once

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace fitzel {

struct WindowConfig {
    int         width     = 1280;
    int         height    = 720;
    std::string title     = "Fitzel";
    bool        vsync     = true;
    bool        maximized = false; // start maximized to the screen
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
    // Block until an event arrives or `seconds` elapse, then process events.
    // Like pollEvents() but sleeps instead of spinning -- used to cap the idle
    // editor to a low frame rate without busy-waiting (wakes instantly on input).
    void waitEventsTimeout(double seconds);

    // Framebuffer size in pixels (may differ from window size on HiDPI).
    void framebufferSize(int& width, int& height) const;
    float aspectRatio() const;

    // Seconds elapsed since the windowing system was initialized.
    double time() const;

    // --- Fullscreen --------------------------------------------------------
    // BORDERLESS fullscreen: an undecorated window the exact size of the monitor
    // it is on, never a display-mode change.
    //
    // The distinction is not cosmetic. Exclusive fullscreen (GLFW's real
    // fullscreen, a monitor handed to glfwSetWindowMonitor) takes the display
    // away from the desktop compositor, and everything that reads the screen
    // through the compositor stops seeing the game: Windows' own screenshots,
    // Game Bar, and any capture tool using Windows Graphics Capture all come
    // back black. It also means any overlay that takes focus MINIMISES the
    // window, which is where a 0x0 framebuffer comes from. Borderless costs a
    // frame of latency against all of that and is what shipped games use.
    //
    // The monitor is the one the window is actually on, not the primary: on a
    // two-screen desk, going fullscreen must not jump to the other one.
    void setFullscreen(bool on);
    bool fullscreen() const { return m_fullscreen; }

    GLFWwindow* nativeHandle() const { return m_handle; }

private:
    GLFWwindow* m_handle = nullptr;
    // The windowed rect, remembered while fullscreen so leaving it lands back
    // where it started.
    bool m_fullscreen = false;
    int  m_savedX = 0, m_savedY = 0, m_savedW = 0, m_savedH = 0;
};

} // namespace fitzel
