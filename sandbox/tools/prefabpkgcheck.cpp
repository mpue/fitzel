// prefabpkgcheck -- does a prefab arrive in another project with everything it
// needs, and does the import leave the project's own files alone?
//
// A package fails quietly on both ends. Leave out a model's .meta and the new
// project mints the model a fresh GUID: the file is there and the prefab points
// at nothing. Leave out the graph and the figure stands still. On the way in,
// overwrite a file of the same name and some other object in the project changes
// its sound; copy a second "Default" material and the library fills with twins.
// None of that shows until someone opens the other project.
//
// So two real projects are built in a temp folder, a prefab is exported from the
// first and imported into the second, and the files are read back.
//
//   build/release/bin/prefabpkgcheck.exe
// Exits non-zero if any check fails.
//
// And a look at real projects, for when a package from one comes out wrong:
//   prefabpkgcheck --export <engine content> <project> <prefab file> <out.zip>
//   prefabpkgcheck --plan   <engine content> <project> <package.zip>
//   prefabpkgcheck --apply  <engine content> <project> <package.zip>
// --export and --plan only read the project; --apply writes into it.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "miniz.h"

#include <fitzel/asset/AssetDatabase.hpp>

#include "../src/AnimGraph.hpp"
#include "../src/PrefabPackage.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using prefabpkg::Item;

namespace {

int g_fails = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++g_fails;
}

void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << s;
}

std::string get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string guidOf(const fs::path& asset) {
    try { return json::parse(get(fs::path(asset.string() + ".meta"))).value("guid", std::string()); }
    catch (const json::exception&) { return {}; }
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool anyNote(const std::vector<std::string>& notes, const std::string& part) {
    for (const std::string& n : notes) if (n.find(part) != std::string::npos) return true;
    return false;
}

const Item* itemFor(const prefabpkg::Plan& p, const std::string& entry) {
    for (const Item& it : p.items) if (it.entry == entry) return &it;
    return nullptr;
}

const char* actionName(const Item* it) {
    if (!it) return "(missing)";
    switch (it->action) {
        case Item::Action::Add:     return "add";
        case Item::Action::Present: return "present";
        case Item::Action::Rename:  return "rename";
        case Item::Action::Update:  return "update";
    }
    return "?";
}

void expect(const prefabpkg::Plan& p, const std::string& entry, Item::Action a,
            const std::string& what) {
    const Item* it = itemFor(p, entry);
    check(it && it->action == a, what, entry + ": " + actionName(it) +
                                           (it && !it->why.empty() ? " (" + it->why + ")" : ""));
}

json component(const json& prefab, int entity, const std::string& type) {
    for (const auto& c : prefab["prefab"]["entities"][entity]["components"])
        if (c.value("type", std::string()) == type) return c;
    return json::object();
}

std::string writeZip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof zip);
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (const auto& e : entries)
        mz_zip_writer_add_mem(&zip, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    std::size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    mz_zip_writer_end(&zip);
    put(path, std::string(static_cast<const char*>(buf), size));
    mz_free(buf);
    return path.generic_string();
}

} // namespace

