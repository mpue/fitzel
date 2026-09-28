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
//   build/release/bin/editorcheck.exe

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/scene/Camera.hpp>

#include "../src/Command.hpp"
#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/EditorContext.hpp"
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

    std::printf(failures ? "\neditorcheck: %d FAILED\n" : "\neditorcheck: all good\n", failures);
    return failures ? 1 : 0;
}
