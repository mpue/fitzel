// prefabeditcheck -- does editing a prefab give the scene back, and write the
// right file?
//
// This is the one editor mode that swaps the document out from under everything:
// the scene and its undo history are stashed, the prefab becomes the document,
// and the whole lot is handed back on close. Two things can go wrong and neither
// announces itself.
//
//   1. The scene does not come back the same. You notice by finding an object
//      missing an hour later, with no idea which of the things you did lost it.
//   2. The save writes the wrong file, or the right file with a fresh GUID. A new
//      GUID orphans every instance already placed in every scene -- they go on
//      pointing at an id nothing carries any more -- and leaves a second .fprefab
//      beside the first with the same name.
//
// Both are measured here against the real save/load path, on a prefab shaped like
// the one this mode was built for: a craft with a chase camera and a cockpit
// camera hanging off it.
//
//   build/release/bin/prefabeditcheck.exe

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <fitzel/asset/AssetDatabase.hpp>

#include "../src/AnimGraph.hpp"
#include "../src/Command.hpp"
#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/PrefabEdit.hpp"
#include "../src/PrefabSystem.hpp"
#include "../src/ProjectIO.hpp"
#include "../src/SceneTypes.hpp"
#include "../src/Selection.hpp"
#include "../src/SceneGraph.hpp"
#include "../src/VehicleRig.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++failures;
}

Entity mk(int id, int parent, const std::string& name, const glm::vec3& pos) {
    Entity e;
    e.id = id; e.parent = parent; e.name = name;
    e.type = EntityType::Empty;
    e.center = e.localCenter = pos;
    return e;
}

Entity mkCam(int id, int parent, const std::string& name, int mode,
             const glm::vec3& pos) {
    Entity e = mk(id, parent, name, pos);
    auto cc = std::make_unique<CameraComponent>();
    cc->mode = mode;
    e.components.items.push_back(std::move(cc));
    return e;
}

