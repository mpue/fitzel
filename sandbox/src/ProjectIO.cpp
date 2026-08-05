#include "ProjectIO.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include <glm/glm.hpp>

#include <fitzel/Version.hpp>
#include <fitzel/asset/AssetDatabase.hpp>

#include "GameSettings.hpp"
#include "PropertyMeta.hpp"

using fitzel::AssetId;

namespace projectio {

namespace {

nlohmann::json vec3Json(const glm::vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}
glm::vec3 readVec3Json(const nlohmann::json& a, glm::vec3 def) {
    if (a.is_array() && a.size() == 3)
        return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
    return def;
}

// Write a `.meta` sidecar so the database adopts our chosen GUID for a freshly
// written asset file (keeps in-memory ids and on-disk ids in sync).
void writeMeta(const std::string& path, AssetId id, const char* type) {
    nlohmann::json m; m["guid"] = id.toString(); m["type"] = type;
    std::ofstream f(path + ".meta"); if (f) f << m.dump(2) << '\n';
}

// Serialise one material to <dir>/<name>-<guid8>.fmat (+ its .meta).
void writeMaterialFile(const MaterialDef& md, const std::string& dir) {
    const std::string file = dir + "/" + safeName(md.name) + "-" +
                             md.assetId.toString().substr(0, 8) + ".fmat";
    nlohmann::json m;
    m["name"]         = md.name;
    m["albedo"]       = vec3Json(md.albedo);
    m["tint"]         = vec3Json(md.tint);
    m["reflectivity"] = md.reflectivity;
    m["roughness"]    = md.roughness;
    m["opacity"]      = md.opacity;
    m["glass"]        = md.glass;
    m["alphaMode"]    = static_cast<int>(md.alphaMode);
    m["alphaCutoff"]  = md.alphaCutoff;
    m["emission"]         = vec3Json(md.emission);
    m["emissionStrength"] = md.emissionStrength;
    if (md.texId.valid())         m["texture"]     = md.texId.toString();
    if (md.normalTexId.valid())   m["normalMap"]   = md.normalTexId.toString();
    if (md.emissionTexId.valid()) m["emissionMap"] = md.emissionTexId.toString();
    std::ofstream f(file); if (f) f << m.dump(2) << '\n';
    writeMeta(file, md.assetId, "Material");
}

} // namespace

// Filesystem-safe token from a display name.
std::string safeName(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s)
        o.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return o.empty() ? std::string("material") : o;
}

void savePrefs(const Context& ctx) {
    nlohmann::json j;
    j["lastLocation"] = ctx.prefLocation;
    j["recent"]       = ctx.recentProjects;
    j["uiFontSize"]   = ctx.uiFontSize;
    j["uiFontFamily"] = ctx.uiFontFamily;
    std::ofstream f(ctx.prefsPath);
    if (f) f << j.dump(2) << '\n';
}

// One canonical spelling for a project folder, so the same path written with
// mixed separators / a trailing slash / a "." segment compares equal.
static std::string normProjectPath(const std::string& folder) {
    return std::filesystem::path(folder).lexically_normal().generic_string();
}

// Collapse a recent-projects list to one entry per folder, newest kept, order
// preserved -- normalising as it goes. Cleans up duplicates written by older
// builds (whose dedup only dropped the folder being re-added, leaving stale
// pairs) so the Open Recent menu can't render two items with the same ImGui id.
static void dedupRecent(std::vector<std::string>& recent) {
    std::vector<std::string> out;
    out.reserve(recent.size());
    for (const std::string& e : recent) {
        std::string n = normProjectPath(e);
        if (std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(std::move(n));
    }
    recent.swap(out);
}

void loadPrefs(Context& ctx) {
    std::ifstream f(ctx.prefsPath);
    if (!f) return;
    nlohmann::json j;
    try { f >> j; } catch (const nlohmann::json::exception&) { return; }
    ctx.prefLocation   = j.value("lastLocation", ctx.prefLocation);
    ctx.recentProjects = j.value("recent", std::vector<std::string>{});
    dedupRecent(ctx.recentProjects); // drop duplicates left by older builds
    ctx.uiFontSize     = j.value("uiFontSize", ctx.uiFontSize);
    ctx.uiFontFamily   = j.value("uiFontFamily", std::string{});
}

void rememberProject(Context& ctx, const std::string& folder) {
    const std::string norm = normProjectPath(folder);
    ctx.prefLocation =
        std::filesystem::path(norm).parent_path().generic_string();
    // Front-load this project, then fully dedup: keeps the freshly-opened entry
    // (now first) and strips every other copy, current or stale.
    ctx.recentProjects.insert(ctx.recentProjects.begin(), norm);
    dedupRecent(ctx.recentProjects);
    if (ctx.recentProjects.size() > 8) ctx.recentProjects.resize(8);
    savePrefs(ctx);
}

std::string matsDirIn(const std::string& folder) { return folder + "/materials"; }

std::string sceneFileIn(const std::string& folder) {
    const std::string stem = std::filesystem::path(folder).filename().string();
    const std::string preferred = folder + "/" + stem + ".fitzel";
    std::error_code ec;
    if (std::filesystem::exists(preferred, ec)) return preferred;
    for (const auto& de : std::filesystem::directory_iterator(folder, ec))
        if (de.path().extension() == ".fitzel")
            return de.path().generic_string();
    return preferred;
}

std::vector<std::pair<std::string, std::string>>
listProjectsIn(const std::string& root) {
    std::vector<std::pair<std::string, std::string>> out;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(root, ec)) {
        if (!de.is_directory()) continue;
        const std::string folder = de.path().generic_string();
        bool hasScene = false;
        std::error_code e2;
        for (const auto& f : std::filesystem::directory_iterator(folder, e2))
            if (f.path().extension() == ".fitzel") { hasScene = true; break; }
        if (hasScene) out.push_back({de.path().filename().string(), folder});
    }
    std::sort(out.begin(), out.end());
    return out;
}

nlohmann::json writeEntityJson(const Entity& b,
                               const std::function<std::string(int)>& modelGuidOf) {
    nlohmann::json e;
    e["type"] = static_cast<int>(b.type);
    // Simple fields (transform, colour, physics, light params, name) come straight
    // from the property table -- one declaration drives save + UI.
    writeEntityProps(e, b);
    // Bespoke references the table can't own (id/parent, material/model in comps).
    e["id"]     = b.id;
    e["parent"] = b.parent;
    // Attached components: type id + their own serialization. A model component
    // also needs its source asset GUID, which only the loaded-model table resolves.
    if (!b.components.items.empty()) {
        nlohmann::json comps = nlohmann::json::array();
        for (const auto& c : b.components.items) {
            nlohmann::json cj;
            cj["type"] = c->typeId();
            c->save(cj);
            if (const auto* mc = dynamic_cast<const ModelComponent*>(c.get())) {
                const std::string guid = modelGuidOf ? modelGuidOf(mc->modelId)
                                                     : std::string();
                if (!guid.empty()) cj["model"] = guid;
            }
            comps.push_back(std::move(cj));
        }
        e["components"] = std::move(comps);
    }
    return e;
}

Entity readEntityJson(Context& ctx, const nlohmann::json& e) {
    Entity b;
    b.type = static_cast<EntityType>(e.value("type", 0));
    // Table-covered fields (transform, colour, physics, light, name).
    readEntityProps(e, b);
    // Bespoke references.
    b.id     = e.value("id", 0);
    b.parent = e.value("parent", -1);
    // Attached components (type registry -> instance, then its fields). A model
    // component resolves its source file + imports (needs the asset database),
    // which comp->load can't do on its own.
    if (e.contains("components") && e["components"].is_array()) {
        for (const auto& cj : e["components"]) {
            // The component id lives in "type" (a string). Older files have a
            // corrupted light component whose "type" is an INTEGER: the light's
            // point/spot enum was written over the id by a key clash (now fixed by
            // renaming that property to "lightType"). Recover such a component as a
            // light and restore its enum below.
            std::string ct;
            bool legacyLight = false;
            if (cj.contains("type")) {
                const auto& tj = cj.at("type");
                if (tj.is_string()) ct = tj.get<std::string>();
                else if (tj.is_number_integer()) { ct = "light"; legacyLight = true; }
            }
            auto comp = components::create(ct);
            if (!comp) continue;
            comp->load(cj);
            if (legacyLight)
                if (auto* lcp = dynamic_cast<LightComponent*>(comp.get()))
                    lcp->type = cj.at("type").get<int>(); // restore point/spot
            if (auto* mc = dynamic_cast<ModelComponent*>(comp.get())) {
                std::string mp;
                if (cj.contains("model"))
                    mp = ctx.assetDb.pathForId(
                             AssetId::fromString(cj["model"].get<std::string>()))
                             .string();
                if (mp.empty() && cj.contains("modelFile"))
                    mp = ctx.modelDir + "/" + cj["modelFile"].get<std::string>();
                if (!mp.empty()) {
                    mc->modelPath = mp;
                    // Structure-preserving import: a group root (node < 0 with no
                    // "model" ref) has no mesh; a child resolves its own node.
                    if (mc->nodeIndex >= 0)
                        mc->modelId = ctx.importModelNode(mp, mc->nodeIndex);
                    else if (cj.contains("model"))
                        mc->modelId = ctx.importModel(mp);
                }
            }
            b.components.items.push_back(std::move(comp));
        }
    }
    return b;
}