// The command-line half: a real project, a real prefab, what would happen.
int realProject(int argc, char** argv) {
    const std::string mode = argv[1];
    fitzel::AssetDatabase db{fs::path(argv[2])};
    db.mountProject(fs::path(argv[3]));
    db.refresh();
    std::string err;
    if (mode == "--export" && argc == 6) {
        prefabpkg::Report rep;
        if (!prefabpkg::exportZip({argv[3], &db, nullptr}, argv[4], argv[5], rep, err)) {
            std::printf("export failed: %s\n", err.c_str());
            return 1;
        }
        for (const std::string& f : rep.files) std::printf("  packed  %s\n", f.c_str());
        for (const std::string& n : rep.notes) std::printf("  note    %s\n", n.c_str());
        return 0;
    }
    if ((mode == "--plan" || mode == "--apply") && argc == 5) {
        prefabpkg::Plan plan;
        if (!prefabpkg::planImport({argv[3], &db}, argv[4], plan, err)) {
            std::printf("plan failed: %s\n", err.c_str());
            return 1;
        }
        for (const Item& it : plan.items)
            std::printf("  %-8s %s%s%s\n", actionName(&it), it.target.c_str(),
                        it.why.empty() ? "" : "  -- ", it.why.c_str());
        for (const std::string& n : plan.notes) std::printf("  note     %s\n", n.c_str());
        if (mode == "--plan") return 0;
        prefabpkg::Applied done;
        if (!prefabpkg::applyImport({argv[3], &db}, plan, done, err)) {
            std::printf("apply failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("  wrote %d files; prefab at %s\n", static_cast<int>(done.written.size()),
                    done.prefabPath.c_str());
        return 0;
    }
    std::printf("usage: see the top of prefabpkgcheck.cpp\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1][0] == '-') return realProject(argc, argv);
    std::printf("prefabpkgcheck\n");
    const fs::path root = fs::temp_directory_path() / "fitzel_prefabpkgcheck";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path engine = root / "engine", A = root / "projA", B = root / "projB";

    // --- Project A: a car that needs a bit of everything ------------------------
    put(engine / "sounds" / "engine.wav", "RIFF engine");          // every fitzel has it
    put(A / "models" / "car.glb", "glTF car");
    put(A / "textures" / "red.png", "PNG red");
    put(A / "sounds" / "honk.wav", "RIFF honk");                   // named only by the script
    put(A / "sounds" / "boost.wav", "RIFF boost A");
    put(A / "scripts" / "drive.lua", "function update(e) game.playSound(\"honk.wav\") end\n");
    put(A / "content" / "midi" / "tune.mid", "MThd tune");
    fitzel::AssetDatabase dbA(engine);
    dbA.mountProject(A);
    dbA.refresh();
    const std::string carGuid = dbA.idForPath(A / "models" / "car.glb").toString();
    const std::string redTex  = dbA.idForPath(A / "textures" / "red.png").toString();
    const std::string defMat  = json{{"name", "Default"}, {"albedo", {0.8, 0.8, 0.8}}}.dump(2);
    put(A / "materials" / "Default-aaaa1111.fmat", defMat);
    put(A / "materials" / "Red-aaaa2222.fmat",
        json{{"name", "Red"}, {"albedo", {1, 0, 0}}, {"texture", redTex}}.dump(2));
    dbA.refresh();
    const std::string defGuidA = dbA.idForPath(A / "materials" / "Default-aaaa1111.fmat").toString();
    const std::string redGuid  = dbA.idForPath(A / "materials" / "Red-aaaa2222.fmat").toString();

    const std::string wheelGuid = "11111111222222223333333344444444";
    put(A / "prefabs" / "Wheel-11111111.fprefab",
        json{{"version", 1}, {"prefab", {{"name", "Wheel"}, {"guid", wheelGuid},
             {"entities", {{{"id", 0}, {"parent", -1}, {"name", "Wheel"},
                            {"components", {{{"type", "material"}, {"material", defGuidA}}}}}}}}}}.dump(2));
    const std::string carPrefabGuid = "aaaaaaaabbbbbbbbccccccccdddddddd";
    json car = {{"version", 1}, {"prefab", {{"name", "Car"}, {"guid", carPrefabGuid}, {"entities", {
        {{"id", 0}, {"parent", -1}, {"name", "Car"}, {"components", {
            {{"type", "model"}, {"model", carGuid}, {"modelFile", "car.glb"}},
            {{"type", "material"}, {"material", redGuid}},
            {{"type", "script"}, {"file", "drive.lua"}},
            {{"type", "boost_pad"}, {"sound", "boost.wav"}},
            {{"type", "finish_line"}, {"soundGo", "engine.wav"}},
            {{"type", "anim_graph"}, {"graph", "Graph 1"}},
            {{"type", "synth"}, {"midi", "tune.mid"}},
            {{"type", "spawner"}, {"prefab", "Wheel"}}}}},
        {{"id", 1}, {"parent", 0}, {"name", "Body"}, {"components", {
            {{"type", "material"}, {"material", defGuidA}},
            {{"type", "mesh"}, {"faceMats", "0 " + redGuid + " 3 " + redGuid}}}}}}}}}};
    const fs::path carFile = A / "prefabs" / "Car-aaaaaaaa.fprefab";
    put(carFile, car.dump(2));

    // The scene the car was made in: its graph, one state of which plays a
    // Timeline clip (which cannot travel).
    animgraph::Graph g;
    g.name = "Graph 1";
    animgraph::State idle, door;
    idle.name = "Idle"; idle.modelClip = "idle";
    door.name = "Door"; door.clip = "Door opens";
    g.states = {idle, door};
    const std::vector<animgraph::Graph> sceneGraphs{g};

    // --- Export ---------------------------------------------------------------------
    const fs::path zip = root / "car.zip";
    prefabpkg::Report rep;
    std::string err;
    const bool exported = prefabpkg::exportZip({A.generic_string(), &dbA, &sceneGraphs},
                                               carFile.generic_string(), zip.generic_string(), rep, err);
    check(exported, "the prefab exports", err);
    for (const char* f : {"prefabs/Car-aaaaaaaa.fprefab", "models/car.glb", "models/car.glb.meta",
                          "materials/Red-aaaa2222.fmat", "materials/Red-aaaa2222.fmat.meta",
                          "textures/red.png", "textures/red.png.meta", "materials/Default-aaaa1111.fmat",
                          "scripts/drive.lua", "sounds/boost.wav", "content/midi/tune.mid",
                          "prefabs/Wheel-11111111.fprefab"})
        check(has(rep.files, f), std::string("packed: ") + f);
    check(has(rep.files, "sounds/honk.wav"), "packed: the sound only the script names");
    check(!has(rep.files, "sounds/engine.wav") && anyNote(rep.notes, "engine.wav"),
          "the engine's own sound stays behind, and says so");
    check(anyNote(rep.notes, "Door opens"), "a Timeline clip in the graph is reported, not packed");

    // --- Project B: some of it is there already ---------------------------------
    put(B / "sounds" / "boost.wav", "RIFF boost B -- a different sound");
    put(B / "scripts" / "drive.lua", get(A / "scripts" / "drive.lua"));
    put(B / "materials" / "Default-bbbb1111.fmat", defMat);        // its own Default
    fitzel::AssetDatabase dbB(engine);
    dbB.mountProject(B);
    dbB.refresh();
    const std::string defGuidB = dbB.idForPath(B / "materials" / "Default-bbbb1111.fmat").toString();
    const std::string boostB   = get(B / "sounds" / "boost.wav");

    prefabpkg::Plan plan;
    check(prefabpkg::planImport({B.generic_string(), &dbB}, zip.generic_string(), plan, err),
          "the package reads", err);
    expect(plan, "models/car.glb", Item::Action::Add, "a model the project lacks is added");
    expect(plan, "materials/Default-aaaa1111.fmat", Item::Action::Present,
           "the project's own Default is used instead of a twin");
    expect(plan, "scripts/drive.lua", Item::Action::Present, "an identical script is left alone");
    expect(plan, "sounds/boost.wav", Item::Action::Rename, "a different file of the same name is not overwritten");
    expect(plan, "prefabs/Car-aaaaaaaa.fprefab", Item::Action::Add, "the prefab is new here");
    check(!plan.notes.empty() && anyNote(plan.notes, "engine.wav"), "the exporter's notes reach the import");

    prefabpkg::Applied done;
    check(prefabpkg::applyImport({B.generic_string(), &dbB}, plan, done, err), "the import writes", err);
    check(get(B / "sounds" / "boost.wav") == boostB, "the project's own boost.wav is untouched");
    check(get(B / "sounds" / "boost (2).wav") == "RIFF boost A", "the incoming one sits beside it");
    check(guidOf(B / "models" / "car.glb") == carGuid, "the model keeps its GUID -- the prefab still finds it");
    check(fs::exists(B / "sounds" / "honk.wav"), "the script's sound came along");
    check(fs::exists(B / "prefabs" / "Wheel-11111111.fprefab"), "the prefab it spawns came along");
    check(!fs::exists(B / "materials" / "Default-aaaa1111.fmat"), "no second Default");

    const json carB = json::parse(get(B / "prefabs" / "Car-aaaaaaaa.fprefab"));
    check(component(carB, 0, "boost_pad").value("sound", std::string()) == "boost (2).wav",
          "the prefab plays the sound it brought, under its new name",
          component(carB, 0, "boost_pad").value("sound", std::string()));
    check(component(carB, 1, "material").value("material", std::string()) == defGuidB,
          "and uses the project's Default");
    check(component(carB, 1, "mesh").value("faceMats", std::string()).find(redGuid) != std::string::npos,
          "per-face materials that came along are still named");
    check(carB["prefab"].contains("animGraphs") && carB["prefab"]["animGraphs"][0].value("name", "") == "Graph 1",
          "the graph its object runs travelled inside it");
    check(done.prefabPath == (B / "prefabs" / "Car-aaaaaaaa.fprefab").generic_string(),
          "the import names the prefab it brought", done.prefabPath);

    // --- The same package again: nothing to do -----------------------------------
    dbB.refresh();
    prefabpkg::Plan again;
    prefabpkg::planImport({B.generic_string(), &dbB}, zip.generic_string(), again, err);
    int busy = 0;
    for (const Item& it : again.items) busy += it.action != Item::Action::Present;
    check(busy == 0, "importing the same package twice writes nothing", std::to_string(busy) + " items would");

    // --- A newer version of the same prefab ---------------------------------------
    car["prefab"]["name"] = "Car Mk2";
    put(carFile, car.dump(2));
    const fs::path zip2 = root / "car2.zip";
    prefabpkg::exportZip({A.generic_string(), &dbA, &sceneGraphs}, carFile.generic_string(),
                         zip2.generic_string(), rep, err);
    prefabpkg::Plan upd;
    prefabpkg::planImport({B.generic_string(), &dbB}, zip2.generic_string(), upd, err);
    expect(upd, "prefabs/Car-aaaaaaaa.fprefab", Item::Action::Update, "a newer version of the same prefab replaces it");
    prefabpkg::applyImport({B.generic_string(), &dbB}, upd, done, err);
    check(fs::exists(B / "prefabs" / "Car-aaaaaaaa.fprefab.bak") &&
              get(B / "prefabs" / "Car-aaaaaaaa.fprefab").find("Car Mk2") != std::string::npos,
          "and the old one is kept as .bak");

    // --- Packages that must be refused --------------------------------------------
    const std::string evil = writeZip(root / "evil.zip",
        {{"fitzel-prefab.json", json{{"format", 1}, {"prefab", {{"file", "x.fprefab"}}}}.dump()},
         {"x.fprefab", "{}"}, {"../outside.txt", "gotcha"}});
    prefabpkg::Plan bad;
    std::string why;
    check(!prefabpkg::planImport({B.generic_string(), &dbB}, evil, bad, why) &&
              why.find("outside") != std::string::npos,
          "a zip that would write outside the project is refused", why);
    const std::string plain = writeZip(root / "plain.zip", {{"readme.txt", "hello"}});
    check(!prefabpkg::planImport({B.generic_string(), &dbB}, plain, bad, why),
          "a zip that is not a prefab package is refused", why);

    fs::remove_all(root, ec);
    std::printf(g_fails ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}