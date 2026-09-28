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
//     stroke is one undo step; an empty slot lays nothing down; the button
//     going to another tool mid-stroke banks the stroke it left open, and a
//     selection off the mesh makes it let go of the button. It runs inside a
//     real ImGui frame, just one nobody draws.
//   - the operations on objects (SceneOps): delete takes the subtree and
//     never the sun; a copy keeps its parent; the selection's copies hang off
//     each other's copies; one main camera; an Empty slid in above an object
//     leaves it where it was; a cockpit camera faces the nose; a vehicle gets
//     its four lights; a prefab instance unpacks -- each one undo step.
//   - the panels moved out of main: the Unity importer lists every .fbx below
//     a folder, any case, sorted; the Assets browser takes a file dropped on it
//     and leaves one dropped beside it alone.
//   - which tool has the left button (ViewTool): switching one on takes it
//     from the one that had it; switching off one that did not have it
//     changes nothing.
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
//   - the 3D cursor (Cursor3D): Shift+Right-click puts it on the ground under
//     the pointer, and neither a plain right-click nor one in Play does;
//     Shift+S opens the snap menu; the snap operations round it onto the grid,
//     drop it onto the terrain, fetch it from the selection, and move the
//     selection to it -- a parented object included, through its local transform
//     -- each move of the selection one undo step.
//   - the modelling mode (ModelMode): Make editable turns a solid into a mesh of
//     its size as one undo step; an edit moves what it moves and nothing else,
//     squares the object with it and is one undo step; a modal edit starts
//     from the base every frame, puts everything back when cancelled and is one
//     step when committed; a click picks the face under the pointer; the face
//     selection is dropped when the object changes; and G, driven by real key
//     and mouse events, grabs the face, Enter keeps it, Esc puts it back.
//   - the vehicle setup gizmo (VehicleGizmo), through the frame's ViewportFrame:
//     the front-axle handle is grabbed where it is drawn and follows the pointer
//     along its own axis, one undo bracket for the drag.
//   - the Scene window's corner read-outs (ViewportHud): the Play-as picker
//     keeps the pointer off the scene while it is on it, and picking an entry
//     sets the scene's start and marks the scene changed; Exit camera gives the
//     free camera back; the view's name shows only for a standard view or an
//     ortho lens. And ViewportFrame::looking puts the image's centre at NDC 0.
//   build/release/bin/editorcheck.exe

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/scene/Camera.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "../src/Command.hpp"
#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/EditorContext.hpp"
#include "../src/GroundBrush.hpp"
#include "../src/ViewportOverlay.hpp"
#include "../src/ViewportPick.hpp"
#include "../src/TransformGizmo.hpp"
#include "../src/Cursor3D.hpp"
#include "../src/ModelMode.hpp"
#include "../src/VehicleGizmo.hpp"
#include "../src/ViewportHud.hpp"
#include "../src/ViewTool.hpp"
#include "../src/SceneOps.hpp"
#include "../src/AssetsPanel.hpp"
#include "../src/UnityImportPanel.hpp"
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
    int         matSel = -1, faceOwner = -1, faceSel = -1, nextId = 1000;
    std::string status;
    EditorContext ed{document, entities, sel, history, materials, matSel, assetDb, models,
                     camera, status, faceOwner, faceSel, nextId,
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

    // --- Operations on objects ----------------------------------------------------------------
    {
        auto near3 = [](const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; };
        auto child = [&](int id, int parent, const glm::vec3& local) {
            Entity e = makeBox(id, local);
            e.parent = parent;
            e.localCenter = local;
            return e;
        };
        auto find = [&](int id) { return document.find(id); };
        auto revertAll = [&](unsigned rev) {
            while (history.revision() > rev && history.canUndo()) history.undo(document);
        };

        // A root at (10,0,0) with a child 2 m up; a sun beside them.
        entities.clear();
        sel.clear();
        entities.push_back(makeBox(1, glm::vec3(10.0f, 0.0f, 0.0f)));
        entities.push_back(child(2, 1, glm::vec3(0.0f, 2.0f, 0.0f)));
        {
            Entity sun = makeBox(3, glm::vec3(0.0f, 50.0f, 0.0f));
            sun.type = EntityType::Sun;
            entities.push_back(sun);
        }
        scenegraph::resolve(entities);
        const unsigned rev0 = history.revision();

        check(sceneops::isUnder(entities, 2, 1) && !sceneops::isUnder(entities, 1, 2),
              "isUnder: a child is under its parent, not the other way round");

        sceneops::deleteEntity(ed, 0);
        check(!find(1) && !find(2) && find(3) && history.revision() == rev0 + 1,
              "deleting an object takes its children with it, as one undo step");
        history.undo(document);
        check(find(1) && find(2), "...which one undo brings back");
        const unsigned rev1 = history.revision();
        sceneops::deleteEntity(ed, document.indexOf(3));
        check(find(3) && history.revision() == rev1, "the sun cannot be deleted");

        sceneops::duplicateEntity(ed, document.indexOf(2));
        const Entity* copy = find(nextId - 1);
        check(copy && copy->parent == 1 && near3(copy->localCenter, glm::vec3(2.2f, 2.0f, 0.0f)) &&
                  copy->name == "Box 2 copy" && sel.activeId() == copy->id,
              "a duplicate keeps its parent, sits beside the original in the parent's frame, and is selected");
        revertAll(rev1);

        // Two roots with a child each, both roots selected: all four go at once.
        entities.push_back(makeBox(4, glm::vec3(-10.0f, 0.0f, 0.0f)));
        entities.push_back(child(5, 4, glm::vec3(0.0f, 1.0f, 0.0f)));
        sel.select(1);
        sel.toggle(4);
        const unsigned rev2 = history.revision();
        sceneops::deleteSelection(ed);
        check(!find(1) && !find(2) && !find(4) && !find(5) && history.revision() == rev2 + 1,
              "deleting the selection takes every selected subtree, as one undo step");
        history.undo(document);

        // A parent and its child selected together: the child's copy hangs off
        // the parent's copy, not off the original.
        sel.select(1);
        sel.toggle(2);
        const int firstNew = nextId;
        sceneops::duplicateSelection(ed);
        const Entity* pc = find(firstNew);
        const Entity* cc = find(firstNew + 1);
        check(pc && cc && pc->parent == -1 && cc->parent == pc->id && sel.count() == 2 &&
                  sel.contains(pc->id) && sel.contains(cc->id),
              "duplicating a parent with its child: the child's copy hangs off the parent's copy");
        revertAll(rev2);

        // One main camera.
        auto withCam = [&](int id) {
            Entity e = makeBox(id, glm::vec3(0.0f));
            e.type = EntityType::Empty;
            e.components.items.push_back(std::make_unique<CameraComponent>());
            return e;
        };
        entities.push_back(withCam(6));
        entities.push_back(withCam(7));
        auto mainCams = [&] {
            std::vector<int> on;
            for (const Entity& e : entities)
                if (const auto* c = e.components.get<CameraComponent>(); c && c->activeOnStart)
                    on.push_back(e.id);
            return on;
        };
        sceneops::setMainCamera(ed, 6);
        const bool firstOn = mainCams() == std::vector<int>{6};
        sceneops::setMainCamera(ed, 7);
        const bool switched = mainCams() == std::vector<int>{7};
        const unsigned rev3 = history.revision();
        sceneops::setMainCamera(ed, 1);   // no camera on it
        const bool ignored = history.revision() == rev3 && mainCams() == std::vector<int>{7};
        sceneops::setMainCamera(ed, -1);
        check(firstOn && switched && ignored && mainCams().empty(),
              "exactly one main camera; an object without a camera is ignored; -1 clears them all");

        // An Empty slid in above the child: the child stays where it was.
        const glm::vec3 was = find(2)->center;
        sceneops::addEmptyParent(ed, document.indexOf(2));
        scenegraph::resolve(entities);
        const Entity* e2 = find(2);
        const Entity* emp = e2 ? find(e2->parent) : nullptr;
        check(emp && emp->type == EntityType::Empty && emp->parent == 1 && near3(e2->center, was) &&
                  sel.activeId() == emp->id,
              "an Empty slid in above an object keeps it where it was, and is selected");

        // A car, nose along +Z: the cockpit camera faces it; lights: four, under the car.
        {
            Entity car = makeBox(8, glm::vec3(0.0f, 0.0f, 20.0f));
            car.components.items.push_back(std::make_unique<VehicleComponent>());
            entities.push_back(std::move(car));
        }
        scenegraph::resolve(entities);
        sceneops::addCockpitCamera(ed, document.indexOf(8));
        const Entity* cock = find(sel.activeId());
        const bool facesNose = cock && cock->parent == 8 &&
                               std::abs(cock->localRotation.y - 180.0f) < 1e-4f &&
                               cock->localCenter.z > 0.0f;
        find(8)->components.get<VehicleComponent>()->forward = 1;   // built the other way round
        sceneops::addCockpitCamera(ed, document.indexOf(8));
        const Entity* cock2 = find(sel.activeId());
        check(facesNose && cock2 && std::abs(cock2->localRotation.y) < 1e-4f && cock2->localCenter.z < 0.0f,
              "a cockpit camera sits in the front of the craft and faces its nose, whichever way it was built");
        const unsigned rev4 = history.revision();
        sceneops::addVehicleLights(ed, document.indexOf(8));
        int lights = 0;
        for (const Entity& e : entities)
            if (e.parent == 8 && e.components.get<LightComponent>()) ++lights;
        const unsigned rev5 = history.revision();
        sceneops::addVehicleLights(ed, document.indexOf(1));   // no vehicle there
        check(lights == 4 && rev5 == rev4 + 1 && history.revision() == rev5,
              "a vehicle gets its four lights as one undo step; an object that is no vehicle gets none");

        // A prefab instance unpacked.
        {
            Entity inst = makeBox(9, glm::vec3(0.0f, 0.0f, -20.0f));
            auto pc2 = std::make_unique<PrefabComponent>();
            pc2->source = fitzel::AssetId::generate();
            inst.components.items.push_back(std::move(pc2));
            entities.push_back(std::move(inst));
        }
        const unsigned rev6 = history.revision();
        sceneops::unpackPrefab(ed, 9);
        const bool unpacked = !find(9)->components.get<PrefabComponent>() &&
                              history.revision() == rev6 + 1;
        history.undo(document);
        check(unpacked && find(9)->components.get<PrefabComponent>(),
              "a prefab instance unpacks to an ordinary object, as one undo step");
        revertAll(rev0);
        entities.clear();   // what was put in by hand rather than by a command
        sel.clear();
    }

    // --- Which tool has the left button --------------------------------------------------------
    {
        ViewTool tool = ViewTool::Road;
        takeTool(tool, ViewTool::Spline, true);
        const bool took = tool == ViewTool::Spline;
        takeTool(tool, ViewTool::Road, false);   // the road was not holding it
        const bool kept = tool == ViewTool::Spline;
        takeTool(tool, ViewTool::Spline, false);
        check(took && kept && tool == ViewTool::None,
              "a tool switched on takes the left button; one switched off that did not have it changes nothing");
    }

    // --- The frame a camera sees through an image -------------------------------------------
    {
        fitzel::Camera cam({0.0f, 0.0f, 10.0f});
        const ViewportFrame f = ViewportFrame::looking(cam, ImVec2(100.0f, 50.0f), 800.0f, 400.0f,
                                                       ImVec2(500.0f, 250.0f), true);
        const ViewportFrame corner = ViewportFrame::looking(cam, ImVec2(100.0f, 50.0f), 800.0f,
                                                            400.0f, ImVec2(100.0f, 50.0f), true);
        const glm::mat4 vp = cam.projectionMatrix(2.0f) * cam.viewMatrix();
        check(glm::length(f.mouseNdc) < 1e-6f && glm::length(corner.mouseNdc - glm::vec2(-1.0f, 1.0f)) < 1e-6f &&
                  f.viewProj == vp && f.cameraPos == cam.position() && f.orthoHalfH == 0.0f,
              "a frame from the camera: the image's centre is NDC 0, its top-left corner (-1, 1)");
    }

    // --- The viewport's metres per pixel ------------------------------------------------
    {
        ViewportFrame v;
        v.h         = 900.0f;
        v.cameraPos = glm::vec3(0.0f, 0.0f, 10.0f);
        v.cameraFov = 60.0f;
        const float persp = v.metresPerPixel(glm::vec3(0.0f));
        v.orthoHalfH = 5.0f;
        const float ortho = v.metresPerPixel(glm::vec3(0.0f, 0.0f, -90.0f));
        check(std::abs(persp - 2.0f * 10.0f * std::tan(glm::radians(30.0f)) / 900.0f) < 1e-7f &&
                  std::abs(ortho - 10.0f / 900.0f) < 1e-7f,
              "a pixel spans the right metres at a point's depth, and the same at any depth in ortho");
    }

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
    auto paintFrame = [&](bool lmb) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
        ImGui::NewFrame();
        ImGui::Begin("Scene");
        meshpaintui::brushViewport(ed, view, brush, paintMode, 1.0f / 60.0f);
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
        // The button goes to another tool mid-stroke: main still calls the
        // brush while its stroke is open, with its switch off, and the stroke
        // is banked, not lost.
        const unsigned rev = history.revision();
        for (int i = 0; i < 5; ++i) paintFrame(true);
        ViewTool tool = ViewTool::MeshPaint;
        takeTool(tool, ViewTool::Grass, true);
        paintMode = tool == ViewTool::MeshPaint;
        paintFrame(true);
        check(!paintMode && !brush.stroking && history.revision() == rev + 1,
              "the left button goes to another tool mid-stroke: the brush banks the stroke it left open");
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

    // --- The 3D cursor ---------------------------------------------------------------------
    {
        cursor3d::Cursor cur;
        ViewportFrame v = view;
        v.pickTerrain = [](glm::vec2 ndc, const glm::mat4&, glm::vec3& hit) {
            hit = glm::vec3(ndc.x * 20.0f, 1.5f, ndc.y * 20.0f);
            return true;
        };
        v.groundAt = [](float x, float z) { return 0.25f * x + 0.5f * z; };
        bool menuOpen = false;
        // One frame: Shift, the right button, S -- then the cursor's two calls in
        // the Scene window, as main makes them.
        auto cursorFrame = [&](glm::vec2 ndc, bool shift, bool rmb, bool sKey, bool editing) {
            v.mouseNdc = ndc;
            io.AddMousePosEvent(v.origin.x + (ndc.x * 0.5f + 0.5f) * v.w,
                                v.origin.y + (0.5f - ndc.y * 0.5f) * v.h);
            io.AddKeyEvent(ImGuiMod_Shift, shift);
            io.AddKeyEvent(ImGuiKey_S, sKey);
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, rmb);
            ImGui::NewFrame();
            ImGui::Begin("Scene");
            cursor3d::viewport(v, cur, editing);
            cursor3d::snapMenu(ed, v, cur, editing);
            menuOpen = ImGui::IsPopupOpen("##snapMenu");
            ImGui::End();
            ImGui::Render();
        };
        auto release = [&] { cursorFrame(glm::vec2(0.0f), false, false, false, true); };
        release();
        cursorFrame(glm::vec2(0.5f, -0.25f), true, true, false, true);
        release();
        check(cur.pos == glm::vec3(10.0f, 1.5f, -5.0f),
              "Shift+Right-click puts the cursor on the ground under the pointer");
        cursorFrame(glm::vec2(-0.5f, 0.5f), false, true, false, true);
        release();
        check(cur.pos == glm::vec3(10.0f, 1.5f, -5.0f), "...a plain right-click does not");
        cursorFrame(glm::vec2(-0.5f, 0.5f), true, true, false, false);
        release();
        check(cur.pos == glm::vec3(10.0f, 1.5f, -5.0f), "...nor one in Play");
        cursorFrame(glm::vec2(0.0f), true, false, true, true);
        check(menuOpen, "Shift+S opens the snap menu");
        ImGui::NewFrame();
        ImGui::Begin("Scene");
        if (ImGui::BeginPopup("##snapMenu")) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); }
        ImGui::End();
        ImGui::Render();
        release();

        cur.pos  = glm::vec3(1.4f, 2.6f, -0.2f);
        cur.grid = 0.5f;
        cursor3d::snap(cursor3d::Snap::CursorToGrid, ed, cur, v.groundAt);
        check(cur.pos == glm::vec3(1.5f, 2.5f, 0.0f), "Cursor to grid rounds it onto the grid step");
        cursor3d::snap(cursor3d::Snap::CursorToTerrain, ed, cur, v.groundAt);
        check(cur.pos == glm::vec3(1.5f, 0.375f, 0.0f), "Cursor to terrain drops it onto the ground");

        entities.clear();
        sel.clear();
        entities.push_back(makeBox(60, glm::vec3(0.0f)));
        {
            Entity child = makeBox(61, glm::vec3(0.0f));
            child.parent      = 60;
            child.localCenter = glm::vec3(0.0f, 1.0f, 0.0f);
            entities.push_back(child);
        }
        entities[0].rotation = entities[0].localRotation = glm::vec3(0.0f, 90.0f, 0.0f);
        scenegraph::resolve(entities);
        cursor3d::snap(cursor3d::Snap::CursorToSelection, ed, cur, v.groundAt);
        check(cur.pos == glm::vec3(1.5f, 0.375f, 0.0f), "Cursor to selection with nothing selected does nothing");
        sel.select(61);
        cursor3d::snap(cursor3d::Snap::CursorToSelection, ed, cur, v.groundAt);
        check(glm::length(cur.pos - entities[1].center) < 1e-5f, "Cursor to selection fetches it");
        cur.pos = glm::vec3(3.0f, 2.0f, -1.0f);
        const glm::vec3 childWas = entities[1].center;
        const unsigned  revWas   = history.revision();
        cursor3d::snap(cursor3d::Snap::SelectionToCursor, ed, cur, v.groundAt);
        scenegraph::resolve(entities);   // what main does every frame: world from local
        check(glm::length(entities[1].center - cur.pos) < 1e-4f && entities[1].parent == 60,
              "Selection to cursor moves a parented object there, and it stays parented",
              "at (" + std::to_string(entities[1].center.x).substr(0, 5) + ", " +
                  std::to_string(entities[1].center.y).substr(0, 5) + ", " +
                  std::to_string(entities[1].center.z).substr(0, 5) + ")");
        check(history.revision() == revWas + 1 &&
                  std::string(history.undoName()) == "Selection to cursor",
              "...as one undo step", history.undoName());
        history.undo(document);
        scenegraph::resolve(entities);
        check(glm::length(entities[1].center - childWas) < 1e-5f, "...which one undo takes back");
        cur.grid = 1.0f;
        entities[0].center = entities[0].localCenter = glm::vec3(0.3f, 0.6f, -1.7f);
        entities[0].rotation = entities[0].localRotation = glm::vec3(0.0f);
        sel.select(60);
        cursor3d::snap(cursor3d::Snap::SelectionToGrid, ed, cur, v.groundAt);
        check(entities[0].center == glm::vec3(0.0f, 1.0f, -2.0f), "Selection to grid rounds the object onto it");
        check(std::string(history.undoName()) == "Selection to grid", "...as one undo step",
              history.undoName());
        history.undo(document);
        check(entities[0].center == glm::vec3(0.3f, 0.6f, -1.7f), "...which one undo takes back");
        {
            const unsigned rev = history.revision();
            cursor3d::snap(cursor3d::Snap::SelectionToGrid, ed, cur, v.groundAt);
            history.undo(document);
            entities[0].center = entities[0].localCenter = glm::vec3(1.0f, 2.0f, 3.0f);
            const unsigned mid = history.revision();
            cursor3d::snap(cursor3d::Snap::SelectionToGrid, ed, cur, v.groundAt);
            check(rev + 1 <= mid && history.revision() == mid,
                  "a snap that moves nothing leaves no empty undo step");
        }
        sel.clear();
    }

    // --- The modelling mode -------------------------------------------------------------
    {
        auto near = [](float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; };
        entities.clear();
        sel.clear();
        faceSel = faceOwner = -1;
        entities.push_back(makeBox(70, glm::vec3(0.0f)));
        sel.select(70);
        unsigned rev = history.revision();
        modelmode::convertToMesh(ed);
        const MeshComponent* made = modelmode::selectedMesh(ed);
        glm::vec3 mn(0.0f), mx(0.0f);
        if (made) made->mesh.bounds(mn, mx);
        check(made && history.revision() == rev + 1 && glm::length(mn + glm::vec3(1.0f)) < 1e-5f &&
                  glm::length(mx - glm::vec3(1.0f)) < 1e-5f,
              "Make editable turns the selected solid into a mesh of its size, as one undo step");
        rev = history.revision();
        modelmode::convertToMesh(ed);
        check(history.revision() == rev, "...and once it is one, doing it again changes nothing");

        auto mesh = [&]() -> MeshComponent& { return *modelmode::selectedMesh(ed); };
        auto centre = [&](int f) {
            const std::vector<glm::vec3> w = meshFaceWorld(entities[0], mesh(), f);
            glm::vec3 c(0.0f);
            for (const glm::vec3& q : w) c += q;
            return w.empty() ? c : c / static_cast<float>(w.size());
        };
        auto faceToward = [&](const glm::vec3& dir) {
            for (int f = 0; f < static_cast<int>(mesh().mesh.faces.size()); ++f) {
                const std::vector<glm::vec3> w = meshFaceWorld(entities[0], mesh(), f);
                if (w.size() >= 3 &&
                    glm::dot(glm::normalize(glm::cross(w[1] - w[0], w[2] - w[0])), dir) > 0.9f)
                    return f;
            }
            return -1;
        };
        const int front = faceToward(glm::vec3(0.0f, 0.0f, 1.0f));
        const int back  = faceToward(glm::vec3(0.0f, 0.0f, -1.0f));
        // Moves the front face `d` metres out, in the mesh's own space.
        auto pull = [front](float d) {
            return [front, d](EditMesh& m) {
                const std::vector<int> vs(m.faces[front].begin(), m.faces[front].end());
                editmesh::transformVerts(m, vs, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, d)));
            };
        };

        rev = history.revision();
        modelmode::applyEdit(ed, [&](MeshComponent& mc) { pull(1.0f)(mc.mesh); return front; }, "Pull");
        check(near(centre(front).z, 2.0f) && near(centre(back).z, -1.0f) &&
                  near(entities[0].half.z, 1.5f) && faceSel == front,
              "an edit moves the face, leaves the rest of the mesh where it was, and squares the object with it",
              "front z " + std::to_string(centre(front).z).substr(0, 5) + ", half z " +
                  std::to_string(entities[0].half.z).substr(0, 5));
        check(history.revision() == rev + 1, "...as one undo step", history.undoName());
        history.undo(document);
        check(near(centre(front).z, 1.0f) && near(entities[0].half.z, 1.0f), "...which one undo takes back");

        modelmode::Session sess;
        rev = history.revision();
        modelmode::live(ed, sess, modelkeys::Live::Begin, nullptr, nullptr);
        modelmode::live(ed, sess, modelkeys::Live::Set, pull(1.0f), nullptr);
        modelmode::live(ed, sess, modelkeys::Live::Set, pull(2.0f), nullptr);
        check(near(centre(front).z, 3.0f) && history.revision() == rev,
              "a modal edit starts from the base every frame -- 1 m then 2 m is 2 m -- and banks nothing yet",
              "front z " + std::to_string(centre(front).z).substr(0, 5));
        modelmode::live(ed, sess, modelkeys::Live::Cancel, nullptr, nullptr);
        check(near(centre(front).z, 1.0f) && near(entities[0].half.z, 1.0f) && history.revision() == rev,
              "...cancelled, it puts the mesh back exactly");
        modelmode::live(ed, sess, modelkeys::Live::Begin, nullptr, nullptr);
        modelmode::live(ed, sess, modelkeys::Live::Set, pull(0.5f), nullptr);
        modelmode::live(ed, sess, modelkeys::Live::Commit, nullptr, "Grab");
        check(near(centre(front).z, 1.5f) && history.revision() == rev + 1,
              "...committed, it is one undo step");
        history.undo(document);

        // A click on the mesh picks the face under the pointer.
        auto screenOf = [&](const glm::vec3& w) {
            ImVec2 s;
            view.toScreen(w, s);
            return glm::vec2(s.x, s.y);
        };
        auto clickAt = [&](glm::vec2 px) {
            io.AddMousePosEvent(px.x, px.y);
            ImGui::NewFrame();
            ImGui::Begin("Scene");
            const bool took = modelmode::click(ed, view, sess);
            ImGui::End();
            ImGui::Render();
            return took;
        };
        faceSel = -1;
        sess.sel.clear();
        const bool tookFront = clickAt(screenOf(centre(front)));
        check(tookFront && faceSel == front, "a click on the mesh picks the face under the pointer");
        check(!clickAt(glm::vec2(20.0f, 20.0f)), "...and a click beside it is not the mesh's");

        faceSel   = front;
        faceOwner = 70;
        check(modelmode::keepFaceSelection(ed) && faceSel == front,
              "the face selection stays while the object does");
        entities.push_back(makeBox(71, glm::vec3(5.0f, 0.0f, 0.0f)));
        sel.select(71);
        modelmode::keepFaceSelection(ed);
        check(faceSel == -1 && faceOwner == 71, "...and is dropped when the selection moves on");
        sel.select(70);
        faceOwner = 70;
        faceSel   = 999;
        modelmode::keepFaceSelection(ed);
        check(faceSel == -1, "...or when the mesh has no such face any more");

        // G, Enter and Esc as the keyboard sends them.
        sel.select(70);
        faceSel   = front;
        faceOwner = 70;
        sess.sel  = modeltools::Selection{};
        sess.sel.faces = {front};
        modelmode::ViewportHost mh;
        auto keyFrame = [&](glm::vec2 px, ImGuiKey k, bool down) {
            io.AddMousePosEvent(px.x, px.y);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
            ImGui::NewFrame();
            ImGui::Begin("Scene");
            modelmode::viewport(ed, view, sess, mh);
            ImGui::End();
            ImGui::Render();
        };
        auto press = [&](glm::vec2 px, ImGuiKey k) { keyFrame(px, k, true); keyFrame(px, k, false); };
        const glm::vec2 c0 = screenOf(centre(front));
        rev = history.revision();
        keyFrame(c0, ImGuiKey_None, false);
        press(c0, ImGuiKey_G);
        for (int i = 1; i <= 4; ++i) keyFrame(c0 + glm::vec2(15.0f * i, 0.0f), ImGuiKey_None, false);
        const float grabbed = centre(front).x;
        keyFrame(c0 + glm::vec2(60.0f, 0.0f), ImGuiKey_None, false);
        keyFrame(c0 + glm::vec2(60.0f, 0.0f), ImGuiKey_None, false);
        check(grabbed > 0.3f && near(centre(front).x, grabbed, 1e-5f) && near(centre(back).x, 0.0f) &&
                  history.revision() == rev && modelkeys::busy(),
              "G grabs the picked face, which follows the pointer and holds still with it, banking nothing yet",
              "x " + std::to_string(grabbed).substr(0, 5));
        press(c0 + glm::vec2(60.0f, 0.0f), ImGuiKey_Enter);
        check(!modelkeys::busy() && history.revision() == rev + 1 && near(centre(front).x, grabbed, 1e-5f),
              "...Enter keeps it, as one undo step");
        history.undo(document);
        rev = history.revision();
        keyFrame(c0, ImGuiKey_None, false);
        press(c0, ImGuiKey_G);
        keyFrame(c0 + glm::vec2(40.0f, 0.0f), ImGuiKey_None, false);
        press(c0 + glm::vec2(40.0f, 0.0f), ImGuiKey_Escape);
        check(!modelkeys::busy() && near(centre(front).x, 0.0f) && history.revision() == rev,
              "...Esc puts it back and banks nothing");
        sel.clear();
    }

    // --- The vehicle setup gizmo -----------------------------------------------------------
    {
        auto near = [](float a, float b, float eps) { return std::abs(a - b) <= eps; };
        VehicleComponent vc;   // its defaults: a car-sized setup, nose along +Z
        // Looking at the car from up and to the side, so the axles run across
        // the picture and the three handles on the front axle stand apart.
        const glm::vec3 eye(8.0f, 8.0f, 0.0f);
        ViewportFrame v = view;
        v.viewProj  = camera.projectionMatrix(16.0f / 9.0f) *
                      glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        v.cameraPos = eye;
        int  vsel = vehiclegizmo::kNone;
        bool vdrag = false;
        int  opened = 0;
        std::vector<std::string> closed;
        vehiclegizmo::Context gc{vc, glm::mat4(1.0f), vsel, vdrag};
        gc.editable  = true;
        gc.beginEdit = [&] { ++opened; };
        gc.endEdit   = [&](const char* label) { closed.push_back(label); };
        gc.editOpen  = [] { return false; };
        auto px = [&](const glm::vec3& w) {
            ImVec2 s;
            v.toScreen(w, s);
            return glm::vec2(s.x, s.y);
        };
        auto vframe = [&](glm::vec2 at, bool lmb) {
            gc.view          = v;
            gc.view.mousePos = ImVec2(at.x, at.y);
            gc.view.mouseNdc = glm::vec2(at.x / v.w * 2.0f - 1.0f, 1.0f - at.y / v.h * 2.0f);
            io.AddMousePosEvent(at.x, at.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
            ImGui::NewFrame();
            ImGui::Begin("Scene");
            vehiclegizmo::handle(gc);
            ImGui::End();
            ImGui::Render();
        };
        const float z0 = vc.frontZ;
        const glm::vec2 grip = px(glm::vec3(0.0f, vc.wheelY, z0));
        vframe(grip, false);
        vframe(grip, true);
        check(vsel == vehiclegizmo::kFrontZ && vdrag && opened == 1,
              "the vehicle gizmo's front-axle handle is grabbed where it is drawn");
        const glm::vec2 ahead = px(glm::vec3(0.0f, vc.wheelY, z0 + 0.5f));
        vframe(ahead, true);
        vframe(ahead, false);
        check(near(vc.frontZ, z0 + 0.5f, 1e-3f) && !vdrag && closed.size() == 1,
              "...follows the pointer along its axis, and the drag is one undo bracket",
              "front axle " + std::to_string(z0).substr(0, 5) + " -> " +
                  std::to_string(vc.frontZ).substr(0, 5));
    }

    // --- The Scene window's corner read-outs ---------------------------------------------
    {
        // The Scene window pinned over the display, so the pointer is over it.
        const ImVec2 vmin(0.0f, 0.0f), vmax(1600.0f, 900.0f);
        auto hudFrame = [&](glm::vec2 at, bool lmb, const std::function<void()>& body) {
            io.AddMousePosEvent(at.x, at.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(vmin);
            ImGui::SetNextWindowSize(ImVec2(vmax.x - vmin.x, vmax.y - vmin.y));
            ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings);
            body();
            ImGui::End();
            ImGui::Render();
        };
        int  startMode = 2;
        bool onPicker  = false;
        auto picker = [&] { onPicker = viewhud::playAsPicker(vmin, vmax, startMode, history); };
        // The picker is 230 px wide, 12 px in from the right, 10 down.
        const glm::vec2 onIt(vmax.x - 12.0f - 115.0f, vmin.y + 18.0f);
        const glm::vec2 middle(800.0f, 450.0f);
        hudFrame(middle, false, picker);
        hudFrame(middle, false, picker);
        const bool offIt = !onPicker;
        hudFrame(onIt, false, picker);
        hudFrame(onIt, false, picker);
        check(offIt && onPicker, "the Play-as picker has the pointer while it is on it, and only then");
        // Open it and take the first entry, "The game's own start".
        const unsigned rev = history.revision();
        hudFrame(onIt, true, picker);
        hudFrame(onIt, false, picker);
        const float rowY = vmin.y + 10.0f + ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y +
                           ImGui::GetFontSize() * 0.5f;
        const glm::vec2 first(onIt.x - 60.0f, rowY);
        hudFrame(first, false, picker);
        hudFrame(first, true, picker);
        hudFrame(first, false, picker);
        check(startMode == -1 && history.revision() == rev + 1,
              "...picking an entry sets the scene's start and marks the scene changed",
              "start " + std::to_string(startMode));

        int  activeCam = 7;
        bool onExit    = false;
        auto exitBtn = [&] { onExit = viewhud::exitCamera(vmin, "Chase cam", activeCam); };
        const glm::vec2 btn(vmin.x + 24.0f, vmin.y + 38.0f);
        hudFrame(btn, false, exitBtn);
        hudFrame(btn, false, exitBtn);
        const bool hoverSeen = onExit;
        hudFrame(btn, true, exitBtn);
        hudFrame(btn, false, exitBtn);
        check(hoverSeen && activeCam == -1, "Exit camera says the pointer is on it, and gives the free camera back");

        auto drawnBy = [&](const std::function<void()>& draw) {
            int added = 0;
            hudFrame(middle, false, [&] {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const int before = dl->VtxBuffer.Size;
                draw();
                added = dl->VtxBuffer.Size - before;
            });
            return added;
        };
        check(drawnBy([&] { viewhud::viewLabel(vmin, "Front", false); }) > 0 &&
                  drawnBy([&] { viewhud::viewLabel(vmin, nullptr, true); }) > 0 &&
                  drawnBy([&] { viewhud::viewLabel(vmin, nullptr, false); }) == 0,
              "the view's name shows for a standard view or an ortho lens, and not for a free view");
        check(drawnBy([&] { viewhud::traceStatus(vmin, ""); }) == 0 &&
                  drawnBy([&] { viewhud::traceStatus(vmin, "Waiting for the view to settle"); }) > 0,
              "the tracer's status shows only when there is one");
    }

    // --- Panels moved out of main -------------------------------------------------------------
    {
        const fs::path root = dir / "unity";
        fs::create_directories(root / "Pack" / "Meshes");
        fs::create_directories(root / "Pack" / "Textures");
        auto touch = [](const fs::path& f) { std::ofstream(f) << "x"; };
        touch(root / "Pack" / "Meshes" / "Rock.FBX");
        touch(root / "Pack" / "Meshes" / "Tree.fbx");
        touch(root / "Pack" / "Textures" / "Rock_Albedo.png");
        touch(root / "Loose.fbx");
        const auto found = unityimportui::scanFbx(root.generic_string());
        std::string got;
        for (const auto& f : found) got += f.first + " ";
        check(found.size() == 3 && found[0].first == "Loose.fbx" &&
                  found[1].first == "Pack/Meshes/Rock.FBX" && found[2].first == "Pack/Meshes/Tree.fbx",
              "the Unity importer lists every .fbx below the folder, any case, sorted", got);

        assetsui::State as;
        bool showAssets = true;
        const std::string noProject;
        std::vector<std::string> dropped{(dir / "some.png").generic_string()};
        auto assetsFrame = [&](float dx, float dy) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(100.0f, 100.0f));
            ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
            assetsui::panel(ed, as, {showAssets, noProject, nullptr, dropped, dx, dy, nullptr});
            ImGui::Render();
        };
        assetsFrame(50.0f, 50.0f);   // beside the window
        const bool leftAlone = dropped.size() == 1 && as.dropStatus.empty();
        assetsFrame(300.0f, 250.0f); // on it
        check(leftAlone && dropped.empty() && !as.dropStatus.empty(),
              "the Assets browser leaves a file dropped beside it alone, and takes one dropped on it",
              as.dropStatus);
    }

    ImGui::DestroyContext();

    std::printf(failures ? "\neditorcheck: %d FAILED\n" : "\neditorcheck: all good\n", failures);
    return failures ? 1 : 0;
}
