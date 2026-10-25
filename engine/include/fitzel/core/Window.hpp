#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "fitzel/rhi/Types.hpp"

struct GLFWwindow;

namespace fitzel {

namespace rhi { class Device; }

struct WindowConfig {
    int         width     = 1280;
    int         height    = 720;
    std::string title     = "Fitzel";
    bool        vsync     = true;
    bool        maximized = false; // start maximized to the screen
    // False: never shown -- an off-screen context for a test or a tool that
    // renders into its own framebuffers.
    bool        visible   = true;
    // The graphics API the window's device speaks.
    rhi::Backend backend  = rhi::Backend::OpenGL;
    // Who the window is, for the desktop. On Linux a program's icon is not in
    // its binary: the desktop finds a .desktop file by this name (the Wayland
    // app_id, the X11 WM_CLASS) and takes the icon -- and the dock entry -- from
    // there. Empty: unset, and the window gets a generic icon.
    std::string appId;
    // A PNG for the window itself where the platform takes one from the
    // program (X11's title bar and task switcher). Wayland ignores it -- there
    // the .desktop file above is the only way -- and so does Windows, where the
    // exe's own icon resource is used. Empty or unreadable: none.
    std::string iconPng;
};

// Owns a GLFW window plus the rhi::Device that draws into it (on OpenGL: its
// context, 4.3 core where the driver has it and 3.3 core where not).
// Construction creates both and makes the device current; destruction tears
// everything down. Move-only: a Window uniquely owns its native handle.
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

    // Show the frame (the device presents). Call once per frame.
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

    rhi::Device& device() const { return *m_device; }
    // Make this window's device the one rhi::device() answers (for a program
    // with more than one window; the last one made is current already).
    void makeCurrent();

private:
    void destroy();

    GLFWwindow* m_handle = nullptr;
    std::unique_ptr<rhi::Device> m_device;
    // The windowed rect, remembered while fullscreen so leaving it lands back
    // where it started.
    bool m_fullscreen = false;
    int  m_savedX = 0, m_savedY = 0, m_savedW = 0, m_savedH = 0;
};

} // namespace fitzel
