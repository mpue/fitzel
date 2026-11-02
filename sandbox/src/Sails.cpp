#include "Sails.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>

#include "Component.hpp"   // SailComponent, ModelComponent
#include "SceneGraph.hpp"

namespace {

// Where a model part's own space is in the world: as SceneSubmit draws it, the
// model's box centred on the entity and stretched to its half-extents.
glm::mat4 modelMatrix(const Entity& e, const LoadedModel& lm) {
    const glm::vec3 sz = glm::max(lm.size(), glm::vec3(1e-4f));
    return scenegraph::compose(e.center, e.rotation, (e.half * 2.0f) / sz) *
           glm::translate(glm::mat4(1.0f), -lm.center());
}

// How many metres one of the model's units is, on average over its axes.
float unitOf(const Entity& e, const LoadedModel& lm) {
    const glm::vec3 sz = glm::max(lm.size(), glm::vec3(1e-4f));
    const glm::vec3 s = (e.half * 2.0f) / sz;
    return std::max((s.x + s.y + s.z) / 3.0f, 1e-4f);
}

} // namespace

void SailSystem::begin(const std::vector<Entity>& entities, const ModelOf& modelOf) {
    m_sails.clear();
    std::unordered_map<int, const Entity*> byId;
    byId.reserve(entities.size());
    for (const Entity& e : entities) byId[e.id] = &e;
    std::unordered_set<int> claimed;   // a model's meshes are posed once

    for (const Entity& e : entities) {
        const auto* mc = e.components.get<ModelComponent>();
        if (!e.components.get<SailComponent>() || !mc || !e.activeInHierarchy) continue;
        LoadedModel* lm = modelOf(mc->modelId);
        if (!lm || lm->animated || !claimed.insert(lm->id).second) continue;
        const glm::mat4 toSail = glm::inverse(modelMatrix(e, *lm));
        const float unit = unitOf(e, *lm);

        // What holds it: every other model part of the object it belongs to --
        // the hull with its masts and yards, the rigging -- in the sail's own
        // space. All of their triangles' corners: the spars are long and have
        // few of them, and the ties are found where they come close.
        const Entity* root = &e;
        for (int guard = 0; root->parent >= 0 && guard < 256; ++guard) {
            auto it = byId.find(root->parent);
            if (it == byId.end()) break;
            root = it->second;
        }
        std::vector<glm::vec3> holders;
        for (int id : scenegraph::subtree(entities, root->id)) {
            auto it = byId.find(id);
            if (it == byId.end()) continue;
            const Entity& p = *it->second;
            if (&p == &e || !p.activeInHierarchy || p.components.get<SailComponent>()) continue;
            const auto* pm = p.components.get<ModelComponent>();
            const LoadedModel* pl = pm ? modelOf(pm->modelId) : nullptr;
            if (!pl) continue;
            const glm::mat4 toHere = toSail * modelMatrix(p, *pl);
            const std::vector<glm::vec3>& src = pl->meshTris.empty() ? pl->hullPoints : pl->meshTris;
            holders.reserve(holders.size() + src.size());
            for (const glm::vec3& v : src) holders.push_back(glm::vec3(toHere * glm::vec4(v, 1.0f)));
        }

        for (std::size_t i = 0; i < lm->meshes.size(); ++i) {
            // The corners back off the GPU, once: models keep no CPU copy.
            fitzel::MeshData md = lm->meshes[i].readback();
            std::vector<fitzel::Vertex> tris;
            if (md.indices.empty()) {
                tris = std::move(md.vertices);
            } else {
                tris.reserve(md.indices.size());
                for (std::uint32_t k : md.indices)
                    if (k < md.vertices.size()) tris.push_back(md.vertices[k]);
            }
            Sail s;
            s.entity = e.id;
            s.model  = lm->id;
            s.mesh   = static_cast<int>(i);
            // Within sixty centimetres of a spar or a rope is tied to it.
            if (!s.cloth.build(tris, holders, 0.6f / unit, unit)) continue;
            fitzel::Mesh posable = fitzel::Mesh::create(s.cloth.rest, s.cloth.indices);
            s.modelled = std::make_unique<fitzel::Mesh>(std::move(lm->meshes[i]));
            lm->meshes[i] = std::move(posable);
            m_sails.push_back(std::move(s));
        }
    }
}

void SailSystem::update(const std::vector<Entity>& entities, const ModelOf& modelOf,
                        const wind::State& air, float windSpeed, float dt) {
    if (m_sails.empty()) return;
    for (Sail& s : m_sails) {
        const Entity* e = nullptr;
        for (const Entity& c : entities)
            if (c.id == s.entity) { e = &c; break; }
        LoadedModel* lm = modelOf(s.model);
        if (!e || !lm || s.mesh >= static_cast<int>(lm->meshes.size())) continue;
        const auto* sc = e->components.get<SailComponent>();
        fitzel::Mesh& mesh = lm->meshes[static_cast<std::size_t>(s.mesh)];
        if (!sc || !e->activeInHierarchy || mesh.vertexCount() != s.cloth.rest.size()) continue;

        // The air in the sail's own space: the ship heels and turns under it.
        const glm::mat4 m = modelMatrix(*e, *lm);
        glm::vec3 dir = glm::inverse(glm::mat3(m)) * glm::vec3(air.dir.x, 0.0f, air.dir.y);
        const float len = glm::length(dir);
        dir = len > 1e-6f ? dir / len : glm::vec3(1.0f, 0.0f, 0.0f);
        // Each piece in the gust where it is.
        s.gust.resize(s.cloth.islands.size());
        for (std::size_t i = 0; i < s.cloth.islands.size(); ++i) {
            const glm::vec3 c = glm::vec3(m * glm::vec4(s.cloth.islands[i].centroid, 1.0f));
            s.gust[i] = wind::gust(air, glm::vec2(c.x, c.z));
        }
        sail::Cloth::Air a;
        a.dir   = dir;
        a.speed = windSpeed;
        a.gust  = s.gust.data();
        s.cloth.pose(a, sc->billow, sc->flutter, dt, s.posed);
        mesh.update(s.posed);
    }
}

void SailSystem::end(const ModelOf& modelOf) {
    for (Sail& s : m_sails) {
        LoadedModel* lm = modelOf(s.model);
        if (lm && s.modelled && s.mesh < static_cast<int>(lm->meshes.size()))
            lm->meshes[static_cast<std::size_t>(s.mesh)] = std::move(*s.modelled);
    }
    m_sails.clear();
}
