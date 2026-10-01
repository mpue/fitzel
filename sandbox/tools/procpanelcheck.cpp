// procpanelcheck: the Procedural window (ProcGraphPanel.cpp), driven by real
// ImGui mouse and key events in a frame nobody draws -- the same way a person
// uses it. The panel tells the harness where it drew each control (its probe),
// and the harness clicks there:
//   - a preset tile makes a procedural object, selected, as one undo step;
//   - a node is picked by clicking it; a stepper's + changes its value and
//     re-cooks the object, one undo step; a typed value lands too;
//   - Add node puts a node in unwired; a wire is two clicks (an output dot,
//     then a node) or a drag from dot to dot, a loop is refused and said so,
//     an input's wire pulled off onto nothing comes out, a click on the
//     background puts a wire in the making down;
//   - a dragged node lands where the pointer let go, on the grid, the others
//     staying put, as one undo step and without a cook; Move + a click does
//     the same without a drag; Arrange gives the layout back to the canvas;
//   - Delete closes the chain, Show as output makes one node the object, and
//     undoing everything brings back the preset as it was made.
//   build/release/bin/procpanelcheck.exe
//   build/release/bin/procpanelcheck.exe --png out.png [--size WxH] [--node name] [--preset i]
// The second form draws the window through the editor's own Gui and theme,
// the Ring station open with one node picked, and writes a picture of it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/core/Window.hpp>
#include <fitzel/scene/Camera.hpp>
#include <fitzel/ui/Gui.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/Command.hpp"
#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/EditorContext.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/Modifiers.hpp"
#include "../src/ProcGraph.hpp"
#include "../src/ProcGraphPanel.hpp"
#include "../src/ProcPresets.hpp"
#include "../src/SceneGraph.hpp"
#include "../src/Selection.hpp"
#include "../src/UiStyle.hpp"
#include "../src/ViewportFrame.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++failures;
}

