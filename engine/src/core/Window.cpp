#include "fitzel/core/Window.hpp"

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

} // namespace

Window::Window(const WindowConfig& config) {
    ensureGlfwInitialized();

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // required on macOS
    glfwWindowHint(GLFW_MAXIMIZED, config.maximized ? GLFW_TRUE : GLFW_FALSE);

    m_handle = glfwCreateWindow(config.width, config.height,
                                config.title.c_str(), nullptr, nullptr);
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

    std::printf("[Fitzel] OpenGL %s | %s\n",
                glGetString(GL_VERSION), glGetString(GL_RENDERER));
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

void Window::swapBuffers() {
    glfwSwapBuffers(m_handle);
}

void Window::pollEvents() {
    glfwPollEvents();
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
