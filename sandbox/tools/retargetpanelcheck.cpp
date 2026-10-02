// retargetpanelcheck -- draws the Retarget animations window and writes a picture of it.
//
// treepanelcheck's trick: the editor's own retargetui::RetargetTool through the
// editor's own Gui and theme, so what comes out is the window -- the studio with
// both figures, the clip list, the tabs -- not a drawing of one. It works on a
// copy of a character in the temp folder (with a recipe of two clips), so the
// project's own files are never touched. Run from the directory the shaders
// were copied to (build/release/bin).
//
//   retargetpanelcheck [out.png] [--size WxH] [--clip n] [--time s] [--tab n]
//                      [--character model.glb] [--audition motion.fbx]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/Retarget.hpp"
#include "../src/RetargetPanel.hpp"
#include "../src/UiStyle.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::string out = "retargetpanel.png";
    std::string character = "D:/fitzel_projects/treetest/models/vicky.glb.vor-claude-backup";
    std::string auditionFile;
    std::string openAsIs;   // a character opened where it is (nothing is changed unless told)
    std::string copySource; // the Characters tab: copy from this one...
    std::vector<std::string> picks;   // ...these animations (by their name in the file)
    bool emptyRecipe = false;
    int w = 1280, h = 900, clip = 0, tab = 0;
    float time = 1.0f, turn = 20.0f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (a == "--clip" && i + 1 < argc) clip = std::atoi(argv[++i]);
        else if (a == "--time" && i + 1 < argc) time = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--tab" && i + 1 < argc) tab = std::atoi(argv[++i]);
        else if (a == "--turn" && i + 1 < argc) turn = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--character" && i + 1 < argc) character = argv[++i];
        else if (a == "--audition" && i + 1 < argc) auditionFile = argv[++i];
        else if (a == "--open" && i + 1 < argc) openAsIs = argv[++i];
        else if (a == "--copy-from" && i + 1 < argc) copySource = argv[++i];
        else if (a == "--pick" && i + 1 < argc) {
            std::string list = argv[++i];
            for (std::size_t at = 0; at <= list.size();) {
                const std::size_t comma = list.find(',', at);
                const std::string one = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!one.empty()) picks.push_back(one);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
        else if (a == "--empty") emptyRecipe = true;
        else if (!a.empty() && a[0] != '-') out = a;
    }

    // A character of our own to work on, with the two clips Vicky has.
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "retargetpanelcheck";
    fs::create_directories(dir, ec);
    const std::string model = openAsIs.empty() ? (dir / "vicky.glb").generic_string() : openAsIs;
    if (openAsIs.empty() && !fs::exists(model, ec)) fs::copy_file(character, model, ec);
    if (openAsIs.empty()) {
        // A fresh recipe every run: the window saves what it is told.
        retarget::Recipe r;
        retarget::ClipEntry walk;
        walk.name = "walk_f";
        walk.source = "D:/models/Motion/walk_normal_f.fbx";
        retarget::ClipEntry dance;
        dance.name = "sensual_dance";
        dance.source = "D:/models/Motion/sensual-dance-02.fbx";
        if (!emptyRecipe) r.clips = {walk, dance};
        retarget::saveRecipe(model, r);
    }

    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "retargetpanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;

    const std::string project;
    std::string status;
    retargetui::RetargetTool tool({project, status, nullptr, nullptr, nullptr, {}});
    tool.setFirstSize(static_cast<float>(w), static_cast<float>(h));
    tool.openCharacter(model);

    const auto t0 = std::chrono::steady_clock::now();
    int settled = -1, frame = 0;
    bool auditioned = auditionFile.empty();
    int copyStep = copySource.empty() ? 2 : 0;   // 0 open it, 1 add the picks, 2 done
    for (;; ++frame) {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        gui.beginFrame();
        bool show = true;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), static_cast<float>(h)), ImGuiCond_Always);
        tool.panel(show);
        gui.endFrame();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (settled < 0 && frame > 3 && tool.idle()) {
            if (!auditioned) {
                tool.addMotion(auditionFile);
                auditioned = true;
            } else if (copyStep == 0) {
                tool.copyFrom(copySource);
                copyStep = picks.empty() ? 2 : 1;
            } else if (copyStep == 1) {
                tool.addTakes(copySource, picks);
                tool.copyFrom(copySource);   // keep the Characters tab in front
                copyStep = 2;
            } else {
                settled = frame;
                tool.showAt(clip, time, tab);
                tool.setTurn(turn);
            }
        }
        if ((settled >= 0 && frame >= settled + 6) || secs > 120.0) break;
        window.swapBuffers();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    int fw = 0, fh = 0;
    window.framebufferSize(fw, fh);
    std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(out.c_str(), fw, fh, 4, px.data(), fw * 4) != 0;
    std::printf(wrote ? "[retargetpanelcheck] wrote %s (%dx%d) after %d frames%s\n"
                      : "[retargetpanelcheck] could not write %s\n",
                out.c_str(), fw, fh, frame, settled < 0 ? " -- still loading!" : "");
    if (!status.empty()) std::printf("  status: %s\n", status.c_str());
    return wrote && settled >= 0 ? 0 : 1;
}