// treepanelcheck -- draws the Tree generator window and writes a picture of it.
//
// mixercheck's trick applied to the tree generator: it calls the editor's own
// treeui::TreeGenTool through the editor's own Gui and theme, so what comes out
// is the panel -- its studio, its tiles, its steppers -- and not a drawing of
// one. Run from the directory the shaders were copied to (build/release/bin).
//
//   treepanelcheck [out.png] [--size WxH] [--scroll px] [--preset n] [--frames n]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/TreeGenPanel.hpp"
#include "../src/UiStyle.hpp"

int main(int argc, char** argv) {
    std::string out = "treepanel.png";
    int w = 520, h = 1100, frames = 8;
    float scroll = 0.0f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (a == "--scroll" && i + 1 < argc) scroll = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--frames" && i + 1 < argc) frames = std::max(3, std::atoi(argv[++i]));
        else if (!a.empty() && a[0] != '-') out = a;
    }
    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "treepanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;

    const std::string project;   // none open: the save row shows its hint
    std::string status;
    treeui::TreeGenTool tool({project, status, nullptr});
    fitzel::rhi::Device& gpu = fitzel::rhi::device();

    for (int frame = 0; frame < frames; ++frame) {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        gpu.setViewport(0, 0, fw, fh);
        gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
        gpu.clear(fitzel::rhi::ClearColor);
        gui.beginFrame();
        bool show = true;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), static_cast<float>(h)), ImGuiCond_Always);
        if (scroll > 0.0f && frame > 1) ImGui::SetNextWindowScroll(ImVec2(0.0f, scroll));
        tool.panel(show);
        gui.endFrame();
        if (frame + 1 < frames) window.swapBuffers();
    }
    int fw = 0, fh = 0;
    window.framebufferSize(fw, fh);
    std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
    // The window's back buffer: the last frame was drawn but never shown.
    gpu.readPixels({}, 0, fitzel::rhi::Rect{0, 0, fw, fh}, fitzel::rhi::Format::RGBA8, px.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(out.c_str(), fw, fh, 4, px.data(), fw * 4) != 0;
    std::printf(wrote ? "[treepanelcheck] wrote %s (%dx%d)\n" : "[treepanelcheck] could not write %s\n",
                out.c_str(), fw, fh);
    return wrote ? 0 : 1;
}