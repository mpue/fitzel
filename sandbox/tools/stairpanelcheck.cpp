// stairpanelcheck -- draws the Stairs window and writes pictures of it.
//
// imagepanelcheck's trick applied to the stair generator: the editor's own
// stairui::drawPanel drawn through the editor's own Gui and theme, with a
// preset applied, scrolled from top to bottom so the whole panel is seen --
// the plan, the side view, the figures and every section.
//
//   stairpanelcheck [out.png] [--preset n] [--size WxH] [--pages n]
//
// Writes out.png, or out_1.png, out_2.png ... with --pages above 1.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/StairPanel.hpp"
#include "../src/UiStyle.hpp"

int main(int argc, char** argv) {
    std::string out = "stairpanel.png";
    int w = 520, h = 1100, preset = -1, pages = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (a == "--preset" && i + 1 < argc) preset = std::atoi(argv[++i]);
        else if (a == "--pages" && i + 1 < argc) pages = std::max(1, std::atoi(argv[++i]));
        else if (!a.empty() && a[0] != '-') out = a;
    }
    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "stairpanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    fitzel::rhi::Device& gpu = window.device();
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;

    stairgen::Params cfg;
    if (preset >= 0) stairgen::applyPreset(cfg, static_cast<stairgen::Preset>(preset));
    bool show = true, autoRebuild = true, pending = false;
    char name[64] = "Stair";
    const std::string status = "Generated stair.";
    int generated = 0;

    int fail = 0;
    float scrollMax = 0.0f;
    for (int page = 0; page < pages; ++page) {
        for (int f = 0; f < 6; ++f) {
            window.pollEvents();
            int fw = 0, fh = 0;
            window.framebufferSize(fw, fh);
            gpu.setViewport(0, 0, fw, fh);
            gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
            gpu.clear(fitzel::rhi::ClearColor);
            gui.beginFrame();
            // The panel sets its own first size; take the whole window instead.
            ImGui::SetWindowPos("Stairs", ImVec2(0.0f, 0.0f));
            ImGui::SetWindowSize("Stairs", ImVec2(static_cast<float>(fw), static_cast<float>(fh)));
            if (pages > 1)
                ImGui::SetNextWindowScroll(ImVec2(0.0f, scrollMax * static_cast<float>(page) /
                                                            static_cast<float>(pages - 1)));
            stairui::drawPanel({show, cfg, true, generated > 0, false, name, sizeof name,
                                autoRebuild, pending, [&] { ++generated; }, [] {}, [] {}, [] {},
                                status});
            // How far the panel scrolls, read back once it has been laid out
            // (appending to the window draws nothing).
            ImGui::Begin("Stairs");
            if (page == 0) scrollMax = ImGui::GetScrollMaxY();
            ImGui::End();
            gui.endFrame();
            window.swapBuffers();
        }
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        std::vector<unsigned char> pixels(static_cast<std::size_t>(fw) * fh * 4);
        gpu.readPixels({}, 0, fitzel::rhi::Rect{0, 0, fw, fh}, fitzel::rhi::Format::RGBA8, pixels.data());
        stbi_flip_vertically_on_write(1);
        std::string path = out;
        if (pages > 1) {
            const auto dot = out.rfind('.');
            path = out.substr(0, dot) + "_" + std::to_string(page + 1) + out.substr(dot);
        }
        const bool wrote = stbi_write_png(path.c_str(), fw, fh, 4, pixels.data(), fw * 4) != 0;
        std::printf("%s %s\n", wrote ? "wrote" : "FAIL writing", path.c_str());
        fail += wrote ? 0 : 1;
    }
    if (!show) { std::printf("FAIL the window closed itself\n"); ++fail; }
    return fail ? 1 : 0;
}