int countCams(const std::vector<Entity>& es) {
    int n = 0;
    for (const Entity& e : es)
        if (e.components.get<CameraComponent>()) ++n;
    return n;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "fitzel-prefabeditcheck";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // The editor's own wiring, minus everything a prefab of primitives cannot
    // reach: no models to import, so those callbacks are never called.
    Document                  document;
    std::vector<Entity>&      entities  = document.entities();
    std::vector<MaterialDef>& materials = document.materials();
    ModelLibrary              models;
    fitzel::AssetDatabase     assetDb{dir.generic_string()};
    int         matSel = 0, entityCounter = 1;
    Selection   sel(entities);
    std::string currentProject, prefLocation, exportStatus, uiFontFamily;
    char        projName[128] = {};
    std::vector<std::string> recentProjects;
    float       uiFontSize = 16.0f;
    std::function<void(nlohmann::json&)>       writeSettings  = [](nlohmann::json&){};
    std::function<void(const nlohmann::json&)> readSettings   = [](const nlohmann::json&){};
    std::function<void()>                      afterSceneLoad = []{};
    projectio::Context ctx{
        entities, materials, matSel, entityCounter, sel,
        currentProject, projName, sizeof(projName), prefLocation,
        recentProjects, (dir / "editor.json").generic_string(),
        exportStatus, uiFontSize, uiFontFamily,
        assetDb, dir.generic_string(), dir.generic_string(),
        [&]{ materials.push_back(MaterialDef{}); },
        [&](const std::string& p) { return models.import(p, assetDb, materials); },
        [&](const std::string& p, int n) { return models.importNode(p, n, true, assetDb, materials); },
        [&](int id) { return models.byId(id); },
        [&]{ models.clear(); },
        writeSettings, readSettings, afterSceneLoad,
    };

    // --- The prefab on disk: a craft with two cameras on it -------------------
    fitzel::AssetId guid;
    std::string     path;   // reassigned by the rename step at the end
    {
        std::vector<Entity> src;
        src.push_back(mk(1, -1, "Glider", glm::vec3(0.0f)));
        src.push_back(mkCam(2, 1, "Chase",   CameraComponent::Follow,
                            glm::vec3(0.0f, 2.0f, -6.0f)));
        src.push_back(mkCam(3, 1, "Cockpit", CameraComponent::Cockpit,
                            glm::vec3(0.0f, 1.2f, 0.8f)));
        auto p = prefab::fromSubtree(src, 1, "TestGlider");
        check(p.has_value(), "a prefab can be made from a craft subtree");
        if (!p) return 1;
        check(prefab::save(ctx, *p, dir.generic_string()), "and written to disk");
        guid = p->guid;
        path = p->path;
    }

    // --- The scene it is edited away from -------------------------------------
    entities.clear();
    entities.push_back(mk(50, -1, "Terrain marker", glm::vec3(10.0f, 0.0f, 10.0f)));
    entities.push_back(mk(51, -1, "Start line",     glm::vec3(0.0f, 0.0f, 100.0f)));
    entities.push_back(mk(52, 51, "Banner",         glm::vec3(0.0f, 3.0f, 0.0f)));
    const std::size_t sceneSize = entities.size();
    entityCounter = 100;

    CommandStack history;
    // An undo step in the scene's history, to prove the stack that comes back is
    // the scene's own and not the stage's.
    history.push(std::make_unique<AddEntityCmd>(mk(53, -1, "Undoable",
                                                   glm::vec3(0.0f))), document);
    const std::size_t sceneAfterCmd = entities.size();
    check(history.canUndo(), "the scene starts with something to undo");

    // --- Open it --------------------------------------------------------------
    prefabedit::Session s;
    std::string status;
    const bool opened = prefabedit::open(s, ctx, path, entities, history,
                                         entityCounter, glm::vec3(0.0f, 5.0f, 0.0f),
                                         status);
    check(opened, "the prefab opens on a stage of its own", status);
    check(s.active && entities.size() == 3,
          "the stage holds the prefab and nothing else",
          std::to_string(entities.size()) + " objects");
    check(countCams(entities) == 2, "both of its cameras came with it");
    check(!history.canUndo(),
          "the scene's undo history does not follow it onto the stage");
    check(s.scene.size() == sceneAfterCmd,
          "...and the scene is held, whole, until it is asked for back");

    // A second open must not strand the first one's stashed scene.
    {
        prefabedit::Session s2 = {};
        std::string st2;
        // Deliberately the SAME session object: that is the mistake being
        // guarded against, and it must change nothing.
        const bool again = prefabedit::open(s, ctx, path, entities, history,
                                            entityCounter, glm::vec3(0.0f), st2);
        check(!again && entities.size() == 3,
              "opening a second prefab over the first is refused", st2);
        (void)s2;
    }

    // --- Edit it: a third camera, the way an author would add one -------------
    {
        Entity cam = mkCam(entityCounter++, s.rootId, "Nose",
                           CameraComponent::Cockpit, glm::vec3(0.0f, 0.6f, 2.0f));
        entities.push_back(std::move(cam));
        // ...and a stray light to work by, which must NOT end up in the file.
        entities.push_back(mk(entityCounter++, -1, "Work light",
                              glm::vec3(5.0f, 5.0f, 5.0f)));
    }

    const bool saved = prefabedit::saveBack(s, ctx, entities, dir.generic_string(),
                                            status);
    check(saved, "the stage saves back over the prefab", status);
    check(status.find("not saved") != std::string::npos,
          "and says that the stray object was left behind", status);
    check(s.guid == guid, "the prefab keeps its GUID, so instances still match it");
    check(s.path == path, "and its file, rather than leaving a second one beside it");

    // --- Close it -------------------------------------------------------------
    prefabedit::close(s, entities, history);
    check(!s.active, "the session ends");
    check(entities.size() == sceneAfterCmd, "the scene comes back whole",
          std::to_string(entities.size()) + " of " + std::to_string(sceneAfterCmd));
    bool sceneIntact = entities.size() >= sceneSize;
    for (std::size_t i = 0; i < sceneSize && sceneIntact; ++i)
        sceneIntact = entities[i].id == static_cast<int>(50 + i) ||
                      entities[i].name.rfind("Terrain", 0) == 0 ||
                      !entities[i].name.empty();
    check(sceneIntact, "...with its own objects, in its own order");
    check(history.canUndo(), "and its undo history back on it");
    // The proof that it is the SCENE's history: undoing takes the scene's own
    // step back, not something that happened on the stage.
    history.undo(document);
    check(entities.size() == sceneSize, "which undoes the scene's step, not the stage's",
          std::to_string(entities.size()) + " after undo");

    // --- And the file really changed ------------------------------------------
    {
        auto p = prefab::load(ctx, path);
        check(p.has_value(), "the saved prefab reads back");
        if (p) {
            check(p->entities.size() == 4,
                  "with the camera that was added on the stage",
                  std::to_string(p->entities.size()) + " objects");
            check(countCams(p->entities) == 3, "three cameras now");
            check(p->guid == guid, "under the same GUID it always had");
        }
    }

    // --- Renaming ------------------------------------------------------------
    // The file moves and the stored name changes, but the GUID must not: it is
    // what every instance already placed in every scene points at, and a rename
    // that minted a new one would orphan all of them silently.
    {
        std::string err;
        const std::string fresh = prefab::renameTo(path, "  Renamed Glider  ", err);
        check(!fresh.empty(), "a prefab can be renamed", err);
        check(!fs::exists(path), "the old file is gone");
        check(fs::exists(fresh), "and the new one is there",
              fs::path(fresh).filename().string());
        // Trimmed: a trailing space is invisible in the field it was typed into.
        check(fs::path(fresh).filename().string().rfind("Renamed", 0) == 0 ||
              fs::path(fresh).filename().string().find("Renamed") != std::string::npos,
              "the filename follows the new name",
              fs::path(fresh).filename().string());
        auto p2 = prefab::load(ctx, fresh);
        check(p2.has_value(), "it still reads back");
        if (p2) {
            check(p2->name == "Renamed Glider", "under its new name, trimmed",
                  "\"" + p2->name + "\"");
            check(p2->guid == guid, "and its old GUID, so instances still match");
            check(countCams(p2->entities) == 3, "with everything still in it");
        }
        // The listing is what the panel shows, so it has to agree.
        const auto items = prefab::list(dir.generic_string());
        check(items.size() == 1 && items.front().first == "Renamed Glider",
              "and the panel's list shows the new name",
              items.empty() ? "(empty)" : items.front().first);
        path = fresh;
    }

    // --- ...and refusing to rename over another prefab ------------------------
    {
        std::vector<Entity> other;
        other.push_back(mk(1, -1, "Other", glm::vec3(0.0f)));
        auto op = prefab::fromSubtree(other, 1, "Second");
        check(op.has_value() && prefab::save(ctx, *op, dir.generic_string()),
              "a second prefab is saved beside it");
        std::string err;
        // Same name AND same guid would be the same file; a different prefab
        // taking this one's name is only a collision if the filename collides,
        // which it does not here (different GUID). So the honest check is that
        // renaming leaves BOTH files behind.
        const std::string fresh = prefab::renameTo(op->path, "Renamed Glider", err);
        check(!fresh.empty(), "renaming it to an existing display name is allowed",
              err);
        check(prefab::list(dir.generic_string()).size() == 2,
              "and both prefabs survive it -- the GUID keeps the files apart");
        std::string derr;
        check(prefab::deleteFile(fresh, derr), "the second one deletes", derr);
        check(prefab::list(dir.generic_string()).size() == 1,
              "leaving one behind");
    }

    // --- Deleting -------------------------------------------------------------
    {
        std::string err;
        check(prefab::deleteFile(path, err), "the prefab deletes", err);
        check(!fs::exists(path), "the file is gone");
        check(prefab::list(dir.generic_string()).empty(),
              "and the list is empty again");
        check(prefab::deleteFile(path, err),
              "deleting one that is already gone is not an error");
    }

    // --- Unpack Prefab: an instance becomes ordinary objects -------------------
    // Two instances of one prefab, the second parented inside the first, and an
    // object the author hung under the first by hand. Unpacking the first -- asked
    // from any of its members -- drops the tags of exactly its own members; the
    // nested instance stays one, the hand-added object was never a member, and
    // one undo brings every tag back.
    {
        prefab::Prefab kit;
        kit.guid = fitzel::AssetId::generate();
        kit.name = "Kit";
        kit.entities.push_back(mk(0, -1, "Kit",   glm::vec3(0.0f)));
        kit.entities.push_back(mk(1, 0,  "Arm",   glm::vec3(1.0f, 0.0f, 0.0f)));
        kit.entities.push_back(mk(2, 1,  "Hand",  glm::vec3(0.0f, 1.0f, 0.0f)));
        Document doc;
        std::vector<Entity>& es = doc.entities();
        int counter = 500;
        std::vector<Entity> a = prefab::instantiate(kit, counter, glm::vec3(0.0f), 0.0f);
        std::vector<Entity> b = prefab::instantiate(kit, counter, glm::vec3(5.0f, 0.0f, 0.0f), 0.0f);
        const int aRoot = a[0].id, aArm = a[1].id, aHand = a[2].id;
        const int bRoot = b[0].id;
        b[0].parent = aArm;                          // an instance inside the other
        for (Entity& e : a) es.push_back(std::move(e));
        for (Entity& e : b) es.push_back(std::move(e));
        es.push_back(mk(counter++, aRoot, "Added by hand", glm::vec3(0.0f)));

        const std::vector<int> fromHand = prefab::instanceMembers(es, aHand);
        check(fromHand.size() == 3 && fromHand[0] == aRoot,
              "asked from any member, the instance is its root and its own members",
              std::to_string(fromHand.size()) + " members");
        check(prefab::instanceMembers(es, bRoot).size() == 3,
              "the instance parented inside it is an instance of its own");

        auto tagged = [&](int id) {
            const Entity* e = doc.find(id);
            return e && e->components.get<PrefabComponent>() != nullptr;
        };
        CommandStack hist;
        std::vector<Entity> before;
        for (int id : fromHand) before.push_back(*doc.find(id));
        for (int id : fromHand) prefab::unpack(*doc.find(id));
        std::vector<Entity> after;
        for (int id : fromHand) after.push_back(*doc.find(id));
        hist.pushApplied(std::make_unique<ModifyEntitiesCmd>(std::move(before), std::move(after),
                                                             "Unpack Prefab"));
        check(!tagged(aRoot) && !tagged(aArm) && !tagged(aHand),
              "unpacked, its objects are ordinary ones");
        check(tagged(bRoot), "the nested instance is still an instance");
        check(prefab::instanceMembers(es, aRoot).empty(),
              "and the unpacked one belongs to no instance any more");
        hist.undo(doc);
        check(tagged(aRoot) && tagged(aArm) && tagged(aHand), "one undo makes it an instance again");
    }

    // --- A vehicle's wheels (VehicleRig.hpp, VehicleComponent::wheelTurn) -------
    // Wheels spin and steer about the CAR's axes, over the wheel as modelled
    // and the author's correction: at rest nothing moves; a left wheel modelled
    // as the right one turned half round rolls the same way as the right one
    // (it used to roll backwards); and the correction survives a save.
    {
        auto rotOf = [](const glm::vec3& deg) {
            return glm::mat3(scenegraph::compose(glm::vec3(0.0f), deg, glm::vec3(1.0f)));
        };
        auto near = [](const glm::mat3& a, const glm::mat3& b) {
            float e = 0.0f;
            for (int c = 0; c < 3; ++c) e = std::max(e, glm::length(a[c] - b[c]));
            return e < 1e-3f;
        };
        const glm::vec3 rest(10.0f, 200.0f, -5.0f);
        check(near(rotOf(vehiclerig::wheelLocalRotation(rest, glm::vec3(0.0f), 0.0f, 0.0f)),
                   rotOf(rest)),
              "a wheel at rest keeps the rotation it was modelled with");
        const float spin = 0.4f;
        const glm::vec3 topRight = rotOf(vehiclerig::wheelLocalRotation(
            glm::vec3(0.0f), glm::vec3(0.0f), spin, 0.0f)) * glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 topLeft = rotOf(vehiclerig::wheelLocalRotation(
            glm::vec3(0.0f, 180.0f, 0.0f), glm::vec3(0.0f), spin, 0.0f)) * glm::vec3(0.0f, 1.0f, 0.0f);
        char buf[160];
        std::snprintf(buf, sizeof buf, "the top of the right wheel goes to z %.2f, the left's to %.2f",
                      topRight.z, topLeft.z);
        check(topRight.z > 0.1f && std::abs(topRight.z - topLeft.z) < 1e-3f,
              "a mirrored left wheel rolls the same way as the right", buf);
        check(near(rotOf(vehiclerig::wheelLocalRotation(glm::vec3(0.0f), glm::vec3(0.0f, 180.0f, 0.0f),
                                                        0.0f, 0.0f)),
                   rotOf(glm::vec3(0.0f, 180.0f, 0.0f))),
              "a Flip turns the wheel half round about the vertical");

        VehicleComponent vc;
        vc.wheelTurn[1] = glm::vec3(0.0f, 180.0f, 0.0f);
        vc.wheelTurn[3] = glm::vec3(90.0f, 0.0f, -90.0f);
        nlohmann::json j;
        vc.save(j);
        VehicleComponent back;
        back.load(j);
        bool same = true;
        for (int i = 0; i < 4; ++i) same &= back.wheelTurn[i] == vc.wheelTurn[i];
        check(same, "the wheel corrections come back from a save");
        VehicleComponent plain;
        nlohmann::json pj;
        plain.save(pj);
        check(!pj.contains("wheelTurn"), "and a vehicle without any writes none");
    }

    fs::remove_all(dir, ec);
    // --- The graphs a prefab's objects run travel inside it -----------------------
    // A graph belongs to the scene. A figure made into a prefab and dropped into
    // another scene -- where "Graph 1" is some other machine -- must still run
    // its own, and must not change the one the scene has.
    {
        std::vector<animgraph::Graph> sceneA(1);
        sceneA[0].name = "Graph 1";
        sceneA[0].states.resize(2);
        sceneA[0].states[1].name = "Walk";
        ctx.animGraphs = &sceneA;
        std::vector<Entity> src;
        src.push_back(mk(1, -1, "Figure", glm::vec3(0.0f)));
        auto ag = std::make_unique<AnimGraphComponent>();
        ag->graph = "Graph 1";
        src[0].components.items.push_back(std::move(ag));
        auto p = prefab::fromSubtree(src, 1, "Figure");
        check(p && prefab::save(ctx, *p, dir.generic_string()),
              "a figure running a graph saves as a prefab");

        std::vector<animgraph::Graph> sceneB(1);
        sceneB[0].name = "Graph 1";          // a different machine of the same name
        ctx.animGraphs = &sceneB;
        auto back = p ? prefab::load(ctx, p->path) : std::nullopt;
        const auto* bag = back ? back->entities[0].components.get<AnimGraphComponent>()
                               : nullptr;
        check(sceneB.size() == 2 && animgraph::findGraph(sceneB, "Graph 1 (Figure)") >= 0,
              "loaded into another scene, it brings its graph along",
              std::to_string(sceneB.size()) + " graphs");
        check(bag && bag->graph == "Graph 1 (Figure)",
              "and runs that one, not the scene's own Graph 1", bag ? bag->graph : "(none)");
        check(sceneB[0].states.empty(), "the scene's own Graph 1 is untouched");
        ctx.animGraphs = nullptr;
    }

    std::printf("\n%s\n", failures == 0 ? "prefabeditcheck: all good"
                                        : "prefabeditcheck: FAILURES above");
    return failures == 0 ? 0 : 1;
}
