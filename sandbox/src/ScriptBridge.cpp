#include "ScriptBridge.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include <fitzel/asset/AssetDatabase.hpp>

#include "Component.hpp"
#include "Document.hpp"
#include "ModelLibrary.hpp"
#include "SceneTypes.hpp"

using fitzel::AssetDatabase;
using fitzel::AssetId;
using fitzel::AssetType;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

AssetType typeFromName(const std::string& s) {
    const std::string t = lower(s);
    if (t == "texture")  return AssetType::Texture;
    if (t == "model")    return AssetType::Model;
    if (t == "sound")    return AssetType::Sound;
    if (t == "material") return AssetType::Material;
    return AssetType::Unknown; // also what "" (any) maps to
}

// Resolve a script-supplied asset reference -- a GUID, a file name ("brick.png"),
// a relative path ("textures/brick.png") or a stem ("brick") -- to an id.
// `want` restricts the search to one type (Unknown = any). Invalid id if nothing
// matches. Exact file-name matches beat path suffixes, which beat stems, so
// "brick.png" never resolves to "brick_normal.png" while a stem still works.
AssetId resolveAsset(AssetDatabase& db, const std::string& ref, AssetType want) {
    if (ref.empty()) return {};
    if (ref.size() == 32) {
        const AssetId gid = AssetId::fromString(ref);
        if (gid.valid() && db.entry(gid)) return gid;
    }
    const std::string needle = lower(ref);
    AssetId byPath, byStem;
    for (const AssetId& id : db.allAssets()) {
        const AssetDatabase::Entry* e = db.entry(id);
        if (!e) continue;
        if (want != AssetType::Unknown && e->type != want) continue;
        const std::string file = lower(e->absPath.filename().string());
        if (file == needle) return id;
        const std::string rel = lower(e->relPath);
        if (!byPath.valid() && (rel == needle ||
                                (rel.size() > needle.size() &&
                                 rel.compare(rel.size() - needle.size(), needle.size(),
                                             needle) == 0 &&
                                 rel[rel.size() - needle.size() - 1] == '/')))
            byPath = id;
        if (!byStem.valid() && lower(e->absPath.stem().string()) == needle) byStem = id;
    }
    if (byPath.valid()) return byPath;
    return byStem;
}

ScriptAssetInfo assetRow(const AssetDatabase& db, const AssetDatabase::Entry& e) {
    ScriptAssetInfo r;
    r.id   = e.id.toString();
    r.name = e.absPath.filename().string();
    r.path = e.relPath;
    r.type = fitzel::assetTypeName(e.type);
    r.source = (e.sourceIndex >= 0 &&
                e.sourceIndex < static_cast<int>(db.sources().size()) &&
                db.sources()[e.sourceIndex].kind == fitzel::AssetSourceKind::Project)
                   ? "Project" : "Engine";
    return r;
}

ScriptMaterialInfo materialRow(const MaterialDef& m) {
    ScriptMaterialInfo r;
    r.id               = m.assetId.toString();
    r.name             = m.name;
    r.albedo           = m.albedo;
    r.reflectivity     = m.reflectivity;
    r.roughness        = m.roughness;
    r.opacity          = m.opacity;
    r.glass            = m.glass;
    r.alphaMode        = static_cast<int>(m.alphaMode);
    r.alphaCutoff      = m.alphaCutoff;
    r.emission         = m.emission;
    r.emissionStrength = m.emissionStrength;
    r.texture          = m.texId.valid() ? m.texId.toString() : std::string();
    r.normalMap        = m.normalTexId.valid() ? m.normalTexId.toString() : std::string();
    r.emissionMap      = m.emissionTexId.valid() ? m.emissionTexId.toString() : std::string();
    r.fromModel        = m.fromModel;
    return r;
}

// Find a material by GUID (or by name, so scripts can say "Asphalt").
MaterialDef* findMaterialDef(Document& doc, const std::string& ref) {
    if (ref.empty()) return nullptr;
    const AssetId gid = AssetId::fromString(ref);
    for (MaterialDef& m : doc.materials())
        if (gid.valid() && m.assetId == gid) return &m;
    const std::string needle = lower(ref);
    for (MaterialDef& m : doc.materials())
        if (lower(m.name) == needle) return &m;
    return nullptr;
}

// Apply the engaged fields of an edit. Map slots take an asset reference; an
// empty string clears the slot.
void applyEdit(AssetDatabase& db, MaterialDef& m, const ScriptMaterialEdit& ed) {
    if (ed.name)             m.name             = *ed.name;
    if (ed.albedo)           m.albedo           = *ed.albedo;
    if (ed.reflectivity)     m.reflectivity     = glm::clamp(*ed.reflectivity, 0.0f, 1.0f);
    if (ed.roughness)        m.roughness        = glm::clamp(*ed.roughness, 0.0f, 1.0f);
    if (ed.opacity)          m.opacity          = glm::clamp(*ed.opacity, 0.0f, 1.0f);
    if (ed.glass)            m.glass            = *ed.glass;
    if (ed.alphaMode)        m.alphaMode        = static_cast<AlphaMode>(
                                                     glm::clamp(*ed.alphaMode, 0, 2));
    if (ed.alphaCutoff)      m.alphaCutoff      = glm::clamp(*ed.alphaCutoff, 0.0f, 1.0f);
    if (ed.emission)         m.emission         = *ed.emission;
    if (ed.emissionStrength) m.emissionStrength = glm::max(*ed.emissionStrength, 0.0f);

    auto slot = [&](const std::optional<std::string>& ref,
                    std::shared_ptr<fitzel::Texture>& tex, AssetId& texId) {
        if (!ref) return;
        if (ref->empty()) { tex.reset(); texId = {}; return; }
        const AssetId gid = resolveAsset(db, *ref, AssetType::Texture);
        if (!gid.valid()) return;
        if (auto t = db.loadTexture(gid)) { tex = std::move(t); texId = gid; }
    };
    slot(ed.texture,     m.tex,         m.texId);
    slot(ed.normalMap,   m.normalTex,   m.normalTexId);
    slot(ed.emissionMap, m.emissionTex, m.emissionTexId);
}

// Ray vs. axis-aligned box (slab test). Returns the near hit distance, or -1.
float rayBox(const glm::vec3& o, const glm::vec3& d, const glm::vec3& lo,
             const glm::vec3& hi) {
    float tmin = 0.0f, tmax = 1e30f;
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < 1e-6f) {
            if (o[a] < lo[a] || o[a] > hi[a]) return -1.0f;
            continue;
        }
        const float inv = 1.0f / d[a];
        float t0 = (lo[a] - o[a]) * inv, t1 = (hi[a] - o[a]) * inv;
        if (t0 > t1) std::swap(t0, t1);
        tmin = glm::max(tmin, t0);
        tmax = glm::min(tmax, t1);
        if (tmax < tmin) return -1.0f;
    }
    return tmin;
}

} // namespace

namespace scriptbridge {

void install(ScriptHost& host, Deps deps) {
    // Shared, mutable state of the bridge itself: which material game.setColor
    // minted for which entity, so repeated recolours reuse one material instead
    // of growing the library every frame. Held by shared_ptr because every
    // callback below is a separate std::function.
    auto colorMats = std::make_shared<std::unordered_map<int, AssetId>>();

    Document&      doc    = *deps.doc;
    ModelLibrary&  models = *deps.models;
    AssetDatabase& db     = *deps.assetDb;

    // --- Assets --------------------------------------------------------------
    host.assetList = [&db](const std::string& type) {
        const AssetType want = typeFromName(type);
        std::vector<ScriptAssetInfo> out;
        for (const AssetId& id : db.allAssets())
            if (const AssetDatabase::Entry* e = db.entry(id))
                if (want == AssetType::Unknown || e->type == want)
                    out.push_back(assetRow(db, *e));
        return out;
    };
    host.findAsset = [&db](const std::string& name, const std::string& type) {
        const AssetId id = resolveAsset(db, name, typeFromName(type));
        return id.valid() ? id.toString() : std::string();
    };
    host.assetInfo = [&db](const std::string& id, ScriptAssetInfo& out) {
        const AssetId gid = resolveAsset(db, id, AssetType::Unknown);
        const AssetDatabase::Entry* e = gid.valid() ? db.entry(gid) : nullptr;
        if (!e) return false;
        out = assetRow(db, *e);
        return true;
    };
    host.assetPath = [&db](const std::string& id) {
        const AssetId gid = resolveAsset(db, id, AssetType::Unknown);
        const AssetDatabase::Entry* e = gid.valid() ? db.entry(gid) : nullptr;
        return e ? e->absPath.generic_string() : std::string();
    };
    host.refreshAssets = [&db]{ db.refresh(); };

    // --- Models --------------------------------------------------------------
    host.loadModel = [&db, &models, &doc](const std::string& asset) {
        std::string path;
        const AssetId gid = resolveAsset(db, asset, AssetType::Model);
        if (gid.valid()) {
            if (const AssetDatabase::Entry* e = db.entry(gid))
                path = e->absPath.generic_string();
        } else { // not an asset: accept a plain path, so ad-hoc files still load
            std::error_code ec;
            if (std::filesystem::exists(asset, ec)) path = asset;
        }
        if (path.empty()) return -1;
        return models.import(path, db, doc.materials());
    };
    host.modelInfo = [&models](int id, ScriptModelInfo& out) {
        LoadedModel* lm = models.byId(id);
        if (!lm) return false;
        out.name      = lm->name;
        out.path      = lm->path;
        out.boundsMin = lm->boundsMin;
        out.boundsMax = lm->boundsMax;
        out.meshes    = static_cast<int>(lm->meshes.size());
        out.animated  = lm->animated;
        return true;
    };

    // --- Materials -----------------------------------------------------------
    host.materialList = [&doc] {
        std::vector<ScriptMaterialInfo> out;
        out.reserve(doc.materials().size());
        for (const MaterialDef& m : doc.materials()) out.push_back(materialRow(m));
        return out;
    };
    host.findMaterial = [&doc](const std::string& name) {
        const MaterialDef* m = findMaterialDef(doc, name);
        return m ? m->assetId.toString() : std::string();
    };
    host.materialInfo = [&doc](const std::string& id, ScriptMaterialInfo& out) {
        const MaterialDef* m = findMaterialDef(doc, id);
        if (!m) return false;
        out = materialRow(*m);
        return true;
    };
    host.createMaterial = [&doc, &db](const ScriptMaterialEdit& ed) {
        MaterialDef md;
        md.assetId = AssetId::generate();
        md.name    = ed.name ? *ed.name
                             : "Script " + std::to_string(doc.materials().size());
        applyEdit(db, md, ed);
        doc.materials().push_back(md);
        return md.assetId.toString();
    };
    host.setMaterialProps = [&doc, &db](const std::string& id,
                                        const ScriptMaterialEdit& ed) {
        MaterialDef* m = findMaterialDef(doc, id);
        if (!m) return false;
        applyEdit(db, *m, ed);
        return true;
    };
    host.setEntityMaterial = [&doc](int entity, const std::string& id) {
        Entity*     e = doc.find(entity);
        MaterialDef* m = findMaterialDef(doc, id);
        if (!e || !m) return false;
        if (auto* mc = e->components.get<MaterialComponent>()) {
            mc->material = m->assetId;
        } else {
            auto nc = std::make_unique<MaterialComponent>();
            nc->material = m->assetId;
            e->components.items.push_back(std::move(nc));
        }
        return true;
    };
    host.entityMaterial = [&doc](int entity) {
        const Entity* e = doc.find(entity);
        const auto*   mc = e ? e->components.get<MaterialComponent>() : nullptr;
        return (mc && mc->material.valid()) ? mc->material.toString() : std::string();
    };
    host.setEntityColor = [&doc, colorMats](int entity, glm::vec3 rgb) {
        Entity* e = doc.find(entity);
        if (!e) return false;
        // Reuse this entity's own script material if it still exists (a Play stop
        // restores the library, dropping materials minted during the session).
        auto it = colorMats->find(entity);
        MaterialDef* mine = nullptr;
        if (it != colorMats->end())
            for (MaterialDef& m : doc.materials())
                if (m.assetId == it->second) { mine = &m; break; }
        if (!mine) {
            MaterialDef md;
            md.assetId = AssetId::generate();
            md.name    = "Color " + std::to_string(entity);
            doc.materials().push_back(md);
            mine = &doc.materials().back();
            (*colorMats)[entity] = mine->assetId;
            if (auto* have = e->components.get<MaterialComponent>()) {
                have->material = mine->assetId;
            } else {
                auto mc = std::make_unique<MaterialComponent>();
                mc->material = mine->assetId;
                e->components.items.push_back(std::move(mc));
            }
        }
        mine->albedo = rgb;
        return true;
    };

    // --- Entities ------------------------------------------------------------
    host.allEntities = [&doc] {
        std::vector<int> out;
        out.reserve(doc.entities().size());
        for (const Entity& e : doc.entities()) out.push_back(e.id);
        return out;
    };
    host.findEntity = [&doc](const std::string& name) {
        for (const Entity& e : doc.entities())
            if (e.name == name) return e.id;
        const std::string needle = lower(name); // second pass: case-insensitive
        for (const Entity& e : doc.entities())
            if (lower(e.name) == needle) return e.id;
        return -1;
    };
    host.findEntities = [&doc](const std::string& name) {
        const std::string needle = lower(name);
        std::vector<int> out;
        for (const Entity& e : doc.entities())
            if (lower(e.name) == needle) out.push_back(e.id);
        return out;
    };
    host.entityInfo = [&doc](int id, ScriptEntityInfo& out) {
        const Entity* e = doc.find(id);
        if (!e) return false;
        out = {};
        out.id     = e->id;
        out.type   = static_cast<int>(e->type);
        out.parent = e->parent;
        out.name   = e->name;
        out.active = e->active;
        out.activeInHierarchy = e->activeInHierarchy;
        if (const auto* sc = e->components.get<ScriptComponent>())   out.script = sc->file;
        if (const auto* mc = e->components.get<MaterialComponent>();
            mc && mc->material.valid())                              out.material = mc->material.toString();
        if (const auto* mo = e->components.get<ModelComponent>())     out.model = mo->modelPath;
        if (const auto* pc = e->components.get<PhysicsComponent>()) {
            out.hasPhysics = true;
            out.dynamic    = pc->dynamic;
        }
        return true;
    };
    host.getRot = [&doc](int id, glm::vec3& out) {
        const Entity* e = doc.find(id);
        if (!e) return false;
        out = e->rotation;
        return true;
    };
    host.setRot = [setWorldRot = deps.setWorldRot](int id, glm::vec3 r) {
        if (setWorldRot) setWorldRot(id, r);
    };
    host.getScale = [&doc](int id, glm::vec3& out) {
        const Entity* e = doc.find(id);
        if (!e) return false;
        out = e->half;
        return true;
    };
    host.setScale = [&doc](int id, glm::vec3 s) {
        if (Entity* e = doc.find(id)) e->half = glm::max(s, glm::vec3(0.001f));
    };
    host.setName = [&doc](int id, const std::string& n) {
        if (Entity* e = doc.find(id)) e->name = n;
    };
    host.setActive = [&doc](int id, bool on) {
        if (Entity* e = doc.find(id)) e->active = on;
    };
    host.setParent = [reparent = deps.reparent](int id, int parent) {
        if (reparent) reparent(id, parent);
    };
    host.children = [&doc](int id) {
        std::vector<int> out;
        for (const Entity& e : doc.entities())
            if (e.parent == id) out.push_back(e.id);
        return out;
    };

    // --- Lights --------------------------------------------------------------
    host.setLight = [&doc](int id, const ScriptLightEdit& ed) {
        Entity* e = doc.find(id);
        auto*   l = e ? e->components.get<LightComponent>() : nullptr;
        if (!l) return false;
        if (ed.color)     l->color     = *ed.color;
        if (ed.intensity) l->intensity = glm::max(*ed.intensity, 0.0f);
        if (ed.range)     l->range     = glm::max(*ed.range, 0.0f);
        if (ed.type)      l->type      = glm::clamp(*ed.type, 0, 1);
        if (ed.spotAngle) l->spotAngle = glm::clamp(*ed.spotAngle, 1.0f, 89.0f);
        if (ed.spotBlend) l->spotBlend = glm::clamp(*ed.spotBlend, 0.0f, 1.0f);
        return true;
    };

    // --- World ---------------------------------------------------------------
    host.terrainHeight = deps.terrainHeight;
    host.loadScene     = deps.loadScene;
    host.log           = deps.log;
    // Ray vs. the entities' world-space pick boxes (axis-aligned, rotation is
    // ignored) -- the same boxes the viewport picks with. Good enough for line of
    // sight, ground probes and "what am I looking at".
    host.raycast = [&doc](glm::vec3 o, glm::vec3 d, float maxDist, glm::vec3& outHit,
                          float& outDist) {
        const float len = glm::length(d);
        if (len < 1e-6f) return -1;
        d /= len;
        int   best = -1;
        float bestT = maxDist > 0.0f ? maxDist : 1e30f;
        for (const Entity& e : doc.entities()) {
            if (!e.activeInHierarchy || e.type == EntityType::Sun) continue;
            const float t = rayBox(o, d, e.center - e.half, e.center + e.half);
            if (t >= 0.0f && t < bestT) { bestT = t; best = e.id; }
        }
        if (best < 0) return -1;
        outDist = bestT;
        outHit  = o + d * bestT;
        return best;
    };
}

} // namespace scriptbridge