// --png: the window as the editor draws it.
int picture(const std::string& out, int w, int h, const std::string& nodeName, int preset) {
    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "procpanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;

    const fs::path dir = fs::temp_directory_path() / "fitzel-procpanelcheck";
    std::error_code ec;
    fs::create_directories(dir, ec);
    Document                  document;
    std::vector<Entity>&      entities  = document.entities();
    std::vector<MaterialDef>& materials = document.materials();
    Selection                 sel(entities);
    CommandStack              history;
    ModelLibrary              models;
    fitzel::AssetDatabase     assetDb{dir.generic_string()};
    fitzel::Camera            camera({0.0f, 0.0f, 10.0f});
    int         matSel = -1, faceOwner = -1, faceSel = -1, nextId = 1000;
    std::string status;
    EditorContext ed{document, entities, sel, history, materials, matSel, assetDb, models,
                     camera, status, faceOwner, faceSel, nextId,
                     [](glm::vec3, int) {}, [](glm::vec3, const std::string&) {},
                     [](const std::string&) { return false; }};
    procui::Panel panel({ed, [](float) { return glm::vec3(0.0f); }});
    panel.create(preset);

    bool show = true;
    for (int frame = 0; frame < 12; ++frame) {
        window.pollEvents();
        if (frame == 1 && sel.valid())
            if (auto* pg = entities[static_cast<std::size_t>(sel.index())].components.get<ProcGraphComponent>())
                for (const auto& n : pg->graph.nodes)
                    if (n->name == nodeName) panel.pickNode(n->id);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        gui.beginFrame();
        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w) - 16.0f, static_cast<float>(h) - 16.0f),
                                 ImGuiCond_Always);
        panel.draw(show);
        gui.endFrame();
        if (frame + 1 < 12) window.swapBuffers();
    }
    int fw = 0, fh = 0;
    window.framebufferSize(fw, fh);
    std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(out.c_str(), fw, fh, 4, px.data(), fw * 4) != 0;
    std::printf(wrote ? "[procpanelcheck] wrote %s (%dx%d)\n" : "[procpanelcheck] could not write %s\n",
                out.c_str(), fw, fh);
    return wrote ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--png") {
        int w = 1600, h = 900;
        std::string nodeName = "hub";
        int preset = 0;
        for (int i = 3; i + 1 < argc; i += 2) {
            const std::string a = argv[i];
            if (a == "--size") std::sscanf(argv[i + 1], "%dx%d", &w, &h);
            if (a == "--node") nodeName = argv[i + 1];
            if (a == "--preset") preset = std::atoi(argv[i + 1]);
        }
        return picture(argv[2], w, h, nodeName, preset);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // every line out before anything can go wrong
    const fs::path dir = fs::temp_directory_path() / "fitzel-procpanelcheck";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    Document                  document;
    std::vector<Entity>&      entities  = document.entities();
    std::vector<MaterialDef>& materials = document.materials();
    Selection                 sel(entities);
    CommandStack              history;
    ModelLibrary              models;
    fitzel::AssetDatabase     assetDb{dir.generic_string()};
    fitzel::Camera            camera({0.0f, 0.0f, 10.0f});
    int         matSel = -1, faceOwner = -1, faceSel = -1, nextId = 1000;
    std::string status;
    EditorContext ed{document, entities, sel, history, materials, matSel, assetDb, models,
                     camera, status, faceOwner, faceSel, nextId,
                     [](glm::vec3, int) {}, [](glm::vec3, const std::string&) {},
                     [](const std::string&) { return false; }};

    procui::Panel panel({ed, [](float) { return glm::vec3(0.0f, 0.0f, 0.0f); }});
    std::map<std::string, std::pair<ImVec2, ImVec2>> where;
    panel.probe = [&](const std::string& k, ImVec2 a, ImVec2 b) { where[k] = {a, b}; };

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // Wide enough that the station's whole graph is on the canvas at once: a
    // node scrolled out of the child window cannot be clicked, by anyone.
    io.DisplaySize = ImVec2(3800.0f, 1400.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigInputTrickleEventQueue = false;

    bool show = true;
    ImVec2 mouse(3790.0f, 1390.0f);
    bool sized = false;
    auto frame = [&](bool lmb) {
        where.clear();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
        ImGui::NewFrame();
        // The window's own first size is the editor's; this one is the harness's.
        if (sized) ImGui::SetWindowSize("Procedural", ImVec2(3700.0f, 1350.0f));
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        panel.draw(show);
        ImGui::Render();
        sized = true;
        scenegraph::resolve(entities);
    };
    // Click the middle of control `k` as drawn last frame: hover, press, let go.
    auto click = [&](const std::string& k) -> bool {
        auto it = where.find(k);
        if (it == where.end()) {
            check(false, "the panel drew \"" + k + "\"");
            return false;
        }
        mouse = ImVec2(0.5f * (it->second.first.x + it->second.second.x),
                       0.5f * (it->second.first.y + it->second.second.y));
        frame(false);
        frame(false);
        frame(true);
        frame(false);
        frame(false);
        return true;
    };
    auto graph = [&]() -> proc::Graph* {
        if (!sel.valid()) return nullptr;
        auto* pg = entities[static_cast<std::size_t>(sel.index())].components.get<ProcGraphComponent>();
        return pg ? &pg->graph : nullptr;
    };
    auto byName = [&](const std::string& name) -> proc::Node* {
        proc::Graph* g = graph();
        if (!g) return nullptr;
        for (auto& n : g->nodes)
            if (n->name == name) return n.get();
        return nullptr;
    };
    auto setting = [&](const std::string& node, const char* key) -> nlohmann::json {
        proc::Graph* g = graph();
        if (!g) return {};
        nlohmann::json j;
        g->save(j);
        for (const auto& n : j["nodes"])
            if (n.value("name", std::string()) == node && n.contains(key)) return n[key];
        return {};
    };
    auto meshFaces = [&]() -> std::size_t {
        if (!sel.valid()) return 0;
        const auto* mc = entities[static_cast<std::size_t>(sel.index())].components.get<MeshComponent>();
        return mc ? mc->mesh.faces.size() : 0;
    };

    // --- A new object ------------------------------------------------------------------
    frame(false);
    frame(false);
    const unsigned rev0 = history.revision();
    click("tile:0");
    proc::Graph* g = graph();
    check(g != nullptr && entities.size() == 1, "the Ring station tile makes one procedural object, selected");
    if (!g) { std::printf("\nprocpanelcheck: FAILED\n"); return 1; }
    const std::size_t presetHash = proc::hashOf(*g);
    const std::size_t presetFaces = meshFaces();
    check(history.revision() == rev0 + 1 && presetFaces > 5000,
          "...as one undo step, cooked into its mesh", std::to_string(presetFaces) + " faces");
    const auto* ms = entities[0].components.get<ModifierStackComponent>();
    check(ms != nullptr && ms->smooth, "...shaded smooth (a modifier stack with no modifiers)");
    check(materials.size() == 6, "...and its palette is in the library", std::to_string(materials.size()));

    // --- Picking a node, a stepper, a typed value ------------------------------------------
    click("node:hub");
    check(where.count("radius+") == 1, "clicking a node shows its settings");
    const unsigned rev1 = history.revision();
    click("radius+");
    check(setting("hub", "radius") == 8.25 && history.revision() == rev1 + 1,
          "the stepper's + moves the radius one step, as one undo step",
          setting("hub", "radius").dump());
    history.undo(document);
    frame(false);
    check(setting("hub", "radius") == 8.0, "...and undo puts it back");

    click("radius=");
    frame(false);   // the field takes the keyboard the frame after it appears
    check(ImGui::IsAnyItemActive(), "clicking a stepper's value opens it for typing");
    io.AddInputCharactersUTF8("11.5");
    frame(false);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame(false);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame(false);
    frame(false);
    check(setting("hub", "radius") == 11.5, "a typed value lands", setting("hub", "radius").dump());

    // --- Adding: a node that stands on its own --------------------------------------------------
    auto centreOf = [&](const std::string& k) {
        auto it = where.find(k);
        if (it == where.end()) return ImVec2(-1.0f, -1.0f);
        return ImVec2(0.5f * (it->second.first.x + it->second.second.x),
                      0.5f * (it->second.first.y + it->second.second.y));
    };
    auto topLeft = [&](const std::string& k) {
        auto it = where.find(k);
        return it == where.end() ? ImVec2(-1e6f, -1e6f) : it->second.first;
    };
    // Press at `from`, move there in steps with the button held, let go.
    auto drag = [&](ImVec2 from, ImVec2 to) {
        mouse = from;
        frame(false);
        frame(false);
        frame(true);
        for (int k = 1; k <= 8; ++k) {
            mouse = ImVec2(from.x + (to.x - from.x) * k / 8.0f, from.y + (to.y - from.y) * k / 8.0f);
            frame(true);
        }
        frame(false);
        frame(false);
    };
    auto near2 = [](ImVec2 a, ImVec2 b, float tol) {
        return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol;
    };
    const float em = ImGui::GetFontSize();

    click("node:hub");
    const unsigned rev2 = history.revision();
    const std::size_t facesBefore = meshFaces();
    const int outBefore = graph()->output;
    click("add");
    click("add:transform");
    proc::Node* tr = byName("transform1");
    proc::Node* plates = byName("hub_plates");
    proc::Node* hub = byName("hub");
    check(tr && plates && hub && tr->inputs[0] == -1 && plates->inputs[0] == hub->id &&
              graph()->output == outBefore && history.revision() == rev2 + 1 && meshFaces() == facesBefore,
          "Add node puts the node in unwired; the graph and the object stay as they were");
    click("add");
    click("add:box");
    const proc::Node* station = byName("station");
    const std::size_t stationIn = station ? station->inputs.size() : 0;
    const proc::Node* box = byName("box1");
    check(box && stationIn == 10 && graph()->output == station->id,
          "a new shape is not wired into the output either");

    // --- Wiring: two clicks, or a drag ------------------------------------------------------------
    click("out:box1");
    click("node:transform1");
    tr = byName("transform1");
    box = byName("box1");
    check(tr && box && tr->inputs[0] == box->id, "output dot, then a node: wired into it");

    drag(centreOf("out:transform1"), centreOf("in:station:10"));
    station = byName("station");
    tr = byName("transform1");
    check(station && tr && station->inputs.size() == 11 && station->inputs[10] == tr->id &&
              meshFaces() == facesBefore + 6,
          "dragging an output onto a merge's free dot wires it in, and the object grows",
          std::to_string(station ? station->inputs.size() : 0) + " inputs");

    const std::size_t h0 = proc::hashOf(*graph());
    click("out:station");
    click("in:transform1:0");
    check(proc::hashOf(*graph()) == h0 && byName("transform1")->inputs[0] == byName("box1")->id,
          "a wire that would make a loop is refused");

    // An empty spot of the canvas: its bottom right corner, which the graph
    // does not reach in a window this wide.
    const ImVec2 empty(where["canvasview"].second.x - 60.0f, where["canvasview"].second.y - 60.0f);
    drag(centreOf("in:transform1:0"), empty);
    check(byName("transform1")->inputs[0] == -1, "pulling an input's wire off onto nothing takes it out");

    click("out:ring");
    mouse = empty;
    frame(false); frame(false); frame(true); frame(false); frame(false);
    click("in:transform1:0");
    check(byName("transform1")->inputs[0] == -1,
          "a click on the background puts a wire in the making down");
    mouse = empty;
    frame(false); frame(false); frame(true); frame(false); frame(false);

    // --- Moving nodes --------------------------------------------------------------------------------
    check(!graph()->placedByHand(), "until a node is moved, the canvas lays the graph out itself");
    const ImVec2 hub0 = topLeft("node:hub"), spine0 = topLeft("node:spine");
    const std::uint64_t meshRev = entities[0].components.get<MeshComponent>()->revision;
    const unsigned rev3 = history.revision();
    const ImVec2 grab = centreOf("node:hub");
    drag(grab, ImVec2(grab.x + 300.0f, grab.y + 120.0f));
    frame(false);
    const ImVec2 hub1 = topLeft("node:hub");
    check(graph()->placedByHand() && near2(hub1, ImVec2(hub0.x + 300.0f, hub0.y + 120.0f), em * 0.5f + 1.0f),
          "a node goes where it is dragged (onto the grid)",
          std::to_string(hub1.x - hub0.x) + ", " + std::to_string(hub1.y - hub0.y));
    bool allPinned = true;
    for (const auto& n : graph()->nodes) allPinned = allPinned && n->placed;
    check(near2(topLeft("node:spine"), spine0, 0.5f) && allPinned,
          "...and every other node stays where it stood, held there from now on");
    check(history.revision() == rev3 + 1, "one undo step for the whole drag");
    check(entities[0].components.get<MeshComponent>()->revision == meshRev,
          "moving a node does not cook the object again");
    history.undo(document);
    frame(false);
    frame(false);
    check(!graph()->placedByHand() && near2(topLeft("node:hub"), hub0, 0.5f),
          "undo: back to the layout the canvas makes itself");
    history.redo(document);
    frame(false);
    frame(false);

    // A spot on the canvas that nothing drawn there covers (a node, a dot):
    // from the bottom right, row by row, the first point clear of all of them.
    auto clearSpot = [&](float fromRight, float fromBottom) {
        const auto view = where["canvasview"];
        for (float y = view.second.y - fromBottom; y > view.first.y + 40.0f; y -= 25.0f)
            for (float x = view.second.x - fromRight; x > view.first.x + 40.0f; x -= 25.0f) {
                bool free = true;
                for (const auto& [k, r] : where) {
                    if (k.rfind("node:", 0) != 0 && k.rfind("in:", 0) != 0 && k.rfind("out:", 0) != 0) continue;
                    if (x > r.first.x - 15.0f && x < r.second.x + 15.0f &&
                        y > r.first.y - 15.0f && y < r.second.y + 15.0f) { free = false; break; }
                }
                if (free) return ImVec2(x, y);
            }
        return ImVec2(view.second.x - fromRight, view.second.y - fromBottom);
    };
    click("node:spine");
    click("move");
    const ImVec2 target = clearSpot(300.0f, 150.0f);
    mouse = target;
    frame(false); frame(false); frame(true); frame(false); frame(false);
    const ImVec2 spine1 = topLeft("node:spine");
    const ImVec2 sz(where["node:spine"].second.x - spine1.x, where["node:spine"].second.y - spine1.y);
    check(near2(ImVec2(spine1.x + 0.5f * sz.x, spine1.y + 0.5f * sz.y), target, em * 0.5f + 1.0f),
          "Move, then a click: the node goes there without a drag");

    click("add");
    click("add:sphere");
    const proc::Node* sph = byName("sphere1");
    const proc::Node* sp  = byName("spine");
    check(sph && sp && sph->placed && sph->pos.y > sp->pos.y,
          "laid out by hand, a new node is placed below the picked one");

    click("arrange");
    check(!graph()->placedByHand(), "Arrange lays everything out by itself again");

    // --- Delete, bypass, show ---------------------------------------------------------------------
    click("out:box1");
    click("in:transform1:0");
    click("node:transform1");
    click("delete");
    station = byName("station");
    box = byName("box1");
    check(!byName("transform1") && station && box && station->inputs.back() == box->id,
          "Delete closes the chain: what the step fed reads its input");

    click("node:hub_plates");
    click("bypass");
    check(byName("hub_plates") && byName("hub_plates")->bypass, "Bypass is a click");

    click("node:ring");
    click("show");
    check(graph()->output == byName("ring")->id && meshFaces() == 96 * 4,
          "Show as output makes one node the object", std::to_string(meshFaces()) + " faces");
    const glm::vec3 half = entities[0].half;
    check(std::abs(half.x - 65.0f) < 0.01f && std::abs(half.y - 3.5f) < 0.01f,
          "...and the object's box follows its mesh", std::to_string(half.x) + ", " + std::to_string(half.y));
    // --- A curve: its points as a list; the overlay -------------------------------------------------
    click("add");
    click("add:curve");
    auto curvePts = [&] {
        const nlohmann::json s = setting("curve1", "points");
        return s.is_string() ? proc::parsePoints(s.get<std::string>()) : std::vector<glm::vec3>{};
    };
    check(curvePts().size() == 4 && where.count("points.add") == 1, "a new Curve shows its points as a list");
    click("points.add");
    check(curvePts().size() == 5 && curvePts().back() == glm::vec3(40.0f, 0.0f, 6.0f),
          "Add point carries the line on the way its last stretch went");
    click("points.0.y+");
    check(curvePts().front() == glm::vec3(0.0f, 0.5f, 0.0f), "a point's stepper moves it");
    click("points.remove.4");
    check(curvePts().size() == 4, "and X takes a point off");

    // The overlay: the picked node's curve, drawn into the Scene window.
    const fitzel::Camera eye({0.0f, 40.0f, 260.0f});
    const ViewportFrame view = ViewportFrame::looking(eye, ImVec2(0.0f, 0.0f), 1600.0f, 900.0f,
                                                      ImVec2(0.0f, 0.0f), true);
    auto overlayVerts = [&] {
        where.clear();
        io.AddMousePosEvent(mouse.x, mouse.y);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        panel.draw(show);
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(1600.0f, 900.0f));
        ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const int before = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        panel.viewport(view);
        const int drawn = ImGui::GetWindowDrawList()->VtxBuffer.Size - before;
        ImGui::End();
        ImGui::Render();
        return drawn;
    };
    const int curveDrawn = overlayVerts();
    check(curveDrawn > 0, "the picked curve is drawn over the scene", std::to_string(curveDrawn) + " vertices");
    click("add");
    click("add:selectpoints");
    click("out:curve1");
    click("node:selectpoints1");
    click("node:selectpoints1");
    const proc::Node* selNode = byName("selectpoints1");
    check(selNode && selNode->inputs[0] == byName("curve1")->id, "a Select points node wired after the curve");
    const int selDrawn = overlayVerts();
    check(selDrawn > curveDrawn, "...and its selected points are lit on top of the line",
          std::to_string(selDrawn) + " vertices");
    show = false;
    check(overlayVerts() == 0, "with the window closed, nothing is drawn");
    show = true;
    frame(false);

    // --- Picking nodes: one, several, a box --------------------------------------------------------
    auto key = [&](ImGuiKey k, ImGuiKey mod = ImGuiKey_None) {
        if (mod != ImGuiKey_None) io.AddKeyEvent(mod, true);
        io.AddKeyEvent(k, true);
        frame(false);
        io.AddKeyEvent(k, false);
        if (mod != ImGuiKey_None) io.AddKeyEvent(mod, false);
        frame(false);
        frame(false);
    };
    auto clickWith = [&](const std::string& k, ImGuiKey mod) {
        io.AddKeyEvent(mod, true);
        click(k);
        io.AddKeyEvent(mod, false);
        frame(false);
    };
    auto rightClick = [&](ImVec2 at) {
        mouse = at;
        frame(false);
        frame(false);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        frame(false);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        frame(false);
        frame(false);
    };
    auto pickedNames = [&] {
        std::vector<std::string> out;
        for (int id : panel.pickedNodes())
            if (const proc::Node* n = graph()->find(id)) out.push_back(n->name);
        std::sort(out.begin(), out.end());
        std::string s;
        for (const auto& n : out) s += n + " ";
        return s;
    };
    click("arrange");
    click("node:hub");
    check(pickedNames() == "hub ", "a click picks one node", pickedNames());
    clickWith("node:spine", ImGuiMod_Shift);
    check(pickedNames() == "hub spine ", "Shift+click adds one", pickedNames());
    clickWith("node:hub", ImGuiMod_Shift);
    check(pickedNames() == "spine ", "...and Shift+click on a picked one takes it away", pickedNames());
    {
        const auto hubR = where["node:hub"], spineR = where["node:spine"];
        const ImVec2 from(std::min(hubR.first.x, spineR.first.x) - 6.0f, std::min(hubR.first.y, spineR.first.y) - 6.0f);
        const ImVec2 to(std::max(hubR.second.x, spineR.second.x) + 6.0f, std::max(hubR.second.y, spineR.second.y) - 4.0f);
        drag(from, to);
    }
    check(pickedNames() == "hub spine ", "a box drawn on the empty canvas picks what it touches", pickedNames());
    {
        const ImVec2 hub0 = topLeft("node:hub"), spine0 = topLeft("node:spine");
        const unsigned rev = history.revision();
        const ImVec2 grab = centreOf("node:hub");
        drag(grab, ImVec2(grab.x + 160.0f, grab.y + 40.0f));
        frame(false);
        const ImVec2 hub1 = topLeft("node:hub"), spine1 = topLeft("node:spine");
        check(near2(ImVec2(hub1.x - hub0.x, hub1.y - hub0.y), ImVec2(spine1.x - spine0.x, spine1.y - spine0.y), em + 1.0f) &&
                  hub1.x - hub0.x > 100.0f && history.revision() == rev + 1,
              "dragging one picked node moves all the picked ones, one undo step");
    }

    // --- The Delete key: nodes, never the object --------------------------------------------------
    {
        mouse = empty;
        frame(false);
        frame(false);
        check(panel.ownsKeys(), "with the pointer on the window, the keyboard is the window's");
        const std::size_t ents = entities.size();
        const std::size_t nodes = graph()->nodes.size();
        const unsigned rev = history.revision();
        key(ImGuiKey_Delete);
        check(!byName("hub") && !byName("spine") && graph()->nodes.size() == nodes - 2 &&
                  entities.size() == ents && history.revision() == rev + 1,
              "Delete takes the picked nodes out -- the object stays -- in one undo step");
        history.undo(document);
        frame(false);
        check(byName("hub") && byName("spine"), "...and undo brings both back");
        key(ImGuiKey_A);
        check(panel.pickedNodes().size() == graph()->nodes.size(), "A picks every node");
        key(ImGuiKey_A, ImGuiMod_Alt);
        check(panel.pickedNodes().empty(), "Alt+A picks none");
    }

    // --- Shift+A: the Add menu at the pointer ---------------------------------------------------------
    {
        const ImVec2 spot(empty.x - 200.0f, empty.y - 120.0f);
        mouse = spot;
        frame(false);
        key(ImGuiKey_A, ImGuiMod_Shift);
        check(where.count("addhere:search") == 1 && where.count("addhere:box") == 1,
              "Shift+A opens the Add menu, with a search field");
        click("addhere:box");
        const proc::Node* b2 = byName("box2");
        frame(false);
        const ImVec2 c = centreOf("node:box2");
        check(b2 && b2->placed && near2(c, spot, em * 1.2f),
              "a node from the menu lands where the pointer was",
              std::to_string(c.x - spot.x) + ", " + std::to_string(c.y - spot.y));
        mouse = ImVec2(spot.x, spot.y + 200.0f);
        frame(false);
        key(ImGuiKey_A, ImGuiMod_Shift);
        io.AddInputCharactersUTF8("swe");
        frame(false);
        key(ImGuiKey_Enter);
        check(byName("sweep1") != nullptr, "typing into the menu and Enter adds the first match (sweep)");
        rightClick(ImVec2(spot.x - 260.0f, spot.y));
        check(where.count("addhere:search") == 1, "a right-click on the empty canvas opens it too");
        key(ImGuiKey_Escape);
        rightClick(centreOf("node:box2"));
        check(where.count("menu:bypass") == 1, "a right-click on a node opens its menu");
        click("menu:bypass");
        check(byName("box2") && byName("box2")->bypass, "...whose Bypass is a click");
        rightClick(centreOf("node:box2"));
        click("menu:delete");
        check(!byName("box2"), "...and so is Delete");
    }

    // --- The Prefab node ---------------------------------------------------------------------------------
    {
        // A stand-in for the project's prefab cache: a lamp post, a root with a part.
        int spawned = 0;
        panel.prefabNames = [] { return std::vector<std::string>{"Lamp"}; };
        panel.spawnPrefab = [&](const std::string& name, int& counter) {
            std::vector<Entity> es;
            if (name != "Lamp") return es;
            ++spawned;
            Entity root;
            root.id = counter++;
            root.name = "Lamp";
            root.type = EntityType::Empty;
            Entity part;
            part.id = counter++;
            part.name = "Lamp post";
            part.parent = root.id;
            part.localCenter = glm::vec3(0.0f, 1.0f, 0.0f);
            es.push_back(root);
            es.push_back(part);
            return es;
        };
        const int owner = entities[0].id;
        auto made = [&] {
            std::vector<const Entity*> out;
            for (const Entity& en : entities)
                if (en.parent == owner && en.components.get<ProcMadeComponent>()) out.push_back(&en);
            return out;
        };
        // The station is the object again (a test above showed the ring alone).
        click("node:station");
        click("show");
        mouse = ImVec2(empty.x - 420.0f, empty.y - 260.0f);
        frame(false);
        key(ImGuiKey_A, ImGuiMod_Shift);
        click("addhere:prefab");
        click("prefab");
        click("prefab:Lamp");
        check(setting("prefab1", "prefab") == "Lamp" && made().empty(),
              "a Prefab node, the prefab picked -- nothing placed while it is not wired in");
        const proc::Node* st = byName("station");
        const std::string freeDot = "in:station:" + std::to_string(st ? st->inputs.size() : 0);
        drag(centreOf("out:prefab1"), centreOf(freeDot));
        frame(false);
        check(made().size() == 1 && entities.size() >= 3,
              "wired into the output: the prefab stands under the object, as objects",
              std::to_string(made().size()) + " placed");
        const glm::vec3 at0 = made().empty() ? glm::vec3(0.0f) : made()[0]->localCenter;
        const int id0 = made().empty() ? -1 : made()[0]->id;
        click("node:prefab1");
        click("move.x+");
        click("move.x+");
        const glm::vec3 at1 = made().empty() ? glm::vec3(0.0f) : made()[0]->localCenter;
        check(made().size() == 1 && std::abs(at1.x - at0.x - 1.0f) < 1e-3f,
              "moving it in the graph puts it down afresh, one metre on");
        const int spawnedBefore = spawned;
        click("node:hub");
        click("radius+");
        check(spawned == spawnedBefore && made().size() == 1,
              "a change to the faces alone leaves the placed prefab alone");
        history.undo(document);   // the radius
        history.undo(document);   // the second metre
        frame(false);
        check(made().size() == 1 && std::abs(made()[0]->localCenter.x - at0.x - 0.5f) < 1e-3f,
              "undo puts the placed prefab back where it was");
        click("node:prefab1");
        key(ImGuiKey_Delete);
        check(made().empty() && !byName("prefab1"), "deleting the Prefab node takes its objects away");
        history.undo(document);
        frame(false);
        check(made().size() == 1, "...and undo brings them back");
        (void)id0;
    }

    // --- Undo all the way back --------------------------------------------------------------------
    // (revision() counts undos too, so the walk back stops on the preset itself.)
    int undos = 0;
    while (history.canUndo() && !(graph() && proc::hashOf(*graph()) == presetHash)) {
        history.undo(document);
        ++undos;
    }
    frame(false);
    check(graph() && proc::hashOf(*graph()) == presetHash && meshFaces() == presetFaces,
          "undoing every step brings the preset back exactly",
          std::to_string(undos) + " steps, " + std::to_string(meshFaces()) + " faces");
    history.undo(document);
    frame(false);
    check(entities.empty(), "...and one more takes the object away");
    frame(false);
    check(where.count("tile:0") == 1, "with nothing selected, the window offers the presets");

    ImGui::DestroyContext();
    std::printf("\nprocpanelcheck: %s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}