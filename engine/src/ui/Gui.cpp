#include "fitzel/ui/Gui.hpp"

#include "fitzel/core/Window.hpp"

#include <utility>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

namespace fitzel {

Gui::Gui(Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // install_callbacks = true: ImGui chains the callbacks Input already set.
    ImGui_ImplGlfw_InitForOpenGL(window.nativeHandle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");

    m_active = true;
}

Gui::~Gui() {
    if (m_active) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
}

Gui::Gui(Gui&& other) noexcept : m_active(std::exchange(other.m_active, false)) {}

Gui& Gui::operator=(Gui&& other) noexcept {
    if (this != &other) {
        if (m_active) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
        m_active = std::exchange(other.m_active, false);
    }
    return *this;
}

void Gui::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Gui::endFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool Gui::wantsMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool Gui::wantsKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace fitzel
