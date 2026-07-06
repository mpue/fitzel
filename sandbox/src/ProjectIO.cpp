#include "ProjectIO.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetDatabase.hpp>

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
    m["reflectivity"] = md.reflectivity;
    m["roughness"]    = md.roughness;
    m["opacity"]      = md.opacity;
    m["glass"]        = md.glass;
    if (md.texId.valid())       m["texture"]   = md.texId.toString();
    if (md.normalTexId.valid()) m["normalMap"] = md.normalTexId.toString();
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
    std::ofstream f(ctx.prefsPath);
    if (f) f << j.dump(2) << '\n';
}

void loadPrefs(Context& ctx) {
    std::ifstream f(ctx.prefsPath);
    if (!f) return;
    nlohmann::json j;
    try { f >> j; } catch (const nlohmann::json::exception&) { return; }
    ctx.prefLocation   = j.value("lastLocation", ctx.prefLocation);
    ctx.recentProjects = j.value("recent", std::vector<std::string>{});
}

void rememberProject(Context& ctx, const std::string& folder) {
    ctx.prefLocation =
        std::filesystem::path(folder).parent_path().generic_string();
    ctx.recentProjects.erase(
        std::remove(ctx.recentProjects.begin(), ctx.recentProjects.end(), folder),
        ctx.recentProjects.end());
    ctx.recentProjects.insert(ctx.recentProjects.begin(), folder);
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

void saveScene(const Context& ctx, const std::string& path) {
    nlohmann::json j;
    j["version"] = 3;
    nlohmann::json ents = nlohmann::json::array();
    for (const Entity& b : ctx.entities) {
        nlohmann::json e;
        e["type"] = static_cast<int>(b.type);
        // Simple fields (transform, colour, physics, light params, name) come
        // straight from the property table -- one declaration drives save + UI.
        writeEntityProps(e, b);
        // Bespoke references the table can't own (material/model in components).
        e["id"]     = b.id;
        e["parent"] = b.parent;
        // Attached components: type id + their own serialization. A model
        // component also needs its asset GUID, which only the database resolves.
        if (!b.components.items.empty()) {
            nlohmann::json comps = nlohmann::json::array();
            for (const auto& c : b.components.items) {
                nlohmann::json cj;
                cj["type"] = c->typeId();
                c->save(cj);
                if (const auto* mc = dynamic_cast<const ModelComponent*>(c.get()))
                    if (LoadedModel* lm = ctx.loadedModelById(mc->modelId);
                        lm && lm->assetId.valid())
                        cj["model"] = lm->assetId.toString();
                comps.push_back(std::move(cj));
            }
            e["components"] = std::move(comps);
        }
        ents.push_back(std::move(e));
    }
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
        md.reflectivity = m.value("reflectivity", md.reflectivity);
        md.roughness    = m.value("roughness", md.roughness);
        md.opacity      = m.value("opacity", md.opacity);
        md.glass        = m.value("glass", md.glass);
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

bool loadScene(Context& ctx, const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream buf; buf << f.rdbuf();
    const std::string content = buf.str();
    const std::size_t firstCh = content.find_first_not_of(" \t\r\n");
    const bool isJson =
        firstCh != std::string::npos && content[firstCh] == '{';

    ctx.entities.clear();
    ctx.clearModels(); // models re-import fresh below
    int maxId = -1;
    std::map<int, AssetId> legacyMat; // legacy int material id -> synthesized GUID

    if (isJson) {
        nlohmann::json j;
        try { j = nlohmann::json::parse(content); }
        catch (const nlohmann::json::exception& ex) {
            std::fprintf(stderr, "Scene parse error: %s\n", ex.what());
            return false;
        }
        if (j.contains("materials") && j["materials"].is_array() &&
            !j["materials"].empty()) {
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
                if (m.contains("texture")) {
                    md.texId = AssetId::fromString(m["texture"].get<std::string>());
                    if (md.texId.valid()) md.tex = ctx.assetDb.loadTexture(md.texId);
                }
                if (m.contains("normalMap")) {
                    md.normalTexId = AssetId::fromString(m["normalMap"].get<std::string>());
                    if (md.normalTexId.valid()) md.normalTex = ctx.assetDb.loadTexture(md.normalTexId);
                }
                legacyMat[m.value("id", 0)] = md.assetId;
                ctx.materials.push_back(std::move(md));
            }
            if (ctx.materials.empty()) ctx.seedDefaultMaterials();
            ctx.matSel = 0;
        }
        for (const auto& e : j.value("entities", nlohmann::json::array())) {
            Entity b;
            b.type = static_cast<EntityType>(e.value("type", 0));
            // Table-covered fields (transform, colour, physics, light, name).
            readEntityProps(e, b);
            // Bespoke references.
            b.id     = e.value("id", 0);
            b.parent = e.value("parent", -1);
            // Attached components (type registry -> instance, then its fields). A
            // model component resolves its source file + imports (needs the asset
            // database), which comp->load can't do on its own.
            if (e.contains("components") && e["components"].is_array()) {
                for (const auto& cj : e["components"]) {
                    const std::string ct = cj.value("type", std::string{});
                    auto comp = components::create(ct);
                    if (!comp) continue;
                    comp->load(cj);
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
                            mc->modelId   = ctx.importModel(mp);
                        }
                    }
                    b.components.items.push_back(std::move(comp));
                }
            }
            maxId = std::max(maxId, b.id);
            ctx.entities.push_back(std::move(b));
        }
        if (ctx.readSettings && j.contains("settings"))
            ctx.readSettings(j["settings"]);
    } else {
        // Legacy space-separated text: inline "M" materials + int ids.
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
                legacyMat[oldId] = md.assetId;
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
            ctx.entities.push_back(b);
        }
        if (ctx.materials.empty()) ctx.seedDefaultMaterials();
        ctx.matSel = 0;
    }

    ctx.entityCounter = maxId + 1;
    // Invariant: exactly one Sun, and it always carries its SunComponent (which
    // is engine-managed, so it can't be re-added through the UI).
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
    return true;
}

void saveProjectTo(Context& ctx, const std::string& folder) {
    if (folder.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    writeProjectMaterials(ctx, matsDirIn(folder));
    const std::string scene = sceneFileIn(folder);
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

void exportGame(Context& ctx, const std::string& outDir) {
    namespace fs = std::filesystem;
    if (ctx.currentProject.empty()) {
        ctx.exportStatus = "Save the project first.";
        return;
    }
    std::error_code ec;
    const fs::path exeDir = fs::current_path();
    const fs::path out    = fs::path(outDir);
    fs::create_directories(out, ec);
    const std::string game =
        ctx.projNameBuf[0] ? std::string(ctx.projNameBuf) : std::string("game");
    const auto rec = fs::copy_options::recursive |
                     fs::copy_options::overwrite_existing;
    fs::copy_file(exeDir / "sandbox.exe", out / (game + ".exe"),
                  fs::copy_options::overwrite_existing, ec);
    fs::copy(exeDir / "assets", out / "assets", rec, ec);
    fs::copy(ctx.contentRoot,   out / "content", rec, ec);
    fs::copy(fs::path(ctx.currentProject).parent_path(), out / "project", rec, ec);
    nlohmann::json gj;
    gj["project"]    = "project";
    gj["fullscreen"] = true;
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

} // namespace projectio