void saveScene(const Context& ctx, const std::string& path) {
    nlohmann::json j;
    j["version"] = 3;         // scene *format* version, bumped on layout changes
    j["app"]     = fitzel::kVersionFull; // the build that wrote it (diagnostics)
    nlohmann::json ents = nlohmann::json::array();
    const auto modelGuidOf = [&ctx](int modelId) -> std::string {
        LoadedModel* lm = ctx.loadedModelById(modelId);
        return (lm && lm->assetId.valid()) ? lm->assetId.toString() : std::string();
    };
    for (const Entity& b : ctx.entities)
        ents.push_back(writeEntityJson(b, modelGuidOf));
    j["entities"] = std::move(ents);
    if (ctx.writeSettings) {
        nlohmann::json s = nlohmann::json::object();
        ctx.writeSettings(s);
        j["settings"] = std::move(s);
    }
    std::ofstream f(path);
    if (f) f << j.dump(2) << '\n';
}

void writeProjectMaterials(const Context& ctx, const std::string& matsDir) {
    std::error_code ec;
    std::filesystem::create_directories(matsDir, ec);
    for (const auto& de : std::filesystem::directory_iterator(matsDir, ec)) {
        const std::string ext = de.path().extension().string();
        if (ext == ".fmat" || ext == ".meta")
            std::filesystem::remove(de.path(), ec);
    }
    for (const MaterialDef& md : ctx.materials)
        if (!md.fromModel) writeMaterialFile(md, matsDir);
}

void loadProjectMaterials(Context& ctx, const std::string& matsDir) {
    ctx.materials.clear();
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(matsDir, ec)) {
        if (de.path().extension().string() != ".fmat") continue;
        std::ifstream f(de.path());
        if (!f) continue;
        nlohmann::json m;
        try { f >> m; } catch (const nlohmann::json::exception&) { continue; }
        MaterialDef md;
        md.assetId = ctx.assetDb.idForPath(de.path().string());
        if (!md.assetId.valid()) md.assetId = AssetId::generate();
        md.name         = m.value("name", de.path().stem().string());
        md.albedo       = readVec3Json(m.value("albedo", nlohmann::json{}), md.albedo);
        md.tint         = readVec3Json(m.value("tint", nlohmann::json{}), md.tint);
        md.reflectivity = m.value("reflectivity", md.reflectivity);
        md.roughness    = m.value("roughness", md.roughness);
        md.opacity      = m.value("opacity", md.opacity);
        md.glass        = m.value("glass", md.glass);
        md.alphaMode    = static_cast<AlphaMode>(
                              m.value("alphaMode", static_cast<int>(md.alphaMode)));
        md.alphaCutoff  = m.value("alphaCutoff", md.alphaCutoff);
        md.emission     = readVec3Json(m.value("emission", nlohmann::json{}), md.emission);
        md.emissionStrength = m.value("emissionStrength", md.emissionStrength);
        if (m.contains("texture")) {
            md.texId = AssetId::fromString(m["texture"].get<std::string>());
            if (md.texId.valid()) md.tex = ctx.assetDb.loadTexture(md.texId);
        }
        if (m.contains("normalMap")) {
            md.normalTexId = AssetId::fromString(m["normalMap"].get<std::string>());
            if (md.normalTexId.valid()) md.normalTex = ctx.assetDb.loadTexture(md.normalTexId);
        }
        if (m.contains("emissionMap")) {
            md.emissionTexId = AssetId::fromString(m["emissionMap"].get<std::string>());
            if (md.emissionTexId.valid()) md.emissionTex = ctx.assetDb.loadTexture(md.emissionTexId);
        }
        ctx.materials.push_back(std::move(md));
    }
    if (ctx.materials.empty()) ctx.seedDefaultMaterials();
    ctx.matSel = 0;
}

// Inline "materials" array (legacy v2 scenes embedded their materials rather than
// shipping .fmat files). Rebuilds ctx.materials; no-op when the block is absent.
static void loadInlineMaterials(Context& ctx, const nlohmann::json& j) {
    if (!j.contains("materials") || !j["materials"].is_array() ||
        j["materials"].empty())
        return;
    ctx.materials.clear();
    for (const auto& m : j["materials"]) {
        MaterialDef md;
        md.assetId      = AssetId::generate();
        md.name         = m.value("name", std::string{});
        md.albedo       = readVec3Json(m.value("albedo", nlohmann::json{}), md.albedo);
        md.reflectivity = m.value("reflectivity", md.reflectivity);
        md.roughness    = m.value("roughness", md.roughness);
        md.opacity      = m.value("opacity", md.opacity);
        md.glass        = m.value("glass", md.glass);
        md.alphaMode    = static_cast<AlphaMode>(
                              m.value("alphaMode", static_cast<int>(md.alphaMode)));
        md.alphaCutoff  = m.value("alphaCutoff", md.alphaCutoff);
        md.emission     = readVec3Json(m.value("emission", nlohmann::json{}), md.emission);
        md.emissionStrength = m.value("emissionStrength", md.emissionStrength);
        if (m.contains("emissionMap")) {
            md.emissionTexId = AssetId::fromString(m["emissionMap"].get<std::string>());
            if (md.emissionTexId.valid())
                md.emissionTex = ctx.assetDb.loadTexture(md.emissionTexId);
        }
        if (m.contains("texture")) {
            md.texId = AssetId::fromString(m["texture"].get<std::string>());
            if (md.texId.valid()) md.tex = ctx.assetDb.loadTexture(md.texId);
        }
        if (m.contains("normalMap")) {
            md.normalTexId = AssetId::fromString(m["normalMap"].get<std::string>());
            if (md.normalTexId.valid()) md.normalTex = ctx.assetDb.loadTexture(md.normalTexId);
        }
        ctx.materials.push_back(std::move(md));
    }
    if (ctx.materials.empty()) ctx.seedDefaultMaterials();
    ctx.matSel = 0;
}

