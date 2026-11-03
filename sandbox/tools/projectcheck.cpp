// Does a Fitzel project hold together? Answered without a window.
//
// An agent that builds games (the Leitstand's Fitzel agent) runs this after
// every change; a person can too. It answers, as readable lines or one JSON line:
//
//   - every scene loads (projectio::loadScene), and what its objects refer to is
//     there: models and materials by GUID, scripts and sounds by file name. Read
//     from the raw JSON, because loading drops a model GUID it cannot resolve
//     without a word.
//   - every Lua script under scripts/ compiles -- the real compiler, file:line.
//   - the start scene's scripts run headless for N game seconds: start(), then
//     update() thirty times a second, with keys pressed now and then -- a game
//     that waits for Space would otherwise never reach its own code. Objects a
//     script spawns get their scripts too. Runtime errors come back with
//     file:line. One in a line that calls something this harness does not
//     simulate (rays, characters, towns, ...) is marked headless_artifact: it may
//     be the harness, not the game.
//   - --expect-log REGEX: a line the scripts must log; --expect-entity NAME: an
//     object that must exist after the run (spawned ones count).
//   - --watch NAME: where that object is every half second -- what a game DOES,
//     which compiling and not crashing cannot tell (does the ball move, does it
//     turn at the wall, does the paddle follow the keys).
//
// Nothing is drawn, so textures are not loaded (there is no GPU), only found.
// Projects whose game.json says "csharp" get the scenes checked, not the C#.
//
//   build/linux/bin/projectcheck <project> [--scene S] [--seconds N] [--no-input]
//                                [--expect-log RE]... [--expect-entity NAME]... [--watch NAME]... [--json]
//
// Exit 0: nothing found. 1: findings. 2: could not check (no project, no scene).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <fitzel/asset/AssetDatabase.hpp>

#include "Component.hpp"
#include "Document.hpp"
#include "ModelLibrary.hpp"
#include "ProjectIO.hpp"
#include "SceneGraph.hpp"
#include "SceneTypes.hpp"
#include "ScriptBridge.hpp"
#include "ScriptHost.hpp"
#include "ScriptSystem.hpp"
#include "Selection.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct Finding {
    std::string kind, file;
    int         line = 0;
    std::string msg;
    bool        artifact = false;
};

