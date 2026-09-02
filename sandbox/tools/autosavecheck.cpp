// The autosave check: does the crash snapshot survive being written, and does
// the scene come back out of it intact?
//
// This is the one subsystem whose failure is silent by construction. A snapshot
// that is never written, is written half, or comes back with its material links
// broken looks exactly like a working one right up to the moment somebody needs
// it -- and at that moment the work is already gone. Nobody notices an autosave
// that does nothing, so it has to be measured instead of trusted.
//
// Two halves, matching the two things that can go wrong:
//   1. the writer's timing and its files -- when it snapshots, when it stays
//      quiet, what a half-written or failed snapshot leaves behind, and whether
//      what it leaves is recognised at the next start;
//   2. the round trip -- a scene written by saveSceneWithMaterials and read back
//      has to bring its entities AND its material library with it, GUIDs and
//      all, because an entity references its material by GUID and a fresh one
//      would land every object on the default grey.
//
// Console program, like shadercheck and citycheck, and for the same reason: the
// editor is /SUBSYSTEM:WINDOWS in Release and has nowhere to print to.
//   build/release/bin/autosavecheck.exe [workDir]
// Exits non-zero if any check fails.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <fitzel/asset/AssetDatabase.hpp>

#include "../src/Autosave.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/ProjectIO.hpp"
#include "../src/SceneTypes.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// --- 1. The writer ----------------------------------------------------------
// Time is a parameter of tick(), not something it reads, so a whole afternoon of
// editing runs here in a few microseconds and every boundary is exact.

void checkWriter(const fs::path& work) {
    std::printf("\nSnapshot writer\n");

    const fs::path dir     = work / "recovery";
    const fs::path project = work / "proj";
    fs::create_directories(project);
    const std::string scene = (project / "proj.fitzel").generic_string();

    int written = 0;
    // Stands in for the real serializer: writes something to the temp path it is
    // handed, exactly as projectio::saveSceneWithMaterials does.
    const auto writer = [&](const std::string& path) {
        ++written;
        std::ofstream f(path);
        f << "{}\n";
        return static_cast<bool>(f);
    };
    const auto failingWriter = [&](const std::string&) { return false; };

    autosave::Autosave a(dir.generic_string());
    a.setIntervalMinutes(1.0f);

    a.tick(0.0, true, scene, 1, writer);
    check(written == 0, "the first frame takes a baseline, it does not write");
    check(!autosave::pending(dir.generic_string()).valid(),
          "nothing is pending before the first snapshot");

    a.tick(30.0, true, scene, 2, writer);
    check(written == 0, "an edit inside the interval does not trigger a write");

    a.tick(61.0, true, scene, 2, writer);
    check(written == 1, "an edit writes once the interval is up");

    autosave::Snapshot pend = autosave::pending(dir.generic_string());
    check(pend.valid(), "the snapshot is found by pending()");
    check(pend.scenePath == fs::path(scene).generic_string(),
          "it names the scene it stands in for");
    check(pend.projectFolder == project.generic_string(),
          "and the project that scene belongs to");
    check(!pend.writtenAt.empty(), "and when it was taken");
    check(!a.status().empty(), "the editor can show when it last ran");

    a.tick(122.0, true, scene, 2, writer);
    check(written == 1, "an idle editor does not rewrite an unchanged scene");

    a.tick(183.0, true, scene, 2, writer);
    a.tick(244.0, true, scene, 2, writer);
    check(written == 2,
          "but it snapshots anyway once one is stale (edits that skip the undo history)");

    written = 0;
    a.tick(305.0, false, scene, 99, writer);
    check(written == 0, "play mode and loads are held off, however much changed");
    a.tick(366.0, true, std::string(), 99, writer);
    check(written == 0, "so is a session with no project to recover into");

    // A snapshot that cannot be written must not leave a sidecar pointing at the
    // last good scene with a fresh timestamp -- that would offer old work as new.
    const std::string before = autosave::pending(dir.generic_string()).writtenAt;
    autosave::Autosave b(dir.generic_string());
    b.setIntervalMinutes(1.0f);
    b.tick(0.0, true, scene, 1, failingWriter);
    b.tick(61.0, true, scene, 2, failingWriter);
    check(autosave::pending(dir.generic_string()).writtenAt == before,
          "a failed write leaves the previous snapshot untouched");
    check(!fs::exists(dir / "session.fitzel.tmp"),
          "and cleans up after itself");

    // The sidecar is what makes a snapshot visible. Without it there is no offer,
    // however good the scene file beside it looks.
    fs::remove(dir / "session.json");
    check(!autosave::pending(dir.generic_string()).valid(),
          "a scene with no sidecar is not offered (a write that never finished)");

    a.tick(427.0, true, scene, 100, writer);
    check(autosave::pending(dir.generic_string()).valid(), "a later snapshot is offered again");
    a.clear();
    check(!autosave::pending(dir.generic_string()).valid(),
          "saving (or a clean exit) clears the offer");
    check(!fs::exists(dir / "session.fitzel"), "and takes the file with it");

    // The project the snapshot belongs to is gone: there is nothing to restore
    // into, so it must not be offered.
    a.tick(500.0, true, scene, 101, writer);
    a.tick(561.0, true, scene, 102, writer);
    check(autosave::pending(dir.generic_string()).valid(), "a snapshot is pending again");
    fs::remove_all(project);
    check(!autosave::pending(dir.generic_string()).valid(),
          "a snapshot whose project has vanished is not offered");
    fs::create_directories(project);
    check(autosave::pending(dir.generic_string()).valid(),
          "and is offered again when the project is back");
}

