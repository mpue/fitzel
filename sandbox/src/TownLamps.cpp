#include "TownLamps.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "CitySystem.hpp"
#include "Component.hpp"
#include "EditMesh.hpp"
#include "Modifiers.hpp"
#include "ModelLibrary.hpp"
#include "PrefabSystem.hpp"
#include "SceneGraph.hpp"
#include "SplineGenDetail.hpp"   // appendBox, for the standard lamp

namespace {

// A library material by name: found and re-dressed, or made.
fitzel::AssetId ensureLampMaterial(std::vector<MaterialDef>& mats, const std::string& name,
                                   glm::vec3 albedo, float refl, float rough,
                                   glm::vec3 emission, float strength) {
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = albedo; m.reflectivity = refl; m.roughness = rough;
        m.emission = emission; m.emissionStrength = strength;
        if (!m.assetId.valid()) m.assetId = fitzel::AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId = fitzel::AssetId::generate();
    md.name = name;
    md.albedo = albedo; md.reflectivity = refl; md.roughness = rough;
    md.emission = emission; md.emissionStrength = strength;
    mats.push_back(md);
    return md.assetId;
}

// The yaw (degrees about +Y) that turns local +Z onto `v` (x, z): the engine's
// Y rotation takes (0, 0, 1) to (sin y, 0, cos y).
float yawOf(glm::vec2 v) { return glm::degrees(std::atan2(v.x, v.y)); }

// Which way a lamp prefab's head points in its own frame (LampPrefab::forward).
glm::vec2 headAxis(int forward) {
    switch (forward) {
        case 1:  return {0.0f, -1.0f};
        case 2:  return {1.0f, 0.0f};
        case 3:  return {-1.0f, 0.0f};
        default: return {0.0f, 1.0f};
    }
}

} // namespace

float TownLamps::nightFactor(const glm::vec3& sunDir) {
    // On from about seven degrees up, fully lit with the sun on the horizon:
    // a street is dim long before the sky is.
    return 1.0f - glm::smoothstep(0.0f, 0.12f, sunDir.y);
}

TownLamps::Look TownLamps::standardLook(std::vector<MaterialDef>& materials) {
    // A plain modern street lamp, 7.5 m: a mast on a foot, an arm reaching out
    // over the street (+Z, the way the lamp faces), the lantern at its end with
    // the glass underneath. The light hangs just below the glass.
    const fitzel::AssetId pole = ensureLampMaterial(materials, "City Lamp Pole",
        {0.20f, 0.21f, 0.22f}, 0.25f, 0.45f, glm::vec3(0.0f), 0.0f);
    const fitzel::AssetId glass = ensureLampMaterial(materials, "City Lamp Glass",
        {0.92f, 0.90f, 0.84f}, 0.10f, 0.25f, {1.00f, 0.84f, 0.60f}, 5.0f);
    if (m_standard.empty()) {
        using splinegen::detail::appendBox;
        splinegen::detail::Slot sp, sg;
        appendBox(sp, {0.0f, 0.35f, 0.0f}, {0.13f, 0.35f, 0.13f}, 0.0f, 1.0f);    // foot
        appendBox(sp, {0.0f, 3.75f, 0.0f}, {0.065f, 3.75f, 0.065f}, 0.0f, 1.0f);  // mast
        appendBox(sp, {0.0f, 7.45f, 0.85f}, {0.045f, 0.045f, 0.90f}, 0.0f, 1.0f); // arm
        appendBox(sp, {0.0f, 7.40f, 1.70f}, {0.20f, 0.07f, 0.40f}, 0.0f, 1.0f);   // lantern
        appendBox(sg, {0.0f, 7.31f, 1.70f}, {0.16f, 0.02f, 0.33f}, 0.0f, 1.0f);   // its glass
        m_standard.push_back(fitzel::Mesh::create(sp.data));
        m_standard.push_back(fitzel::Mesh::create(sg.data));
    }
    Look look;
    look.standard = true;
    look.parts.push_back({&m_standard[0], pole, glm::mat4(1.0f)});
    look.parts.push_back({&m_standard[1], glass, glm::mat4(1.0f)});
    Light L;
    L.type  = 0;
    L.pos   = {0.0f, 7.05f, 1.70f};
    L.color = glm::vec3(1.00f, 0.84f, 0.62f) * 0.9f;
    L.range = 18.0f;
    L.castShadows = true;
    look.lights.push_back(L);
    return look;
}

void TownLamps::update(const CitySystem& towns, std::vector<MaterialDef>& materials) {
    bool stale = towns.revision() != m_revision;
    // A lamp prefab edited and saved: main drops it from its cache, and the next
    // lookup loads it anew -- at another address. Asked once a second (sixty
    // frames), and only about prefabs that did load: asking again for one that
    // failed would print its failure every time.
    if (!stale && findPrefab && ++m_sinceCheck >= 60) {
        m_sinceCheck = 0;
        for (const Look& l : m_looks)
            if (l.source && findPrefab(l.name) != l.source) {
                stale = true;
                break;
            }
    }
    if (stale) rebuild(towns, materials);
}

void TownLamps::rebuild(const CitySystem& towns, std::vector<MaterialDef>& materials) {
    m_revision   = towns.revision();
    m_sinceCheck = 0;
    m_looks.clear();
    m_lamps.clear();
    m_glow.clear();

    // One look per prefab and head direction, shared by every town naming it. A
    // prefab that would not load keeps its (empty) look too, so it is asked for
    // once per rebuild and not once per lamp.
    auto lookOf = [&](const cityplan::LampPrefab& lp) {
        for (std::size_t i = 0; i < m_looks.size(); ++i)
            if (m_looks[i].name == lp.prefab && m_looks[i].forward == lp.forward)
                return static_cast<int>(i);
        const int index = static_cast<int>(m_looks.size());
        Look look;
        look.name    = lp.prefab;
        look.forward = lp.forward;
        const prefab::Prefab* p = findPrefab ? findPrefab(lp.prefab) : nullptr;
        if (p && flatten(*p, index, look)) look.source = p;
        m_looks.push_back(std::move(look));
        return index;
    };

    const std::vector<CitySystem::Built>& built = towns.built();
    for (std::size_t t = 0; t < built.size() && t < towns.towns.size(); ++t) {
        const cityplan::Rule& r = towns.towns[t];
        if (!r.enabled) continue;
        for (const cityplan::Lamp& lamp : built[t].town.lamps) {
            if (lamp.prefab >= static_cast<int>(r.lampPrefabs.size())) continue;
            int li = -1;
            int forward = 0;
            if (lamp.prefab < 0) {
                // No prefab: the standard lamp, one look for every town.
                for (std::size_t k = 0; k < m_looks.size() && li < 0; ++k)
                    if (m_looks[k].standard) li = static_cast<int>(k);
                if (li < 0) {
                    li = static_cast<int>(m_looks.size());
                    m_looks.push_back(standardLook(materials));
                }
            } else {
                const cityplan::LampPrefab& lp = r.lampPrefabs[static_cast<std::size_t>(lamp.prefab)];
                li = lookOf(lp);
                forward = lp.forward;
            }
            const Look& look = m_looks[static_cast<std::size_t>(li)];
            if (look.parts.empty() && look.lights.empty()) continue;
            // Its head onto the way to the street.
            const float yaw = yawOf(lamp.facing) - yawOf(headAxis(forward));
            Placed pl;
            pl.frame = glm::rotate(glm::translate(glm::mat4(1.0f), lamp.pos), glm::radians(yaw),
                                   glm::vec3(0.0f, 1.0f, 0.0f));
            pl.pos  = lamp.pos;
            pl.look = li;
            m_lamps.push_back(pl);
        }
    }

    // The glowing materials of the looks in use: a lamp's glass is whatever of
    // it has an emission -- colour or map.
    for (const Look& l : m_looks)
        for (const Part& part : l.parts) {
            const bool seen = std::any_of(m_glow.begin(), m_glow.end(),
                                          [&](const auto& g) { return g.first == part.material; });
            if (seen) continue;
            for (const MaterialDef& m : materials) {
                if (m.assetId != part.material) continue;
                if (m.emissionTexId.valid() || glm::dot(m.emission, m.emission) > 0.0f)
                    m_glow.emplace_back(m.assetId, m.emissionStrength);
                break;
            }
        }
}

bool TownLamps::flatten(const prefab::Prefab& p, int lookIndex, Look& out) const {
    // The prefab's entities in its own frame. The root's pivot is the lamp's
    // foot on the pavement -- where the prefab lands when it is placed by hand
    // (prefab::instantiate drops the root at the spot and keeps its turn).
    std::vector<Entity> es = p.entities;
    for (Entity& e : es)
        if (e.parent < 0) e.localCenter = glm::vec3(0.0f);
    scenegraph::resolve(es);

    float lowY = 1e30f;
    auto grow = [&](const glm::mat4& m, const glm::vec3& a, const glm::vec3& b) {
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 c((k & 1) ? b.x : a.x, (k & 2) ? b.y : a.y, (k & 4) ? b.z : a.z);
            lowY = std::min(lowY, (m * glm::vec4(c, 1.0f)).y);
        }
    };
    for (const Entity& e : es) {
        if (!e.activeInHierarchy) continue;
        if (const auto* mc = e.components.get<ModelComponent>(); mc && models) {
            if (LoadedModel* lm = models->byId(mc->modelId)) {
                // As SceneSubmit draws a model: filling centre +/- half.
                const glm::vec3 sz = glm::max(lm->size(), glm::vec3(1e-4f));
                const glm::mat4 m = scenegraph::compose(e.center, e.rotation, (e.half * 2.0f) / sz) *
                                    glm::translate(glm::mat4(1.0f), -lm->center());
                for (std::size_t i = 0; i < lm->meshes.size() && i < lm->primMaterialId.size(); ++i)
                    out.parts.push_back({&lm->meshes[i], lm->primMaterialId[i], m});
                grow(m, lm->boundsMin, lm->boundsMax);
            }
        } else if (const auto* meshC = e.components.get<MeshComponent>(); meshC && meshCache) {
            // Modelled in the editor: uploaded by the shared cache under an id of
            // its own (one per look and entity), dressed as SceneSubmit does --
            // a range of its own too: the traffic keeps 0x70.. and 0x78.., and
            // two users of one cache entry rebuild it under each other.
            const int cacheId = 0x7C000000 + lookIndex * 4096 + e.id;
            // What it shows: its modifier stack's result, if it has one.
            const modifiers::Shown sh =
                modifiers::shown(cacheId, *meshC, e.components.get<ModifierStackComponent>());
            const glm::mat4 m = scenegraph::compose(e.center, e.rotation,
                                                    editmesh::fitScale(*sh.mesh, e.half));
            const auto* matC = e.components.get<MaterialComponent>();
            const fitzel::AssetId own = matC ? matC->material : fitzel::AssetId{};
            for (const EditMeshCache::Sub& sub : meshCache->submeshes(cacheId, sh.revision, *sh.mesh))
                out.parts.push_back({&sub.mesh, sub.material.valid() ? sub.material : own, m});
            glm::vec3 lo, hi;
            sh.mesh->bounds(lo, hi);
            grow(m, lo, hi);
        }
        if (const auto* lc = e.components.get<LightComponent>()) {
            // As main turns a scene light into the renderer's: a spot shines
            // down the entity's +Z.
            Light L;
            L.type  = lc->type == 1 ? 1 : 0;
            L.pos   = e.center;
            L.dir   = glm::normalize(glm::quat(glm::radians(e.rotation)) * glm::vec3(0.0f, 0.0f, 1.0f));
            L.color = lc->color * lc->intensity;
            L.range = lc->range;
            const float outer = glm::radians(glm::clamp(lc->spotAngle, 1.0f, 89.0f));
            const float inner = outer * (1.0f - glm::clamp(lc->spotBlend, 0.0f, 1.0f));
            L.cosOuter = std::cos(outer);
            L.cosInner = std::cos(inner);
            L.castShadows = L.type == 0 && lc->castShadows;
            L.shadowBias  = lc->shadowBias;
            out.lights.push_back(L);
        }
    }
    if (out.parts.empty() && out.lights.empty()) return false;
    // Stood on the pavement: its lowest point on y = 0. A prefab whose root is
    // the lamp's middle rather than its foot would otherwise stand half sunk.
    const float lift = lowY < 1e29f ? -lowY : 0.0f;
    const glm::mat4 up = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, lift, 0.0f));
    for (Part& part : out.parts) part.local = up * part.local;
    for (Light& L : out.lights) L.pos.y += lift;
    return true;
}

void TownLamps::forEachDraw(const glm::vec3& eye,
                            const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                                     const glm::mat4&, bool)>& fn) const {
    for (const Placed& l : m_lamps) {
        const float d = glm::length(l.pos - eye);
        if (d > reach) continue;
        const bool nearEye = d < shadowReach;
        for (const Part& part : m_looks[static_cast<std::size_t>(l.look)].parts)
            fn(*part.mesh, part.material, l.frame * part.local, nearEye);
    }
}

void TownLamps::collectLights(const glm::vec3& eye, float on,
                              std::vector<fitzel::PointLight>& points, int pointCap,
                              std::vector<fitzel::SpotLight>& spots, int spotCap,
                              int shadowCap) const {
    if (on <= 0.001f || m_lamps.empty()) return;
    const int freePoints = std::max(pointCap - static_cast<int>(points.size()), 0);
    const int freeSpots  = std::max(spotCap - static_cast<int>(spots.size()), 0);
    if (freePoints + freeSpots == 0) return;
    std::vector<std::pair<float, int>> nearest;
    for (std::size_t i = 0; i < m_lamps.size(); ++i) {
        const Placed& l = m_lamps[i];
        if (m_looks[static_cast<std::size_t>(l.look)].lights.empty()) continue;
        const float d = glm::length(l.pos - eye);
        if (d < lightReach) nearest.emplace_back(d, static_cast<int>(i));
    }
    std::sort(nearest.begin(), nearest.end());
    // Where the lights stop: the reach, or -- when there are more lamps in it
    // than lights to give -- the first lamp that gets none. Each fades out on
    // its way there, so the lamp dropped next is already dark: the distance of
    // the n-th nearest lamp moves smoothly with the eye, and so does every light.
    float cut = lightReach;
    const std::size_t slots = static_cast<std::size_t>(std::max(freePoints, freeSpots));
    if (nearest.size() > slots) cut = std::min(cut, nearest[slots].first);
    // The same for the shadows: the slots the scene's own shadowed lights left,
    // handed out nearest first, and faded out towards the first light that gets
    // none -- so a shadow comes and goes as smoothly as the light does.
    int freeShadows = shadowCap;
    for (const fitzel::PointLight& p : points) freeShadows -= p.castShadows ? 1 : 0;
    freeShadows = std::max(freeShadows, 0);
    float shadowCut = cut;
    {
        int seen = 0;
        for (const auto& [d, i] : nearest) {
            const Placed& l = m_lamps[static_cast<std::size_t>(i)];
            for (const Light& L : m_looks[static_cast<std::size_t>(l.look)].lights)
                if (L.castShadows && seen++ == freeShadows) shadowCut = std::min(shadowCut, d);
            if (seen > freeShadows) break;
        }
    }
    int shadowsGiven = 0;
    for (const auto& [d, i] : nearest) {
        const float fade = on * (1.0f - glm::smoothstep(0.7f * cut, cut, d));
        if (fade <= 0.001f) break;
        const float shadowFade = 1.0f - glm::smoothstep(0.7f * shadowCut, shadowCut, d);
        const Placed& l = m_lamps[static_cast<std::size_t>(i)];
        for (const Light& L : m_looks[static_cast<std::size_t>(l.look)].lights) {
            const glm::vec3 pos = glm::vec3(l.frame * glm::vec4(L.pos, 1.0f));
            if (L.type == 1) {
                if (static_cast<int>(spots.size()) >= spotCap) continue;
                fitzel::SpotLight s;
                s.position  = pos;
                s.direction = glm::normalize(glm::mat3(l.frame) * L.dir);
                s.color     = L.color * fade;
                s.range     = L.range;
                s.cosInner  = L.cosInner;
                s.cosOuter  = L.cosOuter;
                spots.push_back(s);
            } else {
                if (static_cast<int>(points.size()) >= pointCap) continue;
                fitzel::PointLight pl;
                pl.position = pos;
                pl.color    = L.color * fade;
                pl.range    = L.range;
                if (L.castShadows && shadowsGiven < freeShadows && shadowFade > 0.001f) {
                    pl.castShadows    = true;
                    pl.shadowBias     = L.shadowBias;
                    pl.shadowStrength = shadowFade;
                    ++shadowsGiven;
                }
                points.push_back(pl);
            }
        }
    }
}

void TownLamps::forEachGlow(const std::function<void(const fitzel::AssetId&, float)>& fn) const {
    for (const auto& [id, strength] : m_glow) fn(id, strength);
}