// The old space-separated text format: inline "M" materials + int ids. Small and
// legacy-only, so it's parsed in one shot rather than streamed.
static void loadLegacyTextScene(Context& ctx, const std::string& content, int& maxId) {
    ctx.materials.clear();
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tok;
        ss >> tok;
        if (tok == "M") {
            MaterialDef md;
            int oldId = 0;
            ss >> oldId >> md.albedo.x >> md.albedo.y >> md.albedo.z
               >> md.reflectivity >> md.roughness;
            std::getline(ss, md.name);
            if (!md.name.empty() && md.name[0] == ' ') md.name.erase(0, 1);
            md.assetId = AssetId::generate();
            ctx.materials.push_back(std::move(md));
            continue;
        }
        Entity b;
        int cast = 0, oldMat = 0;
        std::string modelTok, scriptTok;
        float dump = 0.0f; // fields dropped in the component migration
        b.type = static_cast<EntityType>(std::stoi(tok));
        ss >> b.localCenter.x >> b.localCenter.y >> b.localCenter.z
           >> b.half.x >> b.half.y >> b.half.z
           >> dump >> dump >> dump      // color (now LightComponent/material)
           >> dump >> dump >> dump      // intensity / range / shadowBias
           >> cast >> oldMat >> dump    // castShadows / material / scale
           >> b.localRotation.x >> b.localRotation.y >> b.localRotation.z >> modelTok
           >> scriptTok
           >> b.id >> b.parent;
        (void)cast; (void)scriptTok; (void)oldMat; // no longer stored here
        std::getline(ss, b.name);
        if (!b.name.empty() && b.name[0] == ' ') b.name.erase(0, 1);
        if (b.type == EntityType::Model && modelTok != "-") {
            auto mc = std::make_unique<ModelComponent>();
            mc->modelPath = ctx.modelDir + "/" + modelTok;
            mc->modelId   = ctx.importModel(mc->modelPath);
            b.components.items.push_back(std::move(mc));
        }
        maxId = std::max(maxId, b.id);
        ctx.entities.push_back(std::move(b));
    }
    if (ctx.materials.empty()) ctx.seedDefaultMaterials();
    ctx.matSel = 0;
}

// Post-pass shared by every load: entity id counter + the "exactly one Sun with a
// SunComponent" invariant (the Sun is engine-managed and can't be re-added via UI).
static void finalizeSceneLoad(Context& ctx, int maxId) {
    ctx.entityCounter = maxId + 1;
    Entity* sunE = nullptr;
    for (Entity& e : ctx.entities)
        if (e.type == EntityType::Sun) { sunE = &e; break; }
    if (!sunE) {
        Entity sun;
        sun.type = EntityType::Sun; sun.name = "Sun"; sun.id = ctx.entityCounter++;
        sun.components.items.push_back(std::make_unique<SunComponent>());
        ctx.entities.push_back(std::move(sun));
    } else if (!sunE->components.get<SunComponent>()) {
        sunE->components.items.push_back(std::make_unique<SunComponent>());
    }
    ctx.entitySel = -1;
}

// Shared entry point for both the synchronous loadScene and the incremental
// loader: reads + parses the file, resets the scene, and (for JSON) leaves the
// entity array in `load.j` ready to be streamed by stepLoad. Legacy text is small
// enough that it's fully parsed here. Returns false on a read/parse failure (with
// load.done=true, ok=false).
static bool startSceneLoad(Context& ctx, SceneLoad& load, const std::string& path) {
    load = SceneLoad{};
    load.scenePath = path;
    std::ifstream f(path);
    if (!f) { load.done = true; return false; }
    std::stringstream buf; buf << f.rdbuf();
    const std::string content = buf.str();
    const std::size_t firstCh = content.find_first_not_of(" \t\r\n");
    const bool isJson = firstCh != std::string::npos && content[firstCh] == '{';

    ctx.entities.clear();
    ctx.clearModels(); // models re-import fresh below

    if (!isJson) { // legacy text: parse it all now, then just finalize in stepLoad
        loadLegacyTextScene(ctx, content, load.maxId);
        load.entTotal = load.entIdx = ctx.entities.size();
        load.active = true;
        return true;
    }
    try { load.j = nlohmann::json::parse(content); }
    catch (const nlohmann::json::exception& ex) {
        std::fprintf(stderr, "Scene parse error: %s\n", ex.what());
        load.done = true;
        return false;
    }
    loadInlineMaterials(ctx, load.j);
    if (load.j.contains("entities") && load.j["entities"].is_array())
        load.entTotal = load.j["entities"].size();
    load.active   = true;
    load.progress = load.entTotal ? 0.02f : 1.0f;
    load.label    = "Loading objects...";
    return true;
}