// --- 2. The round trip ------------------------------------------------------

void checkRoundTrip(const fs::path& work) {
    std::printf("\nScene round trip\n");

    std::vector<Entity>      entities;
    std::vector<MaterialDef> materials;
    int matSel = 0, entityCounter = 0;
    Selection sel(entities);
    std::string currentProject, prefLocation, exportStatus, uiFontFamily;
    std::vector<std::string> recentProjects;
    char  projName[64] = "";
    float uiFontSize = 16.0f;
    fitzel::AssetDatabase assetDb;
    std::function<void(nlohmann::json&)>       writeSettings;
    std::function<void(const nlohmann::json&)> readSettings;
    std::function<void()>                      afterSceneLoad;

    projectio::Context ctx{
        entities, materials, matSel, entityCounter, sel,
        currentProject, projName, sizeof(projName), prefLocation,
        recentProjects, (work / "editor.json").generic_string(), exportStatus,
        uiFontSize, uiFontFamily,
        assetDb, work.generic_string(), work.generic_string(),
        [&]{ materials.push_back(MaterialDef{}); },   // seedDefaultMaterials
        [](const std::string&){ return -1; },         // importModel
        [](const std::string&, int){ return -1; },    // importModelNode
        [](int) -> LoadedModel* { return nullptr; },  // loadedModelById
        []{},                                         // clearModels
        writeSettings, readSettings, afterSceneLoad,
    };

    // A scene worth losing: two materials, and an object wearing the second one.
    MaterialDef red;
    red.assetId = fitzel::AssetId::generate();
    red.name = "Red"; red.albedo = {0.72f, 0.12f, 0.10f}; red.roughness = 0.30f;
    MaterialDef chrome;
    chrome.assetId = fitzel::AssetId::generate();
    chrome.name = "Chrome"; chrome.albedo = {0.90f, 0.92f, 0.95f};
    chrome.reflectivity = 1.0f; chrome.roughness = 0.04f;
    materials = {red, chrome};

    Entity box;
    box.type = EntityType::Box;
    box.name = "Crate";
    box.id = 7;
    box.localCenter = {1.0f, 2.0f, 3.0f};
    box.half = {0.5f, 0.5f, 0.5f};
    auto mc = std::make_unique<MaterialComponent>();
    mc->material = chrome.assetId;
    box.components.items.push_back(std::move(mc));
    entities.push_back(std::move(box));

    // Scene settings ride along too: the sculpted terrain and the painted grass
    // live in there, and they are hours of work that never touch an entity.
    writeSettings = [](nlohmann::json& j){ j["fogDensity"] = 0.125f; };
    float readFog = 0.0f;
    readSettings = [&](const nlohmann::json& j){ readFog = j.value("fogDensity", 0.0f); };
    afterSceneLoad = []{};

    const std::string snap = (work / "snapshot.fitzel").generic_string();
    check(projectio::saveSceneWithMaterials(ctx, snap), "a snapshot is written");

    // Wipe the document the way a crash does, and read it back.
    entities.clear();
    materials.clear();
    check(projectio::loadScene(ctx, snap), "and reads back");

    check(materials.size() == 2, "the material library comes back with it");
    bool guidsKept = materials.size() == 2 &&
                     materials[0].assetId == red.assetId &&
                     materials[1].assetId == chrome.assetId;
    check(guidsKept, "each material keeps its GUID");
    check(materials.size() == 2 && materials[1].name == "Chrome" &&
              materials[1].reflectivity == 1.0f,
          "and its settings");

    const Entity* crate = nullptr;
    for (const Entity& e : entities)
        if (e.id == 7) crate = &e;
    check(crate != nullptr, "the object is back");
    if (crate) {
        check(crate->name == "Crate" && crate->localCenter.y == 2.0f,
              "with its name and place");
        const MaterialComponent* m = crate->components.get<MaterialComponent>();
        check(m && m->material == chrome.assetId,
              "still wearing the material it was given");
        // ...and that material still EXISTS. This is the one that bites: the
        // entity keeps whatever GUID it was saved with, so a library that came
        // back with fresh ones looks fine object-side and resolves to nothing --
        // Document::materialIndex falls through to the default and the whole
        // scene loads out grey.
        const MaterialDef* worn = nullptr;
        if (m)
            for (const MaterialDef& md : materials)
                if (md.assetId == m->material) worn = &md;
        check(worn && worn->name == "Chrome",
              "and that material is still in the library (not resolved to default)");
    }
    check(readFog == 0.125f, "and the scene settings came along");

    // A snapshot has to stand alone: no .fmat files, no project folder, nothing
    // but the one file -- that is the whole point of writing it outside the
    // project it belongs to.
    std::ifstream f(snap);
    nlohmann::json j;
    f >> j;
    check(j.contains("materials") && j["materials"].is_array() &&
              j["materials"].size() == 2,
          "the file carries its materials inline");
    check(j["materials"][0].contains("id"), "each with the GUID entities point at");
}

} // namespace

int main(int argc, char** argv) {
    const fs::path work = argc > 1 ? fs::path(argv[1])
                                   : fs::temp_directory_path() / "fitzel-autosavecheck";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    std::printf("autosavecheck  (work dir: %s)\n", work.generic_string().c_str());

    checkWriter(work);
    checkRoundTrip(work);

    std::printf("\n%d checks, %d failed\n%s\n", checks, failures,
                failures ? "FAIL: the safety net has a hole in it"
                         : "OK: snapshots are written, found and restored");
    fs::remove_all(work, ec);
    return failures ? 1 : 0;
}
