#include "fitzel/core/Window.hpp"

#include "fitzel/core/GlCaps.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

namespace fitzel {

namespace {

// GLFW is a process-global C library; reference-count init/terminate so that
// multiple Windows (or repeated create/destroy cycles) behave correctly.
int g_glfwWindowCount = 0;

void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "[GLFW] error %d: %s\n", code, description);
}

void ensureGlfwInitialized() {
    if (g_glfwWindowCount == 0) {
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
GLFWmonitor* monitorForWindow(GLFWwindow* w) {
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

    hint(4, 3);
    m_handle = glfwCreateWindow(config.width, config.height,
                                config.title.c_str(), nullptr, nullptr);
    if (!m_handle) {
        hint(3, 3);
        m_handle = glfwCreateWindow(config.width, config.height,
                                    config.title.c_str(), nullptr, nullptr);
    }
    if (!m_handle) {
        if (g_glfwWindowCount == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("Failed to create GLFW window");
    }
    ++g_glfwWindowCount;

    glfwMakeContextCurrent(m_handle);

    if (gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)) == 0) {
        glfwDestroyWindow(m_handle);
        m_handle = nullptr;
        if (--g_glfwWindowCount == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("Failed to load OpenGL via GLAD");
    }

    glfwSwapInterval(config.vsync ? 1 : 0);

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