struct Report {
    std::vector<Finding>     errors;
    std::vector<std::string> warnings;
    json                     scenes = json::array();
    json                     scripts = json::array();
    std::vector<std::string> log;
    json                     expect = json::array();
    json                     run = json::object();
    json                     watch = json::object();
};

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string rel(const fs::path& project, const fs::path& p) {
    std::error_code ec;
    const fs::path r = fs::relative(p, project, ec);
    return ec || r.empty() ? p.generic_string() : r.generic_string();
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Every GUID the project's .meta sidecars name, and every file name in it: what
// a scene may refer to.
struct Inventory {
    std::set<std::string> guids;
    std::set<std::string> names;
};

Inventory inventory(const fs::path& project) {
    Inventory inv;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(project, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (it->is_directory() && (name.empty() || name[0] == '.' || name == "Library" || name == "bin" || name == "obj")) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        inv.names.insert(name);
        if (endsWith(name, ".meta")) {
            try {
                const json j = json::parse(readFile(it->path()));
                if (j.contains("guid") && j["guid"].is_string()) inv.guids.insert(j["guid"].get<std::string>());
            } catch (...) {}
        }
    }
    return inv;
}

// What a scene's objects refer to, from the raw JSON: loading would quietly drop
// a model it cannot find, and a script or sound named wrong only shows in Play.
void checkReferences(const fs::path& project, const fs::path& scene, const json& j, const Inventory& inv,
                     fitzel::AssetDatabase& assetDb, const std::set<std::string>& materialIds, Report& rep,
                     int& errorsHere) {
    const std::string file = rel(project, scene);
    auto known = [&](const std::string& guid) {
        if (inv.guids.count(guid) || materialIds.count(guid)) return true;
        const fitzel::AssetId id = fitzel::AssetId::fromString(guid);
        return id.valid() && !assetDb.pathForId(id).empty();
    };
    for (const json& e : j.value("entities", json::array())) {
        const std::string who = e.value("name", std::string("?"));
        for (const json& c : e.value("components", json::array())) {
            const std::string type = c.value("type", std::string());
            auto fail = [&](const std::string& msg) {
                rep.errors.push_back({"ref", file, 0, "object '" + who + "': " + msg, false});
                ++errorsHere;
            };
            if (type == "script") {
                const std::string f = c.value("file", std::string());
                if (!f.empty() && !fs::exists(project / "scripts" / f)) fail("its script scripts/" + f + " does not exist");
            } else if (type == "model") {
                const std::string g = c.value("model", std::string());
                if (!g.empty() && !known(g))
                    fail("its model " + c.value("modelFile", g) + " is not in the project (no asset with that GUID)");
            } else if (type == "material") {
                const std::string g = c.value("material", std::string());
                if (!g.empty() && !known(g)) fail("its material " + g + " is not in the library");
            }
            // Sounds and other files by name: any string field that names an audio file.
            for (auto it = c.begin(); it != c.end(); ++it) {
                if (!it.value().is_string()) continue;
                const std::string v = it.value().get<std::string>();
                static const char* audio[] = {".wav", ".ogg", ".mp3", ".flac"};
                for (const char* ext : audio)
                    if (endsWith(v, ext) && !inv.names.count(fs::path(v).filename().string()))
                        fail("its " + it.key() + " '" + v + "' is not a file in the project");
            }
        }
    }
}

// Compile one Lua file with the real compiler: "" or "line: message".
Finding compileLua(const fs::path& project, const fs::path& p) {
    Finding f{"lua-syntax", rel(project, p), 0, "", false};
    const std::string src = readFile(p);
    lua_State* L = luaL_newstate();
    const std::string chunk = "@" + f.file;
    if (luaL_loadbuffer(L, src.data(), src.size(), chunk.c_str()) != LUA_OK) {
        std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
        std::smatch m;
        if (std::regex_search(msg, m, std::regex(R"(:(\d+): (.*))"))) {
            f.line = std::stoi(m[1]);
            msg = m[2];
        }
        f.msg = msg;
    }
    lua_close(L);
    return f;
}

// game.* that need parts of the engine this harness does not run. An error in a
// line that calls one of them may be the harness's, not the game's.
const std::set<std::string> kNotSimulated = {
    "raycast", "castRay", "mouseRay", "moveCharacter", "removeCharacter", "rayFigure", "townPlaces", "townPath",
    "streetAt", "waterAt", "boneWorld", "boneNames", "spawnVehicle", "driveVehicle", "drivenVehicle", "leaveVehicle",
    "reach", "collectibles", "collectible", "animState", "getTimeOfDay", "getDayLength", "weather", "trees",
    "fellTree", "burnTree", "getLight", "cameraPos", "cameraDir", "shatter", "teleport", "sequenceTime",
    "currentScene", "orbitFrame", "applyImpulse", "getVelocity", "setAngularVelocity", "groundHeight",
    "terrainHeight", "worldToHud", "hudImage"};

bool lineCallsUnsimulated(const fs::path& script, int line) {
    if (line <= 0) return false;
    std::ifstream in(script);
    std::string l;
    for (int i = 1; std::getline(in, l); ++i) {
        if (i != line) continue;
        static const std::regex call(R"(game\.([A-Za-z_]\w*))");
        for (std::sregex_iterator it(l.begin(), l.end(), call), end; it != end; ++it)
            if (kNotSimulated.count((*it)[1])) return true;
        return false;
    }
    return false;
}

struct Options {
    fs::path                 project;
    std::string              scene;
    float                    seconds = 5.0f;
    bool                     input = true;
    bool                     json = false;
    std::vector<std::string> expectLog, expectEntity, watch;
};

// The start scene's scripts, headless. Errors, log lines and what exists after.
void runScripts(const Options& opt, projectio::Context& ctx, Document& doc, ModelLibrary& models,
                fitzel::AssetDatabase& assetDb, const fs::path& scene, const Inventory& inv,
                const std::vector<fs::path>& luaFiles, Report& rep) {
    doc.entities().clear();
    if (!projectio::loadScene(ctx, scene.generic_string())) {
        rep.errors.push_back({"load", rel(opt.project, scene), 0, "the scene does not load", false});
        return;
    }
    scenegraph::resolve(doc.entities());
    const fs::path scriptsDir = opt.project / "scripts";

    ScriptHost host;
    host.scriptsDir = scriptsDir.generic_string();
    std::vector<std::string>& log = rep.log;
    auto addLog = [&](const std::string& s) { if (log.size() < 2000) log.push_back(s); };
    scriptbridge::install(host, [&] {
        scriptbridge::Deps d;
        d.doc = &doc;
        d.models = &models;
        d.assetDb = &assetDb;
        d.setWorldRot = [&](int id, glm::vec3 r) {
            if (Entity* e = doc.find(id)) e->rotation = e->localRotation = r;
        };
        d.reparent = [&](int id, int parent) {
            if (Entity* e = doc.find(id)) e->parent = parent;
        };
        d.terrainHeight = [](float, float) { return 0.0f; };
        d.loadScene = [&](const std::string& s) { addLog("[projectcheck] game.loadScene(\"" + s + "\")"); };
        d.log = addLog;
        return d;
    }());
    host.log = addLog;

    // Keys: now and then one goes down and stays a while -- Enter and Space first
    // (title screens), then the usual game keys in turn.
    std::set<int> held, pressedNow;
    host.keyDown = [&](int k) { return held.count(k) > 0; };
    host.keyPressed = [&](int k) { return pressedNow.count(k) > 0; };
    host.mouseDown = [](int) { return false; };
    host.mousePressed = [](int) { return false; };
    host.textInput = [] { return std::string(); };
    host.mousePos = [](glm::vec2& p) { p = {960.0f, 540.0f}; return true; };

    // A small world: spawn, destroy, positions, velocities -- deferred like Play.
    std::vector<Entity> pendingSpawns;
    std::vector<int> pendingDestroy;
    std::map<int, glm::vec3> velocity;
    int nextId = 0;
    for (const Entity& e : doc.entities()) nextId = std::max(nextId, e.id + 1);
    auto findAny = [&](int id) -> Entity* {
        if (Entity* e = doc.find(id)) return e;
        for (Entity& e : pendingSpawns)
            if (e.id == id) return &e;
        return nullptr;
    };
    host.spawn = [&](const ScriptSpawn& s) -> int {
        Entity e;
        e.type = static_cast<EntityType>(s.type);
        e.localCenter = e.center = s.pos;
        e.half = glm::max(s.half, glm::vec3(0.02f));
        e.localRotation = e.rotation = s.rot;
        e.name = s.name.empty() ? "spawned" : s.name;
        e.parent = s.parent;
        if (!s.model.empty()) {
            const int mid = host.loadModel ? host.loadModel(s.model) : -1;
            if (mid < 0 && !inv.names.count(fs::path(s.model).filename().string())) {
                addLog("[Fitzel] game.spawn: no model '" + s.model + "'");
                rep.warnings.push_back("game.spawn asked for the model '" + s.model + "', which is not in the project");
                return 0;
            }
            e.type = EntityType::Model;
        }
        if (!s.script.empty()) {
            auto sc = std::make_unique<ScriptComponent>();
            sc->file = s.script;
            e.components.items.push_back(std::move(sc));
            if (!fs::exists(scriptsDir / s.script))
                rep.errors.push_back({"ref", "", 0, "game.spawn gives an object the script '" + s.script +
                                      "', which does not exist under scripts/", false});
        }
        e.id = nextId++;
        velocity[e.id] = s.vel;
        pendingSpawns.push_back(std::move(e));
        return pendingSpawns.back().id;
    };
    host.destroy = [&](int id) { pendingDestroy.push_back(id); };
    host.getPos = [&](int id, glm::vec3& out) {
        const Entity* e = findAny(id);
        if (!e) return false;
        out = e->center;
        return true;
    };
    host.setPos = [&](int id, glm::vec3 p) {
        Entity* e = findAny(id);
        if (!e) return;
        const Entity* parent = e->parent >= 0 ? findAny(e->parent) : nullptr;
        e->center = p;
        e->localCenter = parent ? p - parent->center : p;
    };
    host.setActive = [&](int id, bool on) {
        if (Entity* e = findAny(id)) e->active = on;
    };
    host.setVelocity = [&](int id, glm::vec3 v) {
        if (findAny(id)) velocity[id] = v;
    };
    host.getVelocity = [&](int id, glm::vec3& out) {
        if (!findAny(id)) return false;
        out = velocity[id];
        return true;
    };
    std::set<std::string> sounds;
    host.playSound = [&](const std::string& name) { sounds.insert(name); };
    host.measureText = [](const std::string& s, float size, bool) {
        return glm::vec2(static_cast<float>(s.size()) * size * 0.5f, size);
    };

    ScriptSystem scripts;
    scripts.setHost(&host);
    const float dt = 1.0f / 30.0f;
    const int frames = std::max(1, static_cast<int>(std::lround(opt.seconds / dt)));
    static const int cycle[] = {87, 265, 83, 264, 65, 263, 68, 262, 32};   // W Up S Down A Left D Right Space
    std::set<std::string> seen;
    const std::size_t startCount = doc.entities().size();
    std::size_t spawned = 0, hudTexts = 0;
    float t = 0.0f;
    for (int f = 0; f < frames; ++f) {
        pressedNow.clear();
        if (opt.input) {
            std::set<int> want;
            if (f >= 30 && f < 34) want = {257, 32};                             // Enter, Space
            else if (f >= 45) want = {cycle[(f / 15) % 9], cycle[(f / 15 + 4) % 9]};
            for (int k : want)
                if (!held.count(k)) pressedNow.insert(k);
            held = want;
        }
        host.hudCmds.clear();
        scripts.beginFrame();
        const std::size_t n = doc.entities().size();
        for (std::size_t i = 0; i < n; ++i) {
            Entity& e = doc.entities()[i];
            if (!e.active || !e.activeInHierarchy) continue;
            for (auto& c : e.components.items) {
                auto* sc = dynamic_cast<ScriptComponent*>(c.get());
                if (!sc || sc->file.empty() || endsWith(sc->file, ".cs")) continue;
                const fs::path path = scriptsDir / sc->file;
                const std::string before = scripts.lastError();
                scripts.update(e, *sc, path.generic_string(), dt, t);
                const std::string& err = scripts.lastError();
                if (err == before || err.empty() || seen.count(err)) continue;
                seen.insert(err);
                if (err.rfind("cannot open ", 0) == 0) continue;   // a missing script: the reference check says so
                Finding fd{"lua-runtime", rel(opt.project, path), 0, err, false};
                std::smatch m;
                if (std::regex_search(err, m, std::regex(R"(([^\s:]+\.lua):(\d+): (.*))"))) {
                    // Lua shortens a long chunk name to "...<its end>": find the script that ends so.
                    fs::path where = path;
                    const std::string named = m[1].str();
                    if (named.rfind("...", 0) == 0) {
                        for (const fs::path& f : luaFiles)
                            if (endsWith(f.generic_string(), named.substr(3))) where = f;
                    } else {
                        where = fs::path(named).is_absolute() ? fs::path(named) : scriptsDir / named;
                    }
                    fd.file = rel(opt.project, where);
                    fd.line = std::stoi(m[2]);
                    fd.msg = m[3];
                    fd.artifact = lineCallsUnsimulated(where, fd.line);
                }
                fd.msg += "  (at " + std::to_string(t).substr(0, 4) + " s, object '" + e.name + "')";
                rep.errors.push_back(fd);
            }
        }
        for (Entity& e : pendingSpawns) doc.entities().push_back(std::move(e));
        spawned += pendingSpawns.size();
        pendingSpawns.clear();
        for (int id : pendingDestroy) {
            auto& ents = doc.entities();
            ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) { return e.id == id; }), ents.end());
            scripts.removeEntity(id);
            velocity.erase(id);
        }
        pendingDestroy.clear();
        for (auto& [id, v] : velocity)
            if (Entity* e = doc.find(id); e && glm::dot(v, v) > 0.0f) {
                e->center += v * dt;
                e->localCenter += v * dt;
            }
        scenegraph::resolve(doc.entities());
        hudTexts = host.hudCmds.size();
        t += dt;
        // Where the watched objects are, every half second (and at the end).
        if (!opt.watch.empty() && (f % 15 == 14 || f == frames - 1)) {
            for (const std::string& name : opt.watch) {
                const Entity* hit = nullptr;
                for (const Entity& e : doc.entities())
                    if (e.name == name) { hit = &e; break; }
                json& track = rep.watch[name];
                if (!track.is_array()) track = json::array();
                auto r = [](float v) { return std::round(v * 100.0f) / 100.0f; };
                if (hit) track.push_back({r(t), r(hit->center.x), r(hit->center.y), r(hit->center.z), hit->active});
                else track.push_back({r(t), nullptr});
            }
        }
    }
    for (const std::string& s : sounds)
        if (!inv.names.count(fs::path(s).filename().string()) && !inv.names.count(s + ".wav") &&
            !inv.names.count(s + ".ogg") && !inv.names.count(s + ".mp3"))
            rep.warnings.push_back("game.playSound(\"" + s + "\"): no such sound file in the project (silent in the game)");

    std::set<std::string> names;
    for (const Entity& e : doc.entities()) names.insert(e.name);
    for (const std::string& want : opt.expectEntity)
        rep.expect.push_back({{"what", "object " + want}, {"ok", names.count(want) > 0}});
    for (const std::string& re : opt.expectLog) {
        bool hit = false;
        try {
            const std::regex rx(re);
            for (const std::string& l : log) hit = hit || std::regex_search(l, rx);
        } catch (const std::regex_error&) {
            rep.warnings.push_back("--expect-log '" + re + "' is not a valid regular expression");
        }
        rep.expect.push_back({{"what", "log line /" + re + "/"}, {"ok", hit}});
    }
    rep.run = {{"scene", rel(opt.project, scene)}, {"seconds", opt.seconds}, {"frames", frames},
               {"objects_at_start", startCount}, {"objects_at_end", doc.entities().size()}, {"spawned", spawned},
               {"hud_commands_last_frame", hudTexts}, {"hud_text", host.hud}};
}

