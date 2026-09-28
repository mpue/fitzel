// editorcheck: the editor's core as the tools outside main() see it
// (EditorContext.hpp), and those tools, without a window.
//
// A tool that lived inside main() could only be checked by clicking it. Moved
// out onto the EditorContext, most of one no longer needs a window at all, so
// what it does to the scene can be measured:
//   - an asset dropped on the scene (SceneDrop): a material dropped on an
//     object dresses that object, as one undo step, and selects it; dropped on
//     a face of a modelled mesh it dresses that face alone and hands the face
//     to the modelling panel; a drop on empty sky changes nothing.
//   - the mesh paint brush (MeshPaintPanel): a held button paints the chosen
//     slot onto the mesh under the cursor, splitting its faces, and the whole
//     stroke is one undo step; an empty slot lays nothing down; a rival tool
//     or a selection off the mesh makes it let go of the left button. It runs
//     inside a real ImGui frame, just one nobody draws.
//   - the ground brushes' drag (GroundBrush: grass, trees, flowers, scatter):
//     a press stamps, a held drag stamps again only every `spacing` metres, a
//     fresh press starts over, erasing rubs every frame and never stamps, and
//     a cursor off the ground does nothing.
//   - the viewport's marks (ViewportOverlay, ViewportFrame::wireBox): the
//     selection's box and an Empty's icon are drawn when they are in front of
//     the camera and not at all from behind it, and nothing without a selection.
//   - selecting in the viewport (ViewportPick): a click takes the nearest
//     object under the cursor and the next one behind it when repeated, a
//     click on nothing clears, Ctrl+click toggles one, a Ctrl+drag box adds
//     every centre inside it, Create mode places on empty ground, and a click
//     the modelling panel takes selects nothing.
//   - the transform gizmo (TransformGizmo), dragged by its centre with real
//     mouse events through ImGuizmo: the object moves with the pointer and its
//     child with it, as one undo step banked after the release; Ctrl on a world
//     move lands on the grid, but only on the axes the drag moves; the other
//     selected roots take the same delta; a picked face moves alone while the
//     rest of the mesh stays where it is in the world, and a face scaled and
//     held still stays at the size the pointer says -- it does not keep growing.
//   build/release/bin/editorcheck.exe

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/scene/Camera.hpp>
#include <imgui.h>

#include "../src/Command.hpp"
#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/EditorContext.hpp"
#include "../src/GroundBrush.hpp"
#include "../src/ViewportOverlay.hpp"
#include "../src/ViewportPick.hpp"
#include "../src/TransformGizmo.hpp"
#include "../src/ModelingTools.hpp"
#include "../src/SceneGraph.hpp"
#include "../src/MeshPaintPanel.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/SceneDrop.hpp"
#include "../src/SceneTypes.hpp"
#include "../src/Selection.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++failures;
}

