// holdposepanelcheck -- draws the Hold Pose Studio window and writes a picture of it.
//
// The editor's own holdposeui::HoldPoseTool through the editor's own Gui, so what
// comes out is the window. It works on copies (in the temp folder) of a set of
// arms, their recipe, their Lua table and the weapons the recipe names, so a
// project's own files are never touched. Run from the directory the shaders were
// copied to (build/<preset>/bin).
//
//   holdposepanelcheck [out.png] --project <dir> --arms <model, relative to it>
//                      [--pose name] [--view 0..6] [--xray] [--size WxH]
//                      [--nudge side,x,y,z]   move a hand (metres, weapon frame) first
//                      [--write]              write the pose, read it back, compare
//                      [--arms-move x,y,z,yaw]  move the arms' model (metres) and turn it (degrees)
//                      [--gizmo 0|1|2]        the hand's gizmo: none, move (default), turn
//                      [--drag dx,dy]         drag the gizmo from `dy` pixels above its middle
//                                             (on the up arrow) by (dx, dy) more pixels

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
#include <ImGuizmo.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/HoldPose.hpp"
#include "../src/HoldPosePanel.hpp"
#include "../src/UiStyle.hpp"

namespace fs = std::filesystem;

namespace {

// Copy `rel` from the project into the work folder (same relative path).
void copyIn(const fs::path& from, const fs::path& to, const std::string& rel) {
    std::error_code ec;
    fs::create_directories((to / rel).parent_path(), ec);
    fs::copy_file(from / rel, to / rel, fs::copy_options::overwrite_existing, ec);
    if (ec) std::printf("[holdposepanelcheck] cannot copy %s: %s\n", rel.c_str(), ec.message().c_str());
}

// The palms (view space) of a pose, solved as the window solves it.
bool palms(const std::string& arms, const std::string& pose, glm::vec3 out[2]) {
    holdpose::Recipe r;
    holdpose::loadRecipe(arms, r);
    holdpose::HoldsInfo info;
    std::string err;
    holdpose::readHoldsInfo(holdpose::resolve(arms, r.holds), info, err);
    holdpose::Placement p;
    if (!holdpose::readPlacement(holdpose::resolve(arms, r.holds), pose, p, err)) return false;
    const fitzel::ModelData md = fitzel::loadGltf(arms);
    holdpose::Poser poser;
    if (!poser.init(md)) return false;
    int clip = -1;
    for (std::size_t i = 0; i < md.animations.size(); ++i)
        if (md.animations[i].name == "hold_" + pose) clip = static_cast<int>(i);
    const std::vector<glm::mat4> G = poser.basePose(md, clip);
    glm::vec3 c, size;
    holdpose::bounds(md, c, size);
    const glm::mat4 A = holdpose::armsMatrix(holdpose::weaponFrame(r.poses[pose]), p, info, c);
    for (int s = 0; s < 2; ++s) {
        glm::vec3 pc, f;
        if (!poser.palm(G, s, pc, f)) return false;
        out[s] = glm::vec3(A * glm::vec4(pc, 1.0f));
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string out = "holdposepanel.png", project, armsRel, pose;
    int w = 1400, h = 860, view = 0;
    bool xray = false, doWrite = false, doNudge = false;
    int gizmo = 1;
    bool doDrag = false, doArms = false;
    glm::vec4 armsMove(0.0f);
    std::vector<glm::vec3> jointsBefore;
    glm::vec3 dragged(0.0f);   // what the drag moved the right wrist by (weapon frame)
    float dragDx = 0.0f, dragDy = 0.0f;
    int nudgeSide = 1;
    glm::vec3 nudge(0.0f);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (a == "--project" && i + 1 < argc) project = argv[++i];
        else if (a == "--arms" && i + 1 < argc) armsRel = argv[++i];
        else if (a == "--pose" && i + 1 < argc) pose = argv[++i];
        else if (a == "--view" && i + 1 < argc) view = std::atoi(argv[++i]);
        else if (a == "--nudge" && i + 1 < argc) {
            doNudge = std::sscanf(argv[++i], "%d,%f,%f,%f", &nudgeSide, &nudge.x, &nudge.y, &nudge.z) == 4;
        }
        else if (a == "--xray") xray = true;
        else if (a == "--arms-move" && i + 1 < argc)
            doArms = std::sscanf(argv[++i], "%f,%f,%f,%f", &armsMove.x, &armsMove.y, &armsMove.z, &armsMove.w) == 4;
        else if (a == "--gizmo" && i + 1 < argc) gizmo = std::atoi(argv[++i]);
        else if (a == "--drag" && i + 1 < argc) doDrag = std::sscanf(argv[++i], "%f,%f", &dragDx, &dragDy) == 2;
        else if (a == "--write") doWrite = true;
        else if (!a.empty() && a[0] != '-') out = a;
    }
    if (project.empty() || armsRel.empty()) {
        std::printf("usage: holdposepanelcheck [out.png] --project <dir> --arms <model> [--pose n] [--view v] [--xray] [--write]\n");
        return 2;
    }

    // The work folder: the arms, their recipe, the table and the weapons.
    std::error_code ec;
    const fs::path work = fs::temp_directory_path(ec) / "holdposepanelcheck";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const fs::path src(project);
    copyIn(src, work, armsRel);
    copyIn(src, work, armsRel + ".holdposes");
    const std::string arms = (work / armsRel).generic_string();
    holdpose::Recipe recipe;
    if (!holdpose::loadRecipe(arms, recipe)) {
        std::printf("[holdposepanelcheck] %s has no recipe\n", armsRel.c_str());
        return 1;
    }
    auto relToProject = [&](const std::string& rel) {
        return fs::relative(fs::path(holdpose::resolve((src / armsRel).generic_string(), rel)), src, ec).generic_string();
    };
    copyIn(src, work, relToProject(recipe.holds));
    for (const auto& [name, wpn] : recipe.poses) copyIn(src, work, relToProject(wpn.model));
    if (pose.empty() && !recipe.poses.empty()) pose = recipe.poses.begin()->first;

    glm::vec3 before[2] = {};
    const bool haveBefore = palms(arms, pose, before);

    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "holdposepanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;

    const std::string projectFile;
    std::string status;
    holdposeui::HoldPoseTool tool({projectFile, status, {}});
    tool.setFirstSize(static_cast<float>(w), static_cast<float>(h));
    tool.openArms(arms);

    const auto t0 = std::chrono::steady_clock::now();
    int settled = -1, frame = 0, step = 0;
    for (;; ++frame) {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        gui.beginFrame();
        ImGuizmo::BeginFrame();
        bool show = true;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), static_cast<float>(h)), ImGuiCond_Always);
        tool.panel(show);
        gui.endFrame();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (settled < 0 && frame > 3 && tool.idle()) {
            if (step == 0) {
                tool.selectPose(pose);
                for (int sd = 0; sd < 2; ++sd) {
                    const glm::vec3 hp = tool.handPos(sd) * 100.0f;
                    std::printf("  %s wrist in the weapon's frame: %.1f %.1f %.1f cm\n", sd ? "right" : "left",
                                static_cast<double>(hp.x), static_cast<double>(hp.y), static_cast<double>(hp.z));
                }
                tool.setView(view);
                tool.setXray(xray);
                tool.setGizmo(gizmo);
                if (doNudge) tool.nudge(nudgeSide, nudge);
                if (doArms) {
                    tool.moveArms(glm::vec3(armsMove));
                    tool.turnArmsBy(glm::vec3(0, 1, 0), armsMove.w);
                }
                step = doDrag ? 10 : (doWrite ? 1 : 2);
            } else if (step == 1) {
                jointsBefore = tool.viewJoints();
                std::string err;
                if (!tool.write(err)) {
                    std::printf("[holdposepanelcheck] write failed: %s\n", err.c_str());
                    return 1;
                }
                step = 2;
            } else if (step >= 10 && step < 40) {
                // The drag, as a hand would do it: onto the up arrow, press,
                // move there in steps, let go.
                static float gx = 0.0f, gy = 0.0f;
                static glm::vec3 from(0.0f);
                ImGuiIO& io = ImGui::GetIO();
                if (step == 10) {
                    if (!tool.gizmoScreen(gx, gy)) { std::printf("[holdposepanelcheck] no gizmo shown\n"); return 1; }
                    gy -= 45.0f;
                    from = tool.handPos(1);
                    io.AddMousePosEvent(gx, gy);
                } else if (step == 12) {
                    io.AddMouseButtonEvent(0, true);
                } else if (step >= 14 && step < 30) {
                    const float k = static_cast<float>(step - 13) / 16.0f;
                    io.AddMousePosEvent(gx + dragDx * k, gy + dragDy * k);
                } else if (step == 32) {
                    io.AddMouseButtonEvent(0, false);
                } else if (step == 35) {
                    const glm::vec3 to = tool.handPos(1);
                    dragged = to - from;
                    std::printf("  right wrist dragged by %.1f %.1f %.1f cm (weapon frame)\n",
                                static_cast<double>((to.x - from.x) * 100.0f), static_cast<double>((to.y - from.y) * 100.0f),
                                static_cast<double>((to.z - from.z) * 100.0f));
                }
                ++step;
                if (step == 40) step = doWrite ? 1 : 2;
            } else {
                settled = frame;
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
    std::printf(wrote ? "[holdposepanelcheck] wrote %s (%dx%d) after %d frames%s\n"
                      : "[holdposepanelcheck] could not write %s\n",
                out.c_str(), fw, fh, frame, settled < 0 ? " -- still loading!" : "");
    int bad = wrote && settled >= 0 ? 0 : 1;
    if (doWrite && !jointsBefore.empty()) {
        // Every joint as the window showed it before writing, against where the
        // written files put it once read back.
        const std::vector<glm::vec3> after = tool.viewJoints();
        float worst = 0.0f;
        std::size_t at = 0;
        for (std::size_t j = 0; j < after.size() && j < jointsBefore.size(); ++j) {
            const float e = glm::length(after[j] - jointsBefore[j]);
            if (e > worst) { worst = e; at = j; }
        }
        std::printf("  joints: worst off by %.2f mm (joint %zu) after writing and reading back\n",
                    static_cast<double>(worst * 1000.0f), at);
        if (worst > 0.002f) bad = 1;
    }
    if (doWrite) {
        // What was written, read back: each palm must be where the edit put it
        // (moved by the nudge for that hand, still for the other).
        glm::vec3 after[2] = {};
        if (!haveBefore || !palms(arms, pose, after)) {
            std::printf("[holdposepanelcheck] cannot read the pose back\n");
            return 1;
        }
        recipe = holdpose::Recipe{};
        holdpose::loadRecipe(arms, recipe);
        const glm::mat3 Wr(holdpose::weaponFrame(recipe.poses[pose]));
        for (int s = 0; s < 2; ++s) {
            glm::vec3 moved = doNudge && s == nudgeSide ? nudge : glm::vec3(0.0f);
            if (s == 1) moved += dragged;
            const glm::vec3 want = before[s] + Wr * moved;
            const float err = glm::length(after[s] - want);
            std::printf("  %s palm: off by %.2f mm after the round trip\n", s ? "right" : "left",
                        static_cast<double>(err * 1000.0f));
            if (err > 0.002f) bad = 1;
        }
    }
    return bad;
}