int usage() {
    std::fprintf(stderr, "usage: projectcheck <project> [--scene S] [--seconds N] [--no-input] "
                         "[--expect-log RE]... [--expect-entity NAME]... [--watch NAME]... [--json]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--scene") opt.scene = next();
        else if (a == "--seconds") opt.seconds = std::max(0.0f, std::strtof(next().c_str(), nullptr));
        else if (a == "--no-input") opt.input = false;
        else if (a == "--expect-log") opt.expectLog.push_back(next());
        else if (a == "--expect-entity") opt.expectEntity.push_back(next());
        else if (a == "--watch") opt.watch.push_back(next());
        else if (a == "--json") opt.json = true;
        else if (!a.empty() && a[0] == '-') return usage();
        else opt.project = a;
    }
    if (opt.project.empty()) return usage();
    std::error_code ec;
    opt.project = fs::weakly_canonical(fs::absolute(opt.project), ec);
    if (!fs::is_directory(opt.project)) {
        std::fprintf(stderr, "projectcheck: %s is not a folder\n", opt.project.string().c_str());
        return 2;
    }

    Document doc;
    Selection sel(doc.entities());
    int matSel = 0, entityCounter = 0;
    std::string currentProject, prefLocation, exportStatus, uiFontFamily;
    std::vector<std::string> recentProjects;
    char projName[64] = "";
    float uiFontSize = 16.0f;
