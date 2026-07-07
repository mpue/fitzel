#include "fitzel/ui/Gui.hpp"

#include "fitzel/core/Window.hpp"

#include <fstream>
#include <utility>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

namespace fitzel {

namespace {

// A soft, modern dark theme: calmer blue-grey backgrounds, a single azure
// accent, rounded corners and roomier spacing for readability.
void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 5.0f;
    s.PopupRounding     = 5.0f;
    s.GrabRounding      = 5.0f;
    s.TabRounding       = 5.0f;
    s.ScrollbarRounding = 5.0f;
    s.WindowPadding     = ImVec2(12.0f, 10.0f);
    s.FramePadding      = ImVec2(9.0f, 5.0f);
    s.ItemSpacing       = ImVec2(9.0f, 8.0f);
    s.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    s.ScrollbarSize     = 14.0f;
    s.GrabMinSize       = 11.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowTitleAlign  = ImVec2(0.02f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None; // drop the collapse-arrow clutter

    const ImVec4 accent    = ImVec4(0.28f, 0.56f, 0.86f, 1.00f);
    const ImVec4 accentDim = ImVec4(0.28f, 0.56f, 0.86f, 0.55f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.13f, 0.14f, 0.17f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.26f, 0.28f, 0.33f, 0.50f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.24f, 0.27f, 0.32f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.32f, 0.38f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.10f, 0.11f, 0.13f, 0.80f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.11f, 0.13f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.28f, 0.31f, 0.36f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.39f, 0.45f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = accent;
    c[ImGuiCol_CheckMark]            = accent;
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.38f, 0.66f, 0.96f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.28f, 0.42f, 0.60f, 1.00f);
    c[ImGuiCol_ButtonActive]         = accent;
    c[ImGuiCol_Header]               = ImVec4(0.22f, 0.34f, 0.49f, 0.80f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.26f, 0.42f, 0.62f, 0.90f);
    c[ImGuiCol_HeaderActive]         = accent;
    c[ImGuiCol_Separator]            = ImVec4(0.26f, 0.28f, 0.33f, 0.60f);
    c[ImGuiCol_SeparatorHovered]     = accentDim;
    c[ImGuiCol_SeparatorActive]      = accent;
    c[ImGuiCol_Tab]                  = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.26f, 0.42f, 0.62f, 1.00f);
    c[ImGuiCol_TabActive]            = ImVec4(0.20f, 0.30f, 0.44f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.12f, 0.13f, 0.16f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.16f, 0.20f, 0.26f, 1.00f);
    c[ImGuiCol_DockingPreview]       = accentDim;
    c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = accentDim;
}

// Load a crisp, larger UI font (Segoe UI on Windows, San Francisco on macOS).
// Falls back to scaling the built-in font up if no TTF is found, so text is
// never tiny. The first existing path wins, so per-platform paths can share one
// list -- the others simply don't exist on the current OS.
void loadFont(ImGuiIO& io) {
    const char* candidates[] = {
        // Windows
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\SegoeUI.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
        // macOS: San Francisco (system UI font), then Helvetica/Arial fallbacks
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    for (const char* path : candidates) {
        if (std::ifstream(path).good()) {
            io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg);
            return;
        }
    }
    io.FontGlobalScale = 1.35f; // no system font available: scale the default
}

} // namespace

Gui::Gui(Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // allow panels to dock

    loadFont(io);
    applyTheme();

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

unsigned int Gui::dockspace() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    // A borderless, transparent host that can't itself be docked or focused, so
    // it acts purely as a backdrop for the dockspace over the live scene.
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockId = ImGui::GetID("FitzelDock");
    // PassthruCentralNode keeps the empty centre transparent (scene visible).
    ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
    return dockId;
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