void stepLoad(Context& ctx, SceneLoad& load, double budgetMs) {
    if (!load.active || load.done) return;
    if (load.j.contains("entities") && load.j["entities"].is_array()) {
        const auto& ents = load.j["entities"];
        const auto t0 = std::chrono::steady_clock::now();
        while (load.entIdx < load.entTotal) {
            Entity b = readEntityJson(ctx, ents[load.entIdx]);
            load.maxId = std::max(load.maxId, b.id);
            ctx.entities.push_back(std::move(b));
            ++load.entIdx;
            load.progress = 0.02f + 0.93f *
                static_cast<float>(load.entIdx) / static_cast<float>(load.entTotal);
            load.label = "Loading objects " + std::to_string(load.entIdx) + " / " +
                         std::to_string(load.entTotal);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
            if (ms >= budgetMs) return; // out of time -- resume next frame
        }
        if (ctx.readSettings && load.j.contains("settings"))
            ctx.readSettings(load.j["settings"]);
    }
    // All entities in: finalize + project bookkeeping.
    finalizeSceneLoad(ctx, load.maxId);
    ctx.currentProject = load.scenePath;
    if (load.fullOpen) {
        std::snprintf(ctx.projNameBuf, ctx.projNameBufSize, "%s",
                      std::filesystem::path(load.projectFolder).filename().string().c_str());
        rememberProject(ctx, load.projectFolder);
    }
    load.j       = nlohmann::json{}; // release the parsed scene
    load.progress = 1.0f;
    load.ok      = true;
    load.done    = true;
    load.active  = false;
}

bool beginOpenProject(Context& ctx, SceneLoad& load, const std::string& folder) {
    const std::string scene = sceneFileIn(folder);
    std::error_code ec;
    if (!std::filesystem::exists(scene, ec)) { load = SceneLoad{}; load.done = true; return false; }
    ctx.assetDb.mountProject(folder);
    ctx.assetDb.refresh();
    loadProjectMaterials(ctx, matsDirIn(folder));
    if (!startSceneLoad(ctx, load, scene)) return false;
    load.fullOpen      = true;
    load.projectFolder = folder;
    return true;
}

bool beginLoadScene(Context& ctx, SceneLoad& load, const std::string& scenePath) {
    std::error_code ec;
    if (scenePath.empty() || !std::filesystem::exists(scenePath, ec)) {
        load = SceneLoad{}; load.done = true; return false;
    }
    return startSceneLoad(ctx, load, scenePath);
}

bool loadScene(Context& ctx, const std::string& path) {
    // Synchronous path (player boot, scene triggers): drain the incremental loader
    // in one call with an unlimited per-step budget.
    SceneLoad load;
    if (!startSceneLoad(ctx, load, path)) return false;
    while (!load.done) stepLoad(ctx, load, 1e30);
    return load.ok;
}

void saveProjectTo(Context& ctx, const std::string& folder) {
    if (folder.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    writeProjectMaterials(ctx, matsDirIn(folder));
    // Save into the currently-open scene when it lives in this folder, so a
    // multi-scene project keeps each scene in its own file. Writing to the
    // folder's *default* scene here (as this once did) meant "Save Project" while
    // a second scene was open clobbered the main scene with it -- data loss.
    // Only a fresh save into a different folder (Save As / new project) falls
    // back to the default scene file.
    std::string scene = sceneFileIn(folder);
    if (!ctx.currentProject.empty() &&
        std::filesystem::path(ctx.currentProject).parent_path().lexically_normal() ==
            std::filesystem::path(folder).lexically_normal())
        scene = ctx.currentProject;
    saveScene(ctx, scene);
    ctx.assetDb.mountProject(folder);
    ctx.assetDb.refresh();
    ctx.currentProject = scene;
    std::snprintf(ctx.projNameBuf, ctx.projNameBufSize, "%s",
                  std::filesystem::path(folder).filename().string().c_str());
    rememberProject(ctx, folder);
}

void saveCurrent(Context& ctx) {
    if (!ctx.currentProject.empty())
        saveProjectTo(ctx, std::filesystem::path(ctx.currentProject)
                               .parent_path().generic_string());
}

namespace {
// A 32-hex-char string is an AssetId GUID (how scenes/materials reference models
// and textures). Sound/road/tree refs are bare filenames instead.
bool looksLikeGuid(const std::string& s) {
    if (s.size() != 32) return false;
    for (unsigned char c : s) if (!std::isxdigit(c)) return false;
    return true;
}
// A string that names an asset file we might have to ship (by extension).
bool hasAssetExt(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return false;
    std::string e = name.substr(dot + 1);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* const exts[] = {
        "png", "jpg", "jpeg", "tga", "bmp", "exr", "hdr", // textures
        "gltf", "glb", "dae", "fbx", "obj",               // models
        "bin", "mtl"                                       // model companions
    };
    for (const char* x : exts) if (e == x) return true;
    return false;
}
std::string lowerCopy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// Every string value in a JSON tree (scene / material / prefab), recursively.
void collectJsonStrings(const nlohmann::json& j, std::vector<std::string>& out) {
    if (j.is_string())      out.push_back(j.get<std::string>());
    else if (j.is_array())  { for (const auto& e : j) collectJsonStrings(e, out); }
    else if (j.is_object()) { for (const auto& it : j.items()) collectJsonStrings(it.value(), out); }
}
// Quoted string literals out of Lua source (both "..." and '...'), so a script
// that names an asset keeps it from being trimmed away. Best-effort.
void collectLuaStrings(const std::string& src, std::vector<std::string>& out) {
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char q = src[i];
        if (q != '"' && q != '\'') continue;
        std::string s;
        for (++i; i < src.size() && src[i] != q; ++i) {
            if (src[i] == '\\' && i + 1 < src.size()) { s.push_back(src[++i]); }
            else s.push_back(src[i]);
        }
        out.push_back(s);
    }
}

// "Export only used assets": copy just the content the project references into
// out/content, instead of the whole library. Sounds are copied wholesale (small,
// and script-driven), and every scene/prefab/material/.lua is walked for asset
// references (GUIDs -> pathForId, filenames -> basename lookup). Model companion
// files that share a stem (e.g. a .gltf's .bin) ride along.
void copyUsedContent(Context& ctx, const std::filesystem::path& out,
                     const std::filesystem::path& projDir, const game::Settings& gs,
                     const std::string& bootScene, std::error_code& ec) {
    namespace fs = std::filesystem;
    const fs::path contentRoot = fs::weakly_canonical(fs::path(ctx.contentRoot), ec);

    auto sceneKept = [&](const std::string& stem) {
        if (gs.exportScenes.empty()) return true;
        return stem == bootScene ||
               std::find(gs.exportScenes.begin(), gs.exportScenes.end(), stem) !=
                   gs.exportScenes.end();
    };

    // 1) Gather every asset-reference token from the kept project files.
    std::vector<std::string> tokens;
    std::error_code de;
    for (fs::recursive_directory_iterator it(projDir, de), end; it != end; it.increment(de)) {
        if (de || !it->is_regular_file()) continue;
        const std::string ext = lowerCopy(it->path().extension().string());
        if (ext == ".fitzel") {
            if (!sceneKept(it->path().stem().string())) continue;
        } else if (ext != ".fprefab" && ext != ".fmat" && ext != ".lua") {
            continue;
        }
        std::ifstream f(it->path(), std::ios::binary);
        if (!f) continue;
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (ext == ".lua") {
            collectLuaStrings(body, tokens);
        } else {
            try { collectJsonStrings(nlohmann::json::parse(body), tokens); }
            catch (const nlohmann::json::exception&) {}
        }
    }

    // 2) Resolve tokens to content files. GUIDs via the database; filenames via a
    //    basename lookup over every scanned asset.
    std::unordered_map<std::string, fs::path> byName;
    for (const fitzel::AssetId id : ctx.assetDb.allAssets())
        if (const auto* e = ctx.assetDb.entry(id))
            byName[lowerCopy(e->absPath.filename().string())] = e->absPath;

    std::set<fs::path> keep;
    for (const std::string& tok : tokens) {
        if (looksLikeGuid(tok)) {
            const fitzel::AssetId id = fitzel::AssetId::fromString(tok);
            if (!id.valid()) continue;
            const fs::path p = ctx.assetDb.pathForId(id);
            if (!p.empty()) keep.insert(fs::weakly_canonical(p, ec));
        } else if (hasAssetExt(tok)) {
            const auto hit = byName.find(lowerCopy(fs::path(tok).filename().string()));
            if (hit != byName.end()) keep.insert(fs::weakly_canonical(hit->second, ec));
        }
    }

    // 3) Copy: sounds wholesale, then each kept file (+ stem-siblings). Anything
    //    that isn't under the content root is a project asset already shipped in
    //    out/project, so it's skipped here.
    const auto rec = fs::copy_options::recursive | fs::copy_options::overwrite_existing;
    if (fs::exists(contentRoot / "sounds", ec))
        fs::copy(contentRoot / "sounds", out / "content" / "sounds", rec, ec);

    auto shipUnderContent = [&](const fs::path& src) {
        std::error_code re;
        const fs::path rel = fs::relative(src, contentRoot, re);
        const std::string relS = rel.generic_string();
        if (re || relS.empty() || relS.rfind("..", 0) == 0) return; // outside content
        if (relS.rfind("sounds/", 0) == 0) return;                  // already whole
        const fs::path dst = out / "content" / rel;
        fs::create_directories(dst.parent_path(), re);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, re);
    };
    for (const fs::path& src : keep) {
        shipUnderContent(src);
        std::error_code se;
        for (const auto& sib : fs::directory_iterator(src.parent_path(), se))
            if (!se && sib.is_regular_file() && sib.path() != src &&
                sib.path().stem() == src.stem())
                shipUnderContent(sib.path());
    }
}
} // namespace

void exportGame(Context& ctx, const std::string& outDir) {
    namespace fs = std::filesystem;
    if (ctx.currentProject.empty()) {
        ctx.exportStatus = "Save the project first.";
        return;
    }
    std::error_code ec;
    const fs::path exeDir  = fs::current_path();
    const fs::path out     = fs::path(outDir);
    const fs::path projDir = fs::path(ctx.currentProject).parent_path();
    fs::create_directories(out, ec);
    // Per-project game settings drive the export: exe name, splash, start scene
    // and which scenes to bundle. Empty fields fall back to today's behaviour.
    const game::Settings gs = game::load(projDir.generic_string());
    const std::string game =
        !gs.exeName.empty()  ? gs.exeName
        : ctx.projNameBuf[0] ? std::string(ctx.projNameBuf)
                             : std::string("game");
    const auto rec = fs::copy_options::recursive |
                     fs::copy_options::overwrite_existing;
    // Ship the editor-free player, not the editor itself. It lives next to the
    // editor in the same bin/ dir; if it's missing (player target not built),
    // stop with a clear message rather than shipping a broken export.
    const fs::path player = exeDir / "player.exe";
    if (!fs::exists(player, ec)) {
        ctx.exportStatus =
            "player.exe not found next to the editor - build the 'player' target "
            "(build-release.bat builds both) and export again.";
        std::fprintf(stderr, "[Fitzel] %s\n", ctx.exportStatus.c_str());
        return;
    }
    fs::copy_file(player, out / (game + ".exe"),
                  fs::copy_options::overwrite_existing, ec);
    fs::copy(exeDir / "assets", out / "assets", rec, ec);

    // The boot scene: the configured start scene, else the project's default
    // (<project>.fitzel). Always bundled, whatever the export-scene selection is.
    const std::string bootScene =
        !gs.startScene.empty() ? gs.startScene : projDir.filename().string();

    // Content: the whole library, or -- with "export only used assets" -- just
    // the models/textures the project actually references.
    if (gs.trimAssets)
        copyUsedContent(ctx, out, projDir, gs, bootScene, ec);
    else
        fs::copy(ctx.contentRoot, out / "content", rec, ec);
    fs::copy(projDir, out / "project", rec, ec);

    // Optional scene filter: with an explicit export list, drop every other
    // .fitzel from the copied project (materials/models/scripts stay shared). An
    // empty list means "all scenes", so nothing is removed.
    if (!gs.exportScenes.empty()) {
        std::error_code e2;
        for (const auto& de : fs::directory_iterator(out / "project", e2)) {
            if (de.path().extension() != ".fitzel") continue;
            const std::string stem = de.path().stem().string();
            const bool keep = stem == bootScene ||
                std::find(gs.exportScenes.begin(), gs.exportScenes.end(), stem) !=
                    gs.exportScenes.end();
            if (!keep) {
                fs::remove(de.path(), e2);
                fs::remove(de.path().string() + ".meta", e2); // ignore if absent
            }
        }
    }

    // A custom splash overrides the engine default the player loads from
    // assets/splash.png.
    if (!gs.splash.empty()) {
        const fs::path src = projDir / gs.splash;
        if (fs::exists(src, ec))
            fs::copy_file(src, out / "assets" / "splash.png",
                          fs::copy_options::overwrite_existing, ec);
    }

    nlohmann::json gj;
    gj["project"]    = "project";
    gj["fullscreen"] = true;
    gj["startScene"] = gs.startScene; // "" => player uses the default scene
    std::ofstream(out / "game.json") << gj.dump(2);
    ctx.exportStatus = ec ? ("Export finished with warnings: " + ec.message())
                          : ("Exported to " + out.generic_string());
    std::fprintf(stderr, "[Fitzel] %s\n", ctx.exportStatus.c_str());
}