#ifdef FITZEL_CONTENT_DIR
    fitzel::AssetDatabase assetDb{fs::path(FITZEL_CONTENT_DIR)};
#else
    fitzel::AssetDatabase assetDb;
#endif
    ModelLibrary models;
    std::function<void(json&)> writeSettings = [](json&) {};
    std::function<void(const json&)> readSettings = [](const json&) {};
    std::function<void()> afterSceneLoad = [] {};
    // Editor prefs go to a file of their own: a check never touches the editor's.
    const fs::path prefs = fs::temp_directory_path() / "projectcheck-editor.json";
    projectio::Context ctx{
        doc.entities(), doc.materials(), matSel, entityCounter, sel,
        currentProject, projName, sizeof(projName), prefLocation,
        recentProjects, prefs.generic_string(), exportStatus,
        uiFontSize, uiFontFamily,
        assetDb, opt.project.generic_string(), opt.project.generic_string(),
        [&] { doc.materials().push_back(MaterialDef{}); },
        [](const std::string&) { return -1; },       // models are checked by GUID, not imported
        [](const std::string&, int) { return -1; },
        [](int) -> LoadedModel* { return nullptr; },
        [] {},
        writeSettings, readSettings, afterSceneLoad,
    };
    assetDb.mountProject(opt.project);
    assetDb.refresh();
    projectio::loadProjectMaterials(ctx, projectio::matsDirIn(opt.project.generic_string()));
    std::set<std::string> materialIds;
    for (const MaterialDef& m : doc.materials()) materialIds.insert(m.assetId.toString());

    Report rep;
    const Inventory inv = inventory(opt.project);
    std::vector<fs::path> scenes;
    for (const auto& e : fs::directory_iterator(opt.project, ec))
        if (e.is_regular_file() && e.path().extension() == ".fitzel") scenes.push_back(e.path());
    std::sort(scenes.begin(), scenes.end());
    if (scenes.empty()) {
        std::fprintf(stderr, "projectcheck: no .fitzel scene in %s\n", opt.project.string().c_str());
        return 2;
    }

    // game.json: the start scene, and whether the scripts are C#.
    json game = json::object();
    try {
        if (fs::exists(opt.project / "game.json")) game = json::parse(readFile(opt.project / "game.json"));
    } catch (const std::exception& e) {
        rep.errors.push_back({"load", "game.json", 0, std::string("not valid JSON: ") + e.what(), false});
    }
    const bool csharp = game.value("scripting", std::string("lua")) == "csharp";

    for (const fs::path& s : scenes) {
        int errorsHere = 0;
        json j;
        try {
            j = json::parse(readFile(s));
        } catch (const std::exception& e) {
            rep.errors.push_back({"load", rel(opt.project, s), 0, std::string("not valid JSON: ") + e.what(), false});
            rep.scenes.push_back({{"file", rel(opt.project, s)}, {"entities", 0}, {"errors", 1}});
            continue;
        }
        checkReferences(opt.project, s, j, inv, assetDb, materialIds, rep, errorsHere);
        doc.entities().clear();
        if (!projectio::loadScene(ctx, s.generic_string())) {
            rep.errors.push_back({"load", rel(opt.project, s), 0, "Fitzel cannot load this scene", false});
            ++errorsHere;
        }
        rep.scenes.push_back({{"file", rel(opt.project, s)}, {"entities", doc.entities().size()}, {"errors", errorsHere}});
    }

    std::vector<fs::path> luaFiles;
    if (fs::is_directory(opt.project / "scripts"))
        for (const auto& e : fs::recursive_directory_iterator(opt.project / "scripts", ec))
            if (e.is_regular_file() && e.path().extension() == ".lua") luaFiles.push_back(e.path());
    std::sort(luaFiles.begin(), luaFiles.end());
    bool syntaxOk = true;
    for (const fs::path& p : luaFiles) {
        const Finding f = compileLua(opt.project, p);
        json lint = json::array();
        if (!f.msg.empty()) {
            rep.errors.push_back(f);
            lint.push_back("line " + std::to_string(f.line) + ": " + f.msg);
            syntaxOk = false;
        }
        rep.scripts.push_back({{"file", rel(opt.project, p)}, {"lint", lint}});
    }
    if (csharp) rep.warnings.push_back("the scripts are C#: this check covers the scenes, not the C# code");

    // The headless run: the scene asked for, else game.json's start scene, else the folder's own.
    fs::path runScene = opt.scene.empty() ? fs::path() : opt.project / (opt.scene + ".fitzel");
    if (runScene.empty() && game.contains("startScene") && game["startScene"].is_string())
        runScene = opt.project / (game["startScene"].get<std::string>() + ".fitzel");
    if (runScene.empty() || !fs::exists(runScene)) runScene = projectio::sceneFileIn(opt.project.generic_string());
    if (!fs::exists(runScene)) runScene = scenes.front();
    if (!opt.scene.empty() && !fs::exists(opt.project / (opt.scene + ".fitzel")))
        rep.errors.push_back({"load", opt.scene + ".fitzel", 0, "there is no such scene", false});
    else if (opt.seconds > 0.0f && !csharp && syntaxOk)
        runScripts(opt, ctx, doc, models, assetDb, runScene, inv, luaFiles, rep);
    else if (opt.seconds > 0.0f && !syntaxOk)
        rep.warnings.push_back("the scripts were not run: fix the syntax errors first");

    bool expectOk = true;
    for (const json& x : rep.expect) expectOk = expectOk && x.value("ok", false);
    const bool clean = rep.errors.empty() && expectOk;

    if (opt.json) {
        json errs = json::array();
        for (const Finding& f : rep.errors)
            errs.push_back({{"kind", f.kind}, {"file", f.file}, {"line", f.line}, {"msg", f.msg},
                            {"headless_artifact", f.artifact}});
        json out = {{"ok", clean}, {"project", opt.project.generic_string()}, {"scenes", rep.scenes},
                    {"scripts", rep.scripts}, {"errors", errs}, {"warnings", rep.warnings}, {"expect", rep.expect},
                    {"run", rep.run}, {"watch", rep.watch}};
        json log = json::array();
        for (std::size_t i = 0; i < rep.log.size() && i < 200; ++i) log.push_back(rep.log[i]);
        out["log"] = log;
        std::printf("%s\n", out.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    } else {
        for (const json& s : rep.scenes)
            std::printf("scene %s: %d objects%s\n", s["file"].get<std::string>().c_str(), s["entities"].get<int>(),
                        s["errors"].get<int>() ? " -- with errors" : "");
        std::printf("%zu Lua script(s)\n", rep.scripts.size());
        for (const Finding& f : rep.errors)
            std::printf("FAIL %s %s:%d: %s%s\n", f.kind.c_str(), f.file.c_str(), f.line, f.msg.c_str(),
                        f.artifact ? " [maybe headless only]" : "");
        for (const std::string& w : rep.warnings) std::printf("warn %s\n", w.c_str());
        for (const json& x : rep.expect)
            std::printf("%s expect %s\n", x.value("ok", false) ? "ok  " : "FAIL", x["what"].get<std::string>().c_str());
        if (!rep.run.empty())
            std::printf("ran %s for %.1f s: %zu objects at the end (%zu spawned), %zu log lines\n",
                        rep.run["scene"].get<std::string>().c_str(), opt.seconds,
                        rep.run["objects_at_end"].get<std::size_t>(), rep.run["spawned"].get<std::size_t>(),
                        rep.log.size());
        for (auto it = rep.watch.begin(); it != rep.watch.end(); ++it) {
            std::printf("watch %s:", it.key().c_str());
            for (const json& p : it.value())
                if (p[1].is_null()) std::printf(" %.1fs:-", p[0].get<float>());
                else std::printf(" %.1fs:(%.2f %.2f %.2f)", p[0].get<float>(), p[1].get<float>(), p[2].get<float>(),
                                 p[3].get<float>());
            std::printf("\n");
        }
        std::printf("%s\n", clean ? "projectcheck: ok" : "projectcheck: FINDINGS");
    }
    return clean ? 0 : 1;
}
