#include "fitzel/core/Window.hpp"

#include "fitzel/rhi/Device.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <GLFW/glfw3.h>
#include <stb_image.h>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

#ifdef __EMSCRIPTEN__
#include <GLFW/emscripten_glfw3.h>
#include <emscripten/html5.h>
#endif

namespace fitzel {

namespace {

// GLFW is a process-global C library; reference-count init/terminate so that
// multiple Windows (or repeated create/destroy cycles) behave correctly.
int g_glfwWindowCount = 0;

void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "[GLFW] error %d: %s\n", code, description);
}

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
// Linux has no NvOptimusEnablement (main.cpp): on a hybrid laptop the context
// lands on the iGPU unless the process asks for PRIME render offload -- the
// same variables prime-run sets. Asked for here, before glfwInit, because
// libglvnd and the NVIDIA driver read them when the first display opens. Only
// with the NVIDIA driver loaded, and never over anything the user set:
// FITZEL_GPU=integrated keeps the iGPU, DRI_PRIME picks a Mesa GPU by hand.
void preferDiscreteGpu() {
    const char* want = std::getenv("FITZEL_GPU");
    if (want && std::string_view(want) == "integrated") return;
    if (std::getenv("DRI_PRIME") || std::getenv("__NV_PRIME_RENDER_OFFLOAD") ||
        std::getenv("__GLX_VENDOR_LIBRARY_NAME"))
        return;
    if (access("/proc/driver/nvidia/version", F_OK) != 0) return;
    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
}
#endif

void ensureGlfwInitialized() {
    if (g_glfwWindowCount == 0) {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
        preferDiscreteGpu();
#endif
        glfwSetErrorCallback(glfwErrorCallback);
        if (!glfwInit()) {
            throw std::runtime_error("Failed to initialize GLFW");
        }
    }
}

// The monitor a window is mostly on: the one whose area contains its centre.
// GLFW has no such call -- glfwGetPrimaryMonitor is not the same question, and
// answering it with the primary is how a game on the second screen jumps to the
// first the moment it goes fullscreen.
//
// Wayland has no window position to ask for (asking is an error GLFW prints)
// and no primary monitor either: GLFW's "primary" there is merely the first
// output the compositor announced -- on a laptop the built-in panel, whichever
// screen the desktop calls primary. What Wayland does say is which outputs the
// window's surface has entered, and our GLFW is patched to hand that out
// (cmake/patches/glfw-wayland-window-monitor.patch). Before the window has
// been shown it has entered none, and only then is the first output the answer.
GLFWmonitor* monitorForWindow(GLFWwindow* w) {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        if (GLFWmonitor* on = glfwGetWaylandWindowOutputMonitor(w)) return on;
        return glfwGetPrimaryMonitor();
    }
#endif
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(w, &wx, &wy);
    glfwGetWindowSize(w, &ww, &wh);
    const int cx = wx + ww / 2, cy = wy + wh / 2;

    int count = 0;
    GLFWmonitor** mons = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx = 0, my = 0;
        glfwGetMonitorPos(mons[i], &mx, &my);
        const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
        if (!vm) continue;
        if (cx >= mx && cx < mx + vm->width && cy >= my && cy < my + vm->height)
            return mons[i];
    }
    return glfwGetPrimaryMonitor();   // off every screen (or none): the main one
}

} // namespace

Window::Window(const WindowConfig& config) {
    ensureGlfwInitialized();

    // The backend says what to ask GLFW for, and what to ask for next if the
    // driver will not give it (OpenGL: 4.3 core, then 3.3 core).
    auto hint = [&config](int attempt) {
        if (!rhi::prepareWindowHints(config.backend, attempt)) return false;
        glfwWindowHint(GLFW_MAXIMIZED, config.maximized ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
        return true;
    };

#ifdef __EMSCRIPTEN__
    // The browser: a context on the page's canvas, sized to what the page laid
    // it out at (web/index.html makes it fill the window).
    hint(0);
    double cssW = 0.0, cssH = 0.0;
    emscripten_get_element_css_size("#canvas", &cssW, &cssH);
    const int w = cssW > 0.0 ? static_cast<int>(cssW) : config.width;
    const int h = cssH > 0.0 ? static_cast<int>(cssH) : config.height;
    m_handle = glfwCreateWindow(w, h, config.title.c_str(), nullptr, nullptr);
#else
    if (!config.appId.empty()) {
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, config.appId.c_str());
        glfwWindowHintString(GLFW_X11_CLASS_NAME, config.appId.c_str());
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, config.appId.c_str());
    }
    for (int attempt = 0; !m_handle && hint(attempt); ++attempt)
        m_handle = glfwCreateWindow(config.width, config.height,
                                    config.title.c_str(), nullptr, nullptr);
#endif
    if (!m_handle) {
        if (g_glfwWindowCount == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("Failed to create GLFW window");
    }
    ++g_glfwWindowCount;

    try {
        m_device = rhi::createDevice(config.backend, m_handle, config.vsync);
    } catch (...) {
        glfwDestroyWindow(m_handle);
        m_handle = nullptr;
        if (--g_glfwWindowCount == 0) {
            glfwTerminate();
        }
        throw;
    }

#ifdef __EMSCRIPTEN__
    // The canvas fills the browser window and follows it when that is resized.
    emscripten_glfw_make_canvas_resizable(m_handle, "window", nullptr);
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    // X11 only: Wayland reports an error for it (the icon comes from the
    // .desktop file there), and on Windows the exe carries its own.
    if (!config.iconPng.empty() && glfwGetPlatform() == GLFW_PLATFORM_X11) {
        int w = 0, h = 0, n = 0;
        if (unsigned char* px = stbi_load(config.iconPng.c_str(), &w, &h, &n, 4)) {
            const GLFWimage img{w, h, px};
            glfwSetWindowIcon(m_handle, 1, &img);
            stbi_image_free(px);
        }
    }
#endif

    glfwSetFramebufferSizeCallback(m_handle, [](GLFWwindow*, int w, int h) {
        if (rhi::hasDevice()) rhi::device().setViewport(0, 0, w, h);
    });

    // The compute line is not noise: it is the one difference between two
    // machines that both say "OpenGL fine" and only one of which can run the
    // GPU tracer, and it belongs in the log the day a bug report arrives.
    const rhi::Caps& caps = m_device->caps();
    std::printf("[Fitzel] %s | %s | compute %s\n", caps.apiName, caps.renderer,
                caps.compute ? "yes" : "no");
}

void Window::destroy() {
    if (m_device) {
        if (rhi::hasDevice() && &rhi::device() == m_device.get()) rhi::makeCurrent(nullptr);
        m_device.reset();
    }
    if (m_handle) {
        glfwDestroyWindow(m_handle);
        m_handle = nullptr;
        if (--g_glfwWindowCount == 0) {
            glfwTerminate();
        }
    }
}

Window::~Window() { destroy(); }

Window::Window(Window&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)),
      m_device(std::move(other.m_device)),
      m_fullscreen(other.m_fullscreen),
      m_savedX(other.m_savedX), m_savedY(other.m_savedY),
      m_savedW(other.m_savedW), m_savedH(other.m_savedH) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        destroy();
        m_handle = std::exchange(other.m_handle, nullptr);
        m_device = std::move(other.m_device);
        m_fullscreen = other.m_fullscreen;
        m_savedX = other.m_savedX; m_savedY = other.m_savedY;
        m_savedW = other.m_savedW; m_savedH = other.m_savedH;
    }
    return *this;
}

void Window::makeCurrent() {
    if (m_device) rhi::makeCurrent(m_device.get());
}

bool Window::isOpen() const {
    return m_handle && !glfwWindowShouldClose(m_handle);
}

void Window::requestClose() {
    if (m_handle) {
        glfwSetWindowShouldClose(m_handle, GLFW_TRUE);
    }
}

void Window::setFullscreen(bool on) {
    if (!m_handle || on == m_fullscreen) return;
#ifdef __EMSCRIPTEN__
    // A page may only go fullscreen in answer to a click or a key. The start
    // click on web/index.html already asked for it when the game wants it;
    // later requests (a graphics menu) are answered on the next input event.
    EmscriptenFullscreenChangeEvent fs{};
    emscripten_get_fullscreen_status(&fs);
    if (on && !fs.isFullscreen)
        emscripten_glfw_request_fullscreen(m_handle, false, true);
    else if (!on && fs.isFullscreen)
        emscripten_exit_fullscreen();
    m_fullscreen = on;
    return;
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    // Wayland: the compositor's own fullscreen. Borderless-by-hand is wrong
    // here twice over. A Wayland client sizes its window in LOGICAL units while
    // the video mode is in pixels, so at 133% desktop scaling the "screen-sized"
    // window came out 133% larger than the screen, the game cut off at the
    // right and bottom. And there is nothing exclusive to avoid: a Wayland
    // client cannot change the display mode, so handing GLFW the monitor is
    // just xdg_toplevel.set_fullscreen -- still composited, still capturable,
    // sized by the compositor, decorations and screen blanking handled by GLFW.
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        if (on) {
            glfwGetWindowSize(m_handle, &m_savedW, &m_savedH);
            GLFWmonitor* mon = monitorForWindow(m_handle);
            const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
            if (!vm) return;
            glfwSetWindowMonitor(m_handle, mon, 0, 0, vm->width, vm->height,
                                 GLFW_DONT_CARE);
        } else {
            glfwSetWindowMonitor(m_handle, nullptr, 0, 0, std::max(m_savedW, 320),
                                 std::max(m_savedH, 240), 0);
        }
        m_fullscreen = on;
        return;
    }
#endif
    if (on) {
        glfwGetWindowPos(m_handle, &m_savedX, &m_savedY);
        glfwGetWindowSize(m_handle, &m_savedW, &m_savedH);
        GLFWmonitor* mon = monitorForWindow(m_handle);
        const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
        if (!vm) return;                       // no video mode: stay windowed
        int mx = 0, my = 0;
        glfwGetMonitorPos(mon, &mx, &my);
        glfwSetWindowAttrib(m_handle, GLFW_DECORATED, GLFW_FALSE);
        // The null monitor is the whole point: this stays a WINDOW, sized and
        // placed over the screen, so the desktop compositor keeps seeing it.
        glfwSetWindowMonitor(m_handle, nullptr, mx, my, vm->width, vm->height, 0);
    } else {
        glfwSetWindowMonitor(m_handle, nullptr, m_savedX, m_savedY,
                             std::max(m_savedW, 320), std::max(m_savedH, 240), 0);
        glfwSetWindowAttrib(m_handle, GLFW_DECORATED, GLFW_TRUE);
    }
    m_fullscreen = on;
}

void Window::swapBuffers() {
    m_device->present();
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::waitEventsTimeout(double seconds) {
    glfwWaitEventsTimeout(seconds);
}

void Window::framebufferSize(int& width, int& height) const {
    glfwGetFramebufferSize(m_handle, &width, &height);
}

float Window::aspectRatio() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_handle, &w, &h);
    return (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
}

double Window::time() const {
    return glfwGetTime();
}

} // namespace fitzel