bool openProjectFolder(Context& ctx, const std::string& folder) {
    const std::string scene = sceneFileIn(folder);
    std::error_code ec;
    if (!std::filesystem::exists(scene, ec)) return false;
    ctx.assetDb.mountProject(folder);
    ctx.assetDb.refresh();
    loadProjectMaterials(ctx, matsDirIn(folder));
    if (!loadScene(ctx, scene)) return false;
    ctx.currentProject = scene;
    std::snprintf(ctx.projNameBuf, ctx.projNameBufSize, "%s",
                  std::filesystem::path(folder).filename().string().c_str());
    rememberProject(ctx, folder);
    return true;
}

void newProject(Context& ctx) {
    ctx.entities.clear();
    ctx.clearModels();
    ctx.materials.clear();
    ctx.seedDefaultMaterials();
    ctx.matSel = 0;
    ctx.entityCounter = 0;
    Entity sun;
    sun.type = EntityType::Sun; sun.name = "Sun"; sun.id = ctx.entityCounter++;
    sun.components.items.push_back(std::make_unique<SunComponent>());
    ctx.entities.push_back(std::move(sun));
    ctx.entitySel = -1;
    ctx.currentProject.clear();
    ctx.projNameBuf[0] = '\0';
    ctx.assetDb.unmountProjects();
    ctx.assetDb.refresh();
}

std::vector<std::pair<std::string, std::string>>
listScenesIn(const std::string& folder) {
    std::vector<std::pair<std::string, std::string>> out;
    if (folder.empty()) return out;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(folder, ec))
        if (de.path().extension() == ".fitzel")
            out.push_back({de.path().stem().string(), de.path().generic_string()});
    std::sort(out.begin(), out.end());
    return out;
}

bool loadSceneFile(Context& ctx, const std::string& scenePath) {
    std::error_code ec;
    if (scenePath.empty() || !std::filesystem::exists(scenePath, ec)) return false;
    if (!loadScene(ctx, scenePath)) return false;
    ctx.currentProject = scenePath;
    return true;
}

std::string newSceneInProject(Context& ctx, const std::string& folder,
                              const std::string& name) {
    if (folder.empty() || name.empty()) return {};
    const std::string path = folder + "/" + safeName(name) + ".fitzel";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return {}; // never clobber an existing scene
    // Fresh scene: clear entities + runtime models, keep the project's materials
    // and mounted assets (scenes share them). One Sun, like a brand-new project.
    // The current world settings (terrain, environment, road) are captured into the
    // new file by saveScene, so a new scene starts from the world you're looking at.
    ctx.entities.clear();
    ctx.clearModels();
    ctx.entityCounter = 0;
    Entity sun;
    sun.type = EntityType::Sun; sun.name = "Sun"; sun.id = ctx.entityCounter++;
    sun.components.items.push_back(std::make_unique<SunComponent>());
    ctx.entities.push_back(std::move(sun));
    ctx.entitySel = -1;
    saveScene(ctx, path);
    ctx.currentProject = path;
    return path;
}

std::string renameScene(Context& ctx, const std::string& scenePath,
                        const std::string& newName) {
    if (scenePath.empty() || newName.empty()) return {};
    namespace fs = std::filesystem;
    const fs::path src(scenePath);
    const fs::path dst = src.parent_path() / (safeName(newName) + ".fitzel");
    std::error_code ec;
    if (dst == src) return src.generic_string();      // no-op rename
    if (fs::exists(dst, ec)) return {};               // target name already taken
    fs::rename(src, dst, ec);
    if (ec) return {};
    // Carry a .meta sidecar along if one exists (scenes have none today; be safe).
    if (fs::exists(src.string() + ".meta", ec))
        fs::rename(src.string() + ".meta", dst.string() + ".meta", ec);
    const std::string dstStr = dst.generic_string();
    if (ctx.currentProject == scenePath) ctx.currentProject = dstStr;
    return dstStr;
}

bool deleteSceneFile(const std::string& scenePath) {
    if (scenePath.empty()) return false;
    std::error_code ec;
    std::filesystem::remove(scenePath + ".meta", ec); // ignore if absent
    return std::filesystem::remove(scenePath, ec);
}

bool mergeSceneSettings(const std::string& scenePath, const nlohmann::json& keys) {
    if (scenePath.empty() || !keys.is_object()) return false;
    nlohmann::json j;
    {
        std::ifstream f(scenePath);
        if (!f) return false;
        try { f >> j; } catch (const nlohmann::json::exception&) { return false; }
    }
    if (!j.is_object()) return false; // not a v3 scene (old text format)
    if (!j.contains("settings") || !j["settings"].is_object())
        j["settings"] = nlohmann::json::object();
    for (const auto& [k, v] : keys.items()) j["settings"][k] = v;
    std::ofstream out(scenePath);
    if (!out) return false;
    out << j.dump(2) << '\n';
    return static_cast<bool>(out);
}

} // namespace projectio
