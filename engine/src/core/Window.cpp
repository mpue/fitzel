#include "fitzel/core/Window.hpp"

#include "fitzel/core/GlCaps.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

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
// Wayland tells a client nothing about where its window is (asking is an error
// GLFW prints), so there it is the primary; the compositor still puts the
// fullscreen surface where the window was on a single-screen desk.
GLFWmonitor* monitorForWindow(GLFWwindow* w) {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) return glfwGetPrimaryMonitor();
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

    // 4.3 first, 3.3 if the driver will not give it. The engine draws in 3.3 and
    // every shader it ships is `#version 330 core`, which a 4.3 core context
    // runs unchanged -- so asking for more costs nothing and buys the two things
    // that cannot be written below it: compute shaders and shader storage
    // buffers. Nothing in the engine REQUIRES them; see fitzel::glcaps, which is
    // how a feature that wants them finds out whether it got them.
    auto hint = [&config](int major, int minor) {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // required on macOS
        glfwWindowHint(GLFW_MAXIMIZED, config.maximized ? GLFW_TRUE : GLFW_FALSE);
    };

#ifdef __EMSCRIPTEN__
    // The browser: a WebGL2 context on the page's canvas, sized to what the
    // page laid it out at (web/index.html makes it fill the window). Asked
    // for by its WebGL version, which is what the GLFW port reads the context
    // version as (3 would mean a WebGL 3 that does not exist).
    (void)hint;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    double cssW = 0.0, cssH = 0.0;
    emscripten_get_element_css_size("#canvas", &cssW, &cssH);
    const int w = cssW > 0.0 ? static_cast<int>(cssW) : config.width;
    const int h = cssH > 0.0 ? static_cast<int>(cssH) : config.height;
    m_handle = glfwCreateWindow(w, h, config.title.c_str(), nullptr, nullptr);
#else
    hint(4, 3);
    m_handle = glfwCreateWindow(config.width, config.height,
                                config.title.c_str(), nullptr, nullptr);
    if (!m_handle) {
        hint(3, 3);
        m_handle = glfwCreateWindow(config.width, config.height,
                                    config.title.c_str(), nullptr, nullptr);
    }
#endif
    if (!m_handle) {
        if (g_glfwWindowCount == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("Failed to create GLFW window");
    }
    ++g_glfwWindowCount;

    glfwMakeContextCurrent(m_handle);

#ifdef __EMSCRIPTEN__
    // No loader: the runtime provides the WebGL2 entry points. The canvas
    // fills the browser window and follows it when that is resized.
    emscripten_glfw_make_canvas_resizable(m_handle, "window", nullptr);
#else
    if (gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)) == 0) {
        glfwDestroyWindow(m_handle);
        m_handle = nullptr;
        if (--g_glfwWindowCount == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("Failed to load OpenGL via GLAD");
    }
#endif

#ifndef __EMSCRIPTEN__
    // (The browser presents on its own refresh; there is no interval to set.)
    glfwSwapInterval(config.vsync ? 1 : 0);
#endif

    int fbWidth = 0, fbHeight = 0;
    glfwGetFramebufferSize(m_handle, &fbWidth, &fbHeight);
    glViewport(0, 0, fbWidth, fbHeight);

    glfwSetFramebufferSizeCallback(m_handle, [](GLFWwindow*, int w, int h) {
        glViewport(0, 0, w, h);
    });

    // The compute line is not noise: it is the one difference between two
    // machines that both say "OpenGL fine" and only one of which can run the
    // GPU tracer, and it belongs in the log the day a bug report arrives.
    std::printf("[Fitzel] OpenGL %s | %s | compute %s\n",
                glGetString(GL_VERSION), glGetString(GL_RENDERER),
                glcaps::compute() ? "yes" : "no");
}

Window::~Window() {
    if (m_handle) {
        glfwDestroyWindow(m_handle);
        if (--g_glfwWindowCount == 0) {
            glfwTerminate();
        }
    }
}

Window::Window(Window&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (m_handle) {
            glfwDestroyWindow(m_handle);
            if (--g_glfwWindowCount == 0) {
                glfwTerminate();
            }
        }
        m_handle = std::exchange(other.m_handle, nullptr);
    }
    return *this;
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
    glfwSwapBuffers(m_handle);
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