Entity makeBox(int id, const glm::vec3& at) {
    Entity e;
    e.id = id;
    e.name = "Box " + std::to_string(id);
    e.type = EntityType::Box;
    e.half = glm::vec3(1.0f);
    e.center = e.localCenter = at;
    return e;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "fitzel-editorcheck";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // The editor's core, as main wires it -- minus the model import, which a
    // scene of primitives never reaches.
    Document                  document;
    std::vector<Entity>&      entities  = document.entities();
    std::vector<MaterialDef>& materials = document.materials();
    Selection                 sel(entities);
    CommandStack              history;
    ModelLibrary              models;
    fitzel::AssetDatabase     assetDb{dir.generic_string()};
    fitzel::Camera            camera({0.0f, 0.0f, 10.0f});   // looking down -Z at the origin
    int         matSel = -1, faceOwner = -1, faceSel = -1;
    std::string status;
    EditorContext ed{document, entities, sel, history, materials, matSel, assetDb, models,
                     camera, status, faceOwner, faceSel,
                     [](glm::vec3, int) {}, [](glm::vec3, const std::string&) {},
                     [](const std::string&) { return false; }};

    // The viewport: a 16:9 image, the cursor in its middle.
    ViewportFrame view;
    view.viewProj    = camera.projectionMatrix(16.0f / 9.0f) * camera.viewMatrix();
    view.w           = 1600.0f;
    view.h           = 900.0f;
    view.hovered     = true;
    view.mouseNdc    = glm::vec2(0.0f);
    view.pickTerrain = [](glm::vec2, const glm::mat4&, glm::vec3&) { return false; };

    MaterialDef red;
    red.assetId = fitzel::AssetId::generate();
    red.name    = "Red";
    materials.push_back(red);
    MaterialDef blue;
    blue.assetId = fitzel::AssetId::generate();
    blue.name    = "Blue";
    materials.push_back(blue);

    // --- A material dropped on a box ------------------------------------------------
    entities.push_back(makeBox(1, glm::vec3(0.0f)));
    scenedrop::dropOnScene(ed, view, red.assetId);
    {
        const auto* mc = entities[0].components.get<MaterialComponent>();
        check(mc && mc->material == red.assetId, "a material dropped on a box dresses it");
        check(sel.valid() && sel.index() == 0 && matSel == 0,
              "...selects it, and shows the material in the Materials panel");
        check(history.canUndo(), "...as one undo step", history.undoName());
        history.undo(document);
        check(!entities[0].components.get<MaterialComponent>(), "and one undo takes it off again");
    }

    // --- A drop on empty sky --------------------------------------------------------
    {
        ViewportFrame corner = view;
        corner.mouseNdc = glm::vec2(0.95f, 0.95f);   // well clear of the box
        const bool undoBefore = history.canUndo();
        scenedrop::dropOnScene(ed, corner, blue.assetId);
        check(!entities[0].components.get<MaterialComponent>() && history.canUndo() == undoBefore,
              "a material dropped on empty sky changes nothing");
    }

    // --- A material dropped on a face of a modelled mesh ---------------------------
    entities.clear();
    sel.clear();
    {
        Entity e = makeBox(2, glm::vec3(0.0f));
        auto mc = std::make_unique<MeshComponent>();
        mc->mesh = EditMesh::box(e.half);
        e.components.items.push_back(std::move(mc));
        entities.push_back(std::move(e));
    }
    scenedrop::dropOnScene(ed, view, blue.assetId);
    {
        const auto* mc = entities[0].components.get<MeshComponent>();
        int dressed = 0, which = -1;
        for (int f = 0; mc && f < static_cast<int>(mc->mesh.faces.size()); ++f)
            if (mc->mesh.faceMaterial(f) == blue.assetId) { ++dressed; which = f; }
        // The face the ray hits first is the one facing the camera: +Z.
        glm::vec3 n(0.0f);
        if (which >= 0) {
            const std::vector<glm::vec3> w = meshFaceWorld(entities[0], *mc, which);
            if (w.size() >= 3) n = glm::normalize(glm::cross(w[1] - w[0], w[2] - w[0]));
        }
        check(dressed == 1 && n.z > 0.9f, "a material dropped on a mesh dresses the face under the cursor",
              std::to_string(dressed) + " face(s), normal z " + std::to_string(n.z).substr(0, 5));
        check(!entities[0].components.get<MaterialComponent>(), "...and not the whole object");
        check(faceOwner == 2 && faceSel == which, "...and hands that face to the modelling panel");
        history.undo(document);
        const auto* after = entities[0].components.get<MeshComponent>();
        bool clean = after != nullptr;
        for (int f = 0; after && f < static_cast<int>(after->mesh.faces.size()); ++f)
            clean = clean && !after->mesh.faceMaterial(f).valid();
        check(clean, "one undo takes the face's material off again");
    }

    // --- The mesh paint brush ---------------------------------------------------------
    // An ImGui context with no window behind it: the brush reads the mouse
    // button and Alt from it and draws its ring into the current window.
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // the atlas never leaves the CPU
    // One editor frame with the left button held or not, the brush inside the
    // Scene window as main calls it.
    meshpaintui::Brush brush;
    bool paintMode = true;
    auto paintFrame = [&](bool lmb, bool othersActive = false) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
        ImGui::NewFrame();
        ImGui::Begin("Scene");
        meshpaintui::brushViewport(ed, view, brush, paintMode, othersActive, 1.0f / 60.0f);
        ImGui::End();
        ImGui::Render();
    };
    auto slotWeight = [](const MeshComponent& mc, int slot) {
        float most = 0.0f;
        for (const glm::vec4& w : mc.mesh.paint) most = std::max(most, w[slot]);
        return most;
    };

    entities.clear();
    sel.clear();
    faceOwner = faceSel = -1;
    {
        Entity e = makeBox(3, glm::vec3(0.0f));
        auto mc = std::make_unique<MeshComponent>();
        mc->mesh = EditMesh::box(e.half);
        mc->paintSlots[0].material = red.assetId;   // slot 0 filled, slot 1 left empty
        e.components.items.push_back(std::move(mc));
        entities.push_back(std::move(e));
    }
    sel.select(3);
    const int facesBefore = static_cast<int>(
        entities[0].components.get<MeshComponent>()->mesh.faces.size());
    {
        const unsigned rev = history.revision();
        for (int i = 0; i < 20; ++i) paintFrame(true);
        const auto* mc = entities[0].components.get<MeshComponent>();
        const int faces = static_cast<int>(mc->mesh.faces.size());
        check(slotWeight(*mc, 0) > 0.1f && slotWeight(*mc, 1) == 0.0f,
              "a held brush paints the chosen slot onto the mesh",
              "slot 0 weight " + std::to_string(slotWeight(*mc, 0)).substr(0, 5));
        check(faces > facesBefore, "...splitting the faces it crosses",
              std::to_string(facesBefore) + " -> " + std::to_string(faces) + " faces");
        check(history.revision() == rev && brush.stroking, "...and banks nothing while held");
        paintFrame(false);
        check(history.revision() == rev + 1 && !brush.stroking,
              "letting go banks the whole stroke as one undo step", history.undoName());
        history.undo(document);
        const auto* after = entities[0].components.get<MeshComponent>();
        check(after && slotWeight(*after, 0) == 0.0f &&
                  static_cast<int>(after->mesh.faces.size()) == facesBefore,
              "one undo takes the stroke off, splits and all");
    }
    {
        brush.slot = 1;                              // empty
        const unsigned rev = history.revision();
        for (int i = 0; i < 5; ++i) paintFrame(true);
        paintFrame(false);
        const auto* mc = entities[0].components.get<MeshComponent>();
        check(slotWeight(*mc, 1) == 0.0f && history.revision() == rev,
              "an empty slot lays nothing down");
        brush.slot = 0;
    }
    {
        // A rival grabs the left button mid-stroke: the stroke is banked, not lost.
        const unsigned rev = history.revision();
        for (int i = 0; i < 5; ++i) paintFrame(true);
        paintFrame(true, true);
        check(!paintMode && !brush.stroking && history.revision() == rev + 1,
              "a rival tool takes the left button: the brush banks its stroke and lets go");
        paintFrame(false);
    }
    {
        paintMode = true;
        sel.clear();
        paintFrame(false);
        check(!paintMode, "with no mesh selected the brush lets go of the left button");
    }
    // --- The ground brushes' drag ------------------------------------------------------
    {
        // Flat ground under the whole viewport, 20 m to an NDC unit, so where
        // the cursor is IS where the brush lands.
        ViewportFrame ground = view;
        ground.pickTerrain = [](glm::vec2 ndc, const glm::mat4&, glm::vec3& hit) {
            hit = glm::vec3(ndc.x * 20.0f, 0.0f, ndc.y * 20.0f);
            return true;
        };
        std::vector<glm::vec2> puts;
        int  rubs = 0;
        glm::vec2 last(1e9f);
        bool eraseToggle = false;
        // One frame, the cursor `x` metres along the ground, the button held or not.
        auto dragFrame = [&](float x, bool lmb) {
            ground.mouseNdc = glm::vec2(x / 20.0f, 0.0f);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
            ImGui::NewFrame();
            ImGui::Begin("Scene");
            const groundbrush::Aim at = groundbrush::aim(ground, eraseToggle);
            groundbrush::drag(at, last, 2.0f,
                              [&](glm::vec2 p) { puts.push_back(p); },
                              [&](glm::vec2) { ++rubs; });
            groundbrush::ring(ground, at, 3.0f, IM_COL32(120, 235, 120, 220));
            ImGui::End();
            ImGui::Render();
        };
        dragFrame(0.0f, false);
        dragFrame(0.0f, true);
        check(puts.size() == 1 && std::abs(puts[0].x) < 1e-4f, "a press on the ground stamps once");
        dragFrame(0.0f, true);
        dragFrame(1.0f, true);
        dragFrame(1.9f, true);
        check(puts.size() == 1, "...and holding within the spacing lays nothing more",
              std::to_string(puts.size()) + " stamps");
        dragFrame(2.5f, true);
        dragFrame(4.0f, true);
        dragFrame(5.0f, true);
        check(puts.size() == 3 && std::abs(puts[1].x - 2.5f) < 1e-3f && std::abs(puts[2].x - 5.0f) < 1e-3f,
              "a held drag stamps again every time it has gone the spacing",
              std::to_string(puts.size()) + " stamps");
        dragFrame(5.0f, false);
        dragFrame(5.0f, true);
        check(puts.size() == 4, "a fresh press on the same spot stamps again");
        dragFrame(5.0f, false);
        eraseToggle = true;
        const std::size_t stamped = puts.size();
        for (int i = 0; i < 4; ++i) dragFrame(6.0f, true);
        check(rubs == 4 && puts.size() == stamped, "erasing rubs every held frame and never stamps",
              std::to_string(rubs) + " rubs");
        dragFrame(6.0f, false);
        eraseToggle = false;
        ground.hovered = false;
        dragFrame(9.0f, true);
        dragFrame(12.0f, true);
        check(puts.size() == stamped && rubs == 4, "a cursor off the viewport stamps nothing");
        dragFrame(12.0f, false);
    }

    // --- The viewport's marks -----------------------------------------------------------
    {
        // How many vertices one call adds to the Scene window's draw list.
        auto drawn = [&](const std::function<void()>& draw) {
            ImGui::NewFrame();
            ImGui::Begin("Scene");
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const int before = dl->VtxBuffer.Size;
            draw();
            const int added = dl->VtxBuffer.Size - before;
            ImGui::End();
            ImGui::Render();
            return added;
        };
        entities.clear();
        sel.clear();
        entities.push_back(makeBox(4, glm::vec3(0.0f)));                 // in view
        entities.push_back(makeBox(5, glm::vec3(0.0f, 0.0f, 20.0f)));    // behind the camera
        check(drawn([&] { overlay::selection(ed, view); }) == 0, "no selection, no box");
        sel.select(4);
        check(drawn([&] { overlay::selection(ed, view); }) > 0, "the selected box is outlined");
        sel.select(5);
        check(drawn([&] { overlay::selection(ed, view); }) == 0,
              "...and not from behind the camera");
        std::vector<Entity> empties;
        empties.push_back(makeBox(6, glm::vec3(0.0f)));
        empties.back().type = EntityType::Empty;
        check(drawn([&] { overlay::empties(empties, view); }) > 0, "an Empty in view gets its icon");
        empties.back().center = glm::vec3(0.0f, 0.0f, 20.0f);
        check(drawn([&] { overlay::empties(empties, view); }) == 0, "...and not from behind the camera");
        sel.clear();
    }

    // --- Selecting in the viewport ----------------------------------------------------
    {
        io.ConfigInputTrickleEventQueue = false;   // a click and its position land together
        entities.clear();
        sel.clear();
        entities.push_back(makeBox(7, glm::vec3(0.0f)));                  // nearest on the ray
        entities.push_back(makeBox(8, glm::vec3(0.0f, 0.0f, -5.0f)));     // behind it
        viewpick::Picker picker;
        viewpick::Host   host;
        host.canPick = true;
        std::vector<glm::vec3> placed;
        host.place = [&](const glm::vec3& at) { placed.push_back(at); };
        // One frame: the cursor at an NDC point, the left button, Ctrl.
        auto pickFrame = [&](glm::vec2 ndc, bool lmb, bool ctrl) {
            ViewportFrame v = view;
            v.mouseNdc = ndc;
            io.AddMousePosEvent(v.origin.x + (ndc.x * 0.5f + 0.5f) * v.w,
                                v.origin.y + (0.5f - ndc.y * 0.5f) * v.h);
            io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
            ImGui::NewFrame();
            ImGui::Begin("Scene");
            viewpick::click(ed, v, picker, host);
            ImGui::End();
            ImGui::Render();
        };
        auto clickAt = [&](glm::vec2 ndc, bool ctrl = false) {
            pickFrame(ndc, true, ctrl);
            pickFrame(ndc, false, ctrl);
        };
        const glm::vec2 centre(0.0f), sky(0.95f, 0.95f);
        clickAt(centre);
        check(sel.valid() && sel.activeId() == 7, "a click takes the nearest object under the cursor");
        clickAt(centre);
        check(sel.activeId() == 8, "...clicked again, the one behind it");
        clickAt(centre);
        check(sel.activeId() == 7, "...and again, round to the nearest");
        clickAt(sky);
        check(!sel.valid(), "a click on nothing clears the selection");
        clickAt(centre, true);
        check(sel.contains(7) && !sel.contains(8), "Ctrl+click toggles the object in");
        clickAt(centre, true);
        check(!sel.contains(7), "...and out again");
        pickFrame(glm::vec2(-0.99f, 0.99f), true, true);
        pickFrame(glm::vec2(0.99f, -0.99f), true, true);
        pickFrame(glm::vec2(0.99f, -0.99f), false, true);
        check(sel.contains(7) && sel.contains(8), "a Ctrl+drag box adds every centre inside it");
        sel.clear();
        host.placeMode = true;
        view.pickTerrain = [](glm::vec2, const glm::mat4&, glm::vec3& hit) {
            hit = glm::vec3(3.0f, 0.0f, -7.0f);
            return true;
        };
        clickAt(sky);
        check(placed.size() == 1 && placed[0] == glm::vec3(3.0f, 0.0f, -7.0f),
              "in Create mode a click on empty ground places there");
        host.placeMode = false;
        host.meshClick = [] { return true; };
        clickAt(centre);
        check(!sel.valid(), "a click the modelling panel takes selects nothing");
        host.meshClick = nullptr;
        host.canPick = false;
        clickAt(centre);
        check(!sel.valid() && placed.size() == 1, "and while a tool owns the button, a click does nothing");
    }

    // --- The transform gizmo --------------------------------------------------------------
    {
        // The Scene window pinned over the whole display, as ImGuizmo only takes
        // the mouse over the window its draw list belongs to.
        gizmo::Drag drag;
        auto gizmoFrame = [&](glm::vec2 px, bool lmb, bool ctrl, const gizmo::Settings& gs) {
            io.AddMousePosEvent(px.x, px.y);
            io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
            ImGui::NewFrame();
            ImGuizmo::BeginFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(view.w, view.h));
            ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings);
            gizmo::frame(ed, view, drag, gs);
            ImGui::End();
            ImGui::Render();
        };
        auto screenOf = [&](const glm::vec3& w) {
            ImVec2 s;
            view.toScreen(w, s);
            return glm::vec2(s.x, s.y);
        };
        // Press on the gizmo's centre at `from`, drag to `to` over a few frames,
        // let go, and one frame more -- the one the drag is banked on.
        auto dragGizmo = [&](glm::vec2 from, glm::vec2 to, bool ctrl, const gizmo::Settings& gs) {
            gizmoFrame(from, false, ctrl, gs);
            gizmoFrame(from, false, ctrl, gs);
            gizmoFrame(from, true, ctrl, gs);
            for (int i = 1; i <= 4; ++i) gizmoFrame(from + (to - from) * (i / 4.0f), true, ctrl, gs);
            gizmoFrame(to, false, ctrl, gs);
            gizmoFrame(to, false, ctrl, gs);
        };
        auto near = [](float a, float b, float eps) { return std::abs(a - b) <= eps; };
        gizmo::Settings gs;   // Move, world axes, grid 1 m

        // An object with a child two metres above it.
        entities.clear();
        sel.clear();
        entities.push_back(makeBox(30, glm::vec3(0.0f)));
        {
            Entity child = makeBox(31, glm::vec3(0.0f, 2.0f, 0.0f));
            child.parent = 30;
            child.localCenter = glm::vec3(0.0f, 2.0f, 0.0f);
            entities.push_back(child);
        }
        scenegraph::resolve(entities);
        sel.select(30);
        {
            const unsigned rev = history.revision();
            const glm::vec2 c = screenOf(entities[0].center);
            gizmoFrame(c, false, false, gs);
            gizmoFrame(c, false, false, gs);
            gizmoFrame(c, true, false, gs);
            for (int i = 1; i <= 4; ++i) gizmoFrame(c + glm::vec2(25.0f * i, 0.0f), true, false, gs);
            const glm::vec3 held = entities[0].center;
            check(history.revision() == rev && drag.active, "a gizmo drag banks nothing while held");
            gizmoFrame(c + glm::vec2(100.0f, 0.0f), false, false, gs);
            gizmoFrame(c + glm::vec2(100.0f, 0.0f), false, false, gs);
            const glm::vec3 at = entities[0].center;
            check(at.x > 0.5f && near(at.y, 0.0f, 1e-3f) && near(at.z, 0.0f, 1e-3f) && at == held,
                  "dragging the gizmo's centre right moves the object right, and only that",
                  "x " + std::to_string(at.x).substr(0, 5));
            scenegraph::resolve(entities);
            check(near(entities[1].center.x, at.x, 1e-4f) && near(entities[1].center.y, 2.0f, 1e-4f),
                  "...its child comes along");
            check(history.revision() == rev + 1, "...and the drag is one undo step, banked after the release");
            history.undo(document);
            scenegraph::resolve(entities);
            check(entities[0].center == glm::vec3(0.0f) && near(entities[1].center.x, 0.0f, 1e-5f),
                  "one undo puts it back");
        }
        {
            // Ctrl on a world move: onto the grid, but only along what the drag moves.
            entities[0].center = entities[0].localCenter = glm::vec3(0.0f, 0.3f, 0.0f);
            scenegraph::resolve(entities);
            const glm::vec2 c = screenOf(entities[0].center);
            dragGizmo(c, c + glm::vec2(100.0f, 0.0f), true, gs);
            const glm::vec3 at = entities[0].center;
            check(at.x >= 1.0f && near(at.x, std::round(at.x), 1e-5f) && near(at.y, 0.3f, 1e-4f),
                  "Ctrl lands a world move on the grid, and leaves the axis it did not move alone",
                  "x " + std::to_string(at.x).substr(0, 5) + ", y " + std::to_string(at.y).substr(0, 5));
            history.undo(document);
        }
        {
            // Two roots selected: the other one takes the same step.
            entities.clear();
            entities.push_back(makeBox(40, glm::vec3(0.0f)));
            entities.push_back(makeBox(41, glm::vec3(0.0f, -3.0f, 0.0f)));
            sel.select(41);
            sel.toggle(40);   // 40 active, 41 along
            const glm::vec2 c = screenOf(entities[0].center);
            dragGizmo(c, c + glm::vec2(100.0f, 0.0f), false, gs);
            const float dx = entities[0].center.x;
            check(dx > 0.5f && near(entities[1].center.x, dx, 1e-4f) &&
                      near(entities[1].center.y, -3.0f, 1e-4f),
                  "with two roots selected the other one takes the same step");
            history.undo(document);
        }
        {
            // A face of a modelled box, picked and moved.
            entities.clear();
            sel.clear();
            {
                Entity e = makeBox(50, glm::vec3(0.0f));
                auto mc = std::make_unique<MeshComponent>();
                mc->mesh = EditMesh::box(e.half);
                e.components.items.push_back(std::move(mc));
                entities.push_back(std::move(e));
            }
            sel.select(50);
            auto mesh = [&]() -> const MeshComponent& {
                return *entities[0].components.get<MeshComponent>();
            };
            int front = -1, back = -1;   // the +Z face (towards the camera) and the -Z one
            for (int f = 0; f < static_cast<int>(mesh().mesh.faces.size()); ++f) {
                const std::vector<glm::vec3> w = meshFaceWorld(entities[0], mesh(), f);
                const glm::vec3 n = glm::normalize(glm::cross(w[1] - w[0], w[2] - w[0]));
                if (n.z > 0.9f) front = f;
                if (n.z < -0.9f) back = f;
            }
            modeltools::Selection msel;
            msel.faces = {front};
            faceSel    = front;
            faceOwner  = 50;
            gizmo::Settings fs = gs;
            fs.faceMode = true;
            fs.modelSel = &msel;
            auto centreOf = [](const std::vector<glm::vec3>& w) {
                glm::vec3 c(0.0f);
                for (const glm::vec3& p : w) c += p;
                return c / static_cast<float>(w.size());
            };
            const std::vector<glm::vec3> front0 = meshFaceWorld(entities[0], mesh(), front);
            const std::vector<glm::vec3> back0  = meshFaceWorld(entities[0], mesh(), back);
            const unsigned rev = history.revision();
            const glm::vec2 c = screenOf(centreOf(front0));
            dragGizmo(c, c + glm::vec2(100.0f, 0.0f), false, fs);
            const glm::vec3 moved = centreOf(meshFaceWorld(entities[0], mesh(), front)) - centreOf(front0);
            const std::vector<glm::vec3> back1 = meshFaceWorld(entities[0], mesh(), back);
            float backShift = 0.0f;
            for (std::size_t i = 0; i < back0.size() && i < back1.size(); ++i)
                backShift = std::max(backShift, glm::length(back1[i] - back0[i]));
            check(moved.x > 0.5f && near(moved.y, 0.0f, 1e-3f) && near(moved.z, 0.0f, 1e-3f),
                  "a picked face moves with the gizmo", "x " + std::to_string(moved.x).substr(0, 5));
            check(backShift < 1e-4f, "...and the rest of the mesh stays where it is in the world",
                  "back face moved " + std::to_string(backShift));
            check(history.revision() == rev + 1 && !drag.faceActive, "...as one undo step");
            history.undo(document);
            const glm::vec3 undone = centreOf(meshFaceWorld(entities[0], mesh(), front)) - centreOf(front0);
            check(glm::length(undone) < 1e-4f, "one undo puts the face back");

            // Scaled by its centre and held still: it stays at what the pointer says.
            fs.op = ImGuizmo::SCALE;
            auto width = [&] {
                const std::vector<glm::vec3> w = meshFaceWorld(entities[0], mesh(), front);
                float most = 0.0f;
                for (const glm::vec3& a : w)
                    for (const glm::vec3& b2 : w) most = std::max(most, glm::length(a - b2));
                return most;
            };
            const float w0 = width();
            const glm::vec2 s0 = screenOf(centreOf(front0));
            gizmoFrame(s0, false, false, fs);
            gizmoFrame(s0, false, false, fs);
            gizmoFrame(s0, true, false, fs);
            for (int i = 1; i <= 5; ++i) gizmoFrame(s0 + glm::vec2(10.0f * i, 0.0f), true, false, fs);
            std::vector<float> held;
            for (int i = 0; i < 4; ++i) {
                gizmoFrame(s0 + glm::vec2(50.0f, 0.0f), true, false, fs);
                held.push_back(width() / w0);
            }
            gizmoFrame(s0 + glm::vec2(50.0f, 0.0f), false, false, fs);
            gizmoFrame(s0 + glm::vec2(50.0f, 0.0f), false, false, fs);
            const float spread = *std::max_element(held.begin(), held.end()) -
                                 *std::min_element(held.begin(), held.end());
            check(near(held.back(), 1.5f, 0.02f) && spread < 1e-4f,
                  "a face scaled 50 px and held still stays at 1.5x -- it does not keep growing",
                  "x" + std::to_string(held.back()).substr(0, 5) + ", drift " + std::to_string(spread));
            history.undo(document);
            check(near(width() / w0, 1.0f, 1e-4f), "one undo takes the scale off");
        }
        sel.clear();
    }

    ImGui::DestroyContext();

    std::printf(failures ? "\neditorcheck: %d FAILED\n" : "\neditorcheck: all good\n", failures);
    return failures ? 1 : 0;
}
