// addmenucheck -- is every object in the Add menu what it says, and does the
// menu work by hand?
//
//   1. Every catalogue item makes an entity of its type with its component
//      (created through the component registry, so a mistyped type id shows),
//      named after it, standing on the ground it was given.
//   2. The filter: words in any order and any case, keywords included.
//   3. The menu itself, drawn through the editor's own Gui: it opens with the
//      filter focused, typing narrows the list, Enter adds the first match, a
//      click on a row adds that row -- and the popup closes after either.
//
//   addmenucheck [out.png]     (writes out_all.png and out_filtered.png)

#include <cstdio>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/AddMenu.hpp"
#include "../src/Component.hpp"
#include "../src/UiStyle.hpp"

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("FAIL  %s\n", what.c_str());
}

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : "addmenu.png";

    // --- 1. The catalogue -------------------------------------------------------
    int id = 100;
    for (const addmenu::Item& it : addmenu::catalog()) {
        const std::string n = std::string(it.category) + "/" + it.label;
        if (it.special != addmenu::Special::None) continue;   // made by main's hooks
        addmenu::Target t;
        t.at = glm::vec3(3.0f, 2.0f, -4.0f);
        const Entity e = addmenu::make(it, id++, t, glm::vec3(0.7f));
        check(e.type == it.type, n + ": entity type");
        if (it.component) {
            bool has = false;
            for (const auto& c : e.components.items) has |= std::string(c->typeId()) == it.component;
            check(has, n + ": carries its component '" + it.component + "'");
        }
        if (it.type == EntityType::Light) check(e.components.get<LightComponent>(), n + ": light has a Light");
        check(e.name.rfind(it.label, 0) == 0, n + ": named after the item");
        check(std::abs(e.center.y - e.half.y - it.lift - 2.0f) < 1e-4f && e.center.x == 3.0f,
              n + ": stands on the ground it was given");
        check(e.half.x > 0.0f && e.half.y > 0.0f && e.half.z > 0.0f, n + ": has a size");
        check(it.hint && it.hint[0], n + ": has a tooltip");
    }
    {
        const auto& cat = addmenu::catalog();
        auto find = [&](const char* label) -> const addmenu::Item* {
            for (const auto& it : cat) if (std::string(it.label) == label) return &it;
            return nullptr;
        };
        const auto* spot = find("Spot light");
        const Entity s = addmenu::make(*spot, 1, {}, glm::vec3(0.5f));
        check(s.components.get<LightComponent>()->type == 1 && s.rotation.x == 90.0f,
              "Spot light: a spot, pointing down");
        const Entity c = addmenu::make(*find("Main camera"), 2, {}, glm::vec3(0.5f));
        check(c.components.get<CameraComponent>()->activeOnStart, "Main camera: Play starts with it");
        const Entity b = addmenu::make(*find("Physics box"), 3, {}, glm::vec3(0.5f));
        check(b.components.get<PhysicsComponent>()->dynamic, "Physics box: dynamic");
        const Entity w = addmenu::make(*find("Static collider"), 4, {}, glm::vec3(0.5f));
        check(!w.components.get<PhysicsComponent>()->dynamic, "Static collider: static");
        const Entity box = addmenu::make(*find("Box"), 5, {}, glm::vec3(1.5f, 0.25f, 2.0f));
        check(box.half == glm::vec3(1.5f, 0.25f, 2.0f), "Box: the toolbar's size");
        addmenu::Target centred;
        centred.onGround = false;
        centred.at = glm::vec3(1.0f, 5.0f, 1.0f);
        const Entity child = addmenu::make(*find("Point light"), 6, centred, glm::vec3(0.5f));
        check(child.center == centred.at, "Add child: centred on the target");

        // --- 2. The filter ------------------------------------------------------
        check(addmenu::matches(*find("Spot light"), "LIGHT"), "filter: any case");
        check(addmenu::matches(*find("Spot light"), "light spot"), "filter: words in any order");
        check(!addmenu::matches(*find("Point light"), "spot"), "filter: a word must be found");
        check(addmenu::matches(*find("Player start"), "spawn"), "filter: keywords");
        check(addmenu::matches(*find("Reverb zone"), "audio"), "filter: the category");
        check(addmenu::matches(*find("Box"), ""), "filter: empty finds all");
    }

    // --- 3. The menu, by hand ---------------------------------------------------
    fitzel::Window window(fitzel::WindowConfig{
        .width = 520, .height = 760, .title = "addmenucheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    ui::setBoldFont(gui.boldFont());
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplGlfw_CursorEnterCallback(window.nativeHandle(), 1);

    std::vector<std::string> added;
    addmenu::Actions acts{[&](const addmenu::Item& it, const addmenu::Target&) { added.push_back(it.label); }};
    bool openReq = true, isOpen = false;
    ImVec2 firstRow(0, 0);
    auto frame = [&] {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.25f, 0.27f, 0.30f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        gui.beginFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(fw), static_cast<float>(fh)));
        ImGui::Begin("host", nullptr, ImGuiWindowFlags_NoDecoration);
        if (openReq) {
            ImGui::SetNextWindowPos(ImVec2(30, 30));
            ImGui::OpenPopup("##addMenu");
            openReq = false;
        }
        isOpen = false;
        if (ImGui::BeginPopup("##addMenu")) {
            isOpen = true;
            ImGui::TextDisabled("Add");
            addmenu::body(acts, addmenu::Target{});
            ImGui::EndPopup();
        }
        ImGui::End();
        gui.endFrame();
        window.swapBuffers();
    };
    auto shot = [&](const std::string& path) {
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
        glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        stbi_flip_vertically_on_write(1);
        const bool ok = stbi_write_png(path.c_str(), fw, fh, 4, px.data(), fw * 4) != 0;
        std::printf("%s %s\n", ok ? "wrote" : "FAIL writing", path.c_str());
        check(ok, "screenshot written");
    };
    const auto dot = out.rfind('.');
    const std::string stem = out.substr(0, dot), ext = out.substr(dot);
    auto key = [&](ImGuiKey k) {
        io.AddKeyEvent(k, true); frame();
        io.AddKeyEvent(k, false); frame();
    };

    for (int i = 0; i < 4; ++i) frame();
    check(isOpen, "menu opens");
    check(io.WantTextInput, "the filter has the keyboard when it opens");
    shot(stem + "_all" + ext);

    // Type "spot", Enter: the Spot light.
    for (const char* c = "spot"; *c; ++c) { io.AddInputCharacter(*c); frame(); }
    for (int i = 0; i < 3; ++i) frame();
    shot(stem + "_filtered" + ext);
    key(ImGuiKey_Enter);
    frame();
    check(added.size() == 1 && added[0] == "Spot light", "typing 'spot' + Enter adds the Spot light");
    check(!isOpen, "the menu closes after adding");

    // Open again: the filter starts empty; click the first row (Box).
    openReq = true;
    for (int i = 0; i < 3; ++i) frame();
    check(isOpen, "menu opens again");
    for (const char* c = "player"; *c; ++c) { io.AddInputCharacter(*c); frame(); }
    frame();
    // The list sits under the filter; its first row is about two lines below.
    const float em = ImGui::GetFontSize();
    const ImVec2 row(30 + em * 4.0f, 30 + em * 5.6f);
    for (int i = 0; i < 2; ++i) { io.AddMousePosEvent(row.x, row.y); frame(); }
    io.AddMouseButtonEvent(0, true);  io.AddMousePosEvent(row.x, row.y); frame();
    io.AddMouseButtonEvent(0, false); io.AddMousePosEvent(row.x, row.y); frame();
    frame();
    check(added.size() == 2 && added[1] == "Player start",
          "a click on the row adds it (" + (added.size() > 1 ? added[1] : std::string("nothing")) + ")");
    check(!isOpen, "the menu closes after a click");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
