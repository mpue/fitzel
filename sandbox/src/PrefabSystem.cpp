#include "PrefabSystem.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <fitzel/Version.hpp>
#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/asset/Vfs.hpp>

#include "Component.hpp"

using fitzel::AssetId;

namespace prefab {

namespace {

// Ids of `rootId` and all its descendants, breadth-first (parents before
// children), so the collected order is safe for AddEntitiesCmd and id-remapping.
std::vector<int> subtreeIds(const std::vector<Entity>& scene, int rootId) {
    std::vector<int> ids{rootId};
    for (std::size_t k = 0; k < ids.size(); ++k)
        for (const Entity& e : scene)
            if (e.parent == ids[k]) ids.push_back(e.id);
    return ids;
}

const Entity* findEntity(const std::vector<Entity>& scene, int id) {
    for (const Entity& e : scene)
        if (e.id == id) return &e;
    return nullptr;
}

void stripPrefabTag(Entity& e) {
    auto& items = e.components.items;
    items.erase(std::remove_if(items.begin(), items.end(),
                    [](const std::unique_ptr<ComponentBase>& c) {
                        return std::string(c->typeId()) == "prefab";
                    }),
                items.end());
}

} // namespace

std::string prefabsDirIn(const std::string& projectFolder) {
    return projectFolder + "/prefabs";
}

std::optional<Prefab> fromSubtree(const std::vector<Entity>& scene, int rootId,
                                  const std::string& name) {
    const Entity* root = findEntity(scene, rootId);
    if (!root || root->type == EntityType::Sun) return std::nullopt;

    const std::vector<int> ids = subtreeIds(scene, rootId);
    // Old id -> local id (root first, so it becomes 0).
    std::unordered_map<int, int> local;
    local.reserve(ids.size());
    for (int id : ids) local[id] = static_cast<int>(local.size());

    Prefab p;
    p.name = name.empty() ? (root->name.empty() ? "Prefab" : root->name) : name;
    p.guid = AssetId::generate();
    p.entities.reserve(ids.size());
    for (int id : ids) {
        const Entity* src = findEntity(scene, id);
        if (!src) continue;
        Entity e = *src;                 // deep-clones the component list
        e.id     = local[id];
        e.parent = (id == rootId) ? -1 : local[src->parent];
        stripPrefabTag(e);
        p.entities.push_back(std::move(e));
    }
    return p;
}

bool save(const projectio::Context& ctx, Prefab& p, const std::string& dir) {
    if (p.entities.empty()) return false;
    if (!p.guid.valid()) p.guid = AssetId::generate();

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const auto modelGuidOf = [&ctx](int modelId) -> std::string {
        LoadedModel* lm = ctx.loadedModelById(modelId);
        return (lm && lm->assetId.valid()) ? lm->assetId.toString() : std::string();
    };

    nlohmann::json ents = nlohmann::json::array();
    for (const Entity& e : p.entities)
        ents.push_back(projectio::writeEntityJson(e, modelGuidOf));

    nlohmann::json j;
    j["version"] = 1;                     // prefab format version
    j["app"]     = fitzel::kVersionFull;  // the build that wrote it (diagnostics)
    j["prefab"]  = {
        {"name", p.name},
        {"guid", p.guid.toString()},
        {"entities", std::move(ents)},
    };

    const std::string file = dir + "/" + projectio::safeName(p.name) + "-" +
                             p.guid.toString().substr(0, 8) + ".fprefab";
    std::ofstream f(file);
    if (!f) return false;
    f << j.dump(2) << '\n';
    p.path = std::filesystem::path(file).generic_string();
    return true;
}

std::optional<Prefab> load(projectio::Context& ctx, const std::string& path) {
    const std::string body = fitzel::vfs::readText(path);
    if (body.empty()) return std::nullopt;
    nlohmann::json j;
    try { j = nlohmann::json::parse(body); }
    catch (const nlohmann::json::exception&) { return std::nullopt; }
    if (!j.contains("prefab") || !j["prefab"].is_object()) return std::nullopt;
    const nlohmann::json& pj = j["prefab"];

    Prefab p;
    p.path = std::filesystem::path(path).generic_string();
    p.name = pj.value("name", std::string("Prefab"));
    p.guid = AssetId::fromString(pj.value("guid", std::string()));
    if (!p.guid.valid()) p.guid = AssetId::generate();
    for (const auto& ej : pj.value("entities", nlohmann::json::array()))
        p.entities.push_back(projectio::readEntityJson(ctx, ej));
    if (p.entities.empty()) return std::nullopt;
    return p;
}

std::vector<std::pair<std::string, std::string>> list(const std::string& dir) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& file : fitzel::vfs::listFiles(dir, false)) {
        const std::filesystem::path de(file);
        if (de.extension() != ".fprefab") continue;
        // Prefer the display name stored inside the (small) file; fall back to the
        // filename stem if it can't be read.
        std::string name = de.stem().string();
        if (const std::string body = fitzel::vfs::readText(file); !body.empty()) {
            try {
                const nlohmann::json j = nlohmann::json::parse(body);
                if (j.contains("prefab") && j["prefab"].is_object())
                    name = j["prefab"].value("name", name);
            } catch (const nlohmann::json::exception&) {}
        }
        out.push_back({name, de.generic_string()});
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Entity> instantiate(const Prefab& p, int& entityCounter,
                                const glm::vec3& pos, float yawDeg) {
    std::vector<Entity> out;
    out.reserve(p.entities.size());

    // Local id -> fresh scene id (one pass first, so parents resolve regardless of
    // order; fromSubtree already guarantees parent-before-child though).
    std::unordered_map<int, int> remap;
    remap.reserve(p.entities.size());
    for (const Entity& src : p.entities) remap[src.id] = entityCounter++;

    for (const Entity& src : p.entities) {
        Entity e = src;                  // deep-clones the component list
        e.id     = remap[src.id];
        e.parent = (src.parent < 0)
                       ? -1
                       : (remap.count(src.parent) ? remap[src.parent] : -1);

        // Tag as a prefab instance (localId = its id within the prefab).
        stripPrefabTag(e);
        auto pc = std::make_unique<PrefabComponent>();
        pc->source  = p.guid;
        pc->localId = src.id;
        e.components.items.push_back(std::move(pc));

        if (src.parent < 0) { // the root: drop it into the world at `pos`
            e.localCenter = e.center = pos;
            e.localRotation.y += yawDeg;
            e.rotation = e.localRotation;
            e.parent   = -1;
        }
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace prefab
