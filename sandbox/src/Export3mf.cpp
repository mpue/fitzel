#include "Export3mf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Texture.hpp>

#include "Component.hpp"   // MeshComponent, ModelComponent, MaterialComponent, DecalComponent, PlayerStartComponent
#include "Document.hpp"
#include "EditMesh.hpp"
#include "ModelLibrary.hpp"
#include "Modifiers.hpp"   // modifiers::shown
#include "SceneGraph.hpp"  // scenegraph::compose, subtree

namespace export3mf {
namespace {

// Corners closer than this are one corner (a micrometre, in metres).
constexpr double kWeld = 1.0e-6;

// Does this object show a surface of its own? The same objects SceneSubmit
// draws, less what only the editor draws (a light's cube, a player start).
bool hasSurface(const Entity& e) {
    if (!e.activeInHierarchy) return false;
    if (e.type == EntityType::Sun || e.type == EntityType::Light || e.type == EntityType::Empty) return false;
    if (e.components.get<DecalComponent>() || e.components.get<PlayerStartComponent>()) return false;
    if (e.type == EntityType::Model) return e.components.get<ModelComponent>() != nullptr;
    return e.components.get<MeshComponent>() || isSolidPrimitive(e.type);
}

// An imported model's transform as SceneSubmit draws it.
glm::mat4 modelToWorld(const Entity& e, const LoadedModel& lm) {
    const glm::vec3 sz = glm::max(lm.size(), glm::vec3(1e-4f));
    return scenegraph::compose(e.center, e.rotation, (e.half * 2.0f) / sz) *
           glm::translate(glm::mat4(1.0f), -lm.center());
}

// A solid never made editable, as its shape. The round ones finer than the
// renderer draws them: smooth shading hides a facet on screen, a print shows it.
EditMesh primitiveOf(const Entity& e) {
    switch (e.type) {
        case EntityType::Ramp:     return EditMesh::ramp(e.half);
        case EntityType::Cylinder: return EditMesh::cylinder(e.half, 96);
        case EntityType::Sphere:   return EditMesh::sphere(e.half, 48, 96);
        case EntityType::Plane:    return EditMesh::plane(e.half);
        default:                   return EditMesh::box(e.half);
    }
}

// The colour a material shows: its albedo, or for a textured one the map's
// average (in linear light, as the eye would mix it) through its tint.
glm::vec3 shownColor(const MaterialDef& md) {
    if (!md.tex || !md.tex->isValid()) return md.albedo;
    const fitzel::ImagePixels px = md.tex->readback();
    if (!px.valid() || px.channels < 3) return md.tint;
    const auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    const auto srgb = [](float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; };
    float table[256];
    for (int i = 0; i < 256; ++i) table[i] = lin(static_cast<float>(i) / 255.0f);
    const std::size_t n = static_cast<std::size_t>(px.width) * static_cast<std::size_t>(px.height);
    const std::size_t step = std::max<std::size_t>(1, n / 65536);   // a sample is plenty
    const int ch = px.channels;
    glm::dvec3 sum(0.0);
    double weight = 0.0;
    for (std::size_t i = 0; i < n; i += step) {
        const unsigned char* p = &px.pixels[i * static_cast<std::size_t>(ch)];
        const double a = ch >= 4 ? p[3] / 255.0 : 1.0;   // a cut-out texel is not seen
        sum += a * glm::dvec3(table[p[0]], table[p[1]], table[p[2]]);
        weight += a;
    }
    if (weight <= 0.0) return md.tint;
    const glm::vec3 avg(sum / weight);
    return glm::vec3(srgb(avg.r), srgb(avg.g), srgb(avg.b)) * md.tint;
}

// Gathers one object's triangles into a Part, a corner per position.
struct Welder {
    struct Key {
        std::int64_t x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::uint64_t h = static_cast<std::uint64_t>(k.x) * 0x9E3779B97F4A7C15ull;
            h ^= static_cast<std::uint64_t>(k.y) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h ^= static_cast<std::uint64_t>(k.z) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            return static_cast<std::size_t>(h);
        }
    };
    Part& part;
    std::unordered_map<Key, int, KeyHash> at;

    int corner(const glm::dvec3& w) {
        const Key k{std::llround(w.x / kWeld), std::llround(w.y / kWeld), std::llround(w.z / kWeld)};
        const auto [it, fresh] = at.try_emplace(k, static_cast<int>(part.verts.size()));
        if (fresh) part.verts.push_back(w);
        return it->second;
    }
    void tri(const glm::dmat4& m, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, int color) {
        const int i = corner(glm::dvec3(m * glm::dvec4(glm::dvec3(a), 1.0)));
        const int j = corner(glm::dvec3(m * glm::dvec4(glm::dvec3(b), 1.0)));
        const int k = corner(glm::dvec3(m * glm::dvec4(glm::dvec3(c), 1.0)));
        if (i == j || j == k || i == k) return;   // folded flat by the weld
        part.tris.emplace_back(i, j, k);
        part.triColor.push_back(color);
    }
    void add(const fitzel::MeshData& md, const glm::dmat4& m, int color) {
        const std::size_t nv = md.vertices.size();
        const auto pos = [&](std::size_t i) { return md.vertices[i].position; };
        if (md.indices.empty()) {
            for (std::size_t i = 0; i + 2 < nv; i += 3) tri(m, pos(i), pos(i + 1), pos(i + 2), color);
            return;
        }
        for (std::size_t i = 0; i + 2 < md.indices.size(); i += 3) {
            const std::uint32_t a = md.indices[i], b = md.indices[i + 1], c = md.indices[i + 2];
            if (a < nv && b < nv && c < nv) tri(m, pos(a), pos(b), pos(c), color);
        }
    }
};

} // namespace

std::vector<int> objectsOf(const Document& doc, const std::vector<int>& selected) {
    std::vector<int> out;
    std::unordered_set<int> seen;
    for (int id : selected)
        for (int s : scenegraph::subtree(doc.entities(), id)) {
            if (!seen.insert(s).second) continue;
            const Entity* e = doc.find(s);
            if (e && hasSurface(*e)) out.push_back(s);
        }
    return out;
}

Shape gather(const Document& doc, ModelLibrary& models, const std::vector<int>& selected) {
    Shape s;
    const std::vector<MaterialDef>& mats = doc.materials();
    std::map<int, int> colorOf;   // library index -> Shape::colors index
    const auto color = [&](const fitzel::AssetId& id) {
        if (mats.empty()) return -1;
        const int mi = std::clamp(doc.materialIndex(id), 0, static_cast<int>(mats.size()) - 1);
        const auto [it, fresh] = colorOf.try_emplace(mi, static_cast<int>(s.colors.size()));
        if (fresh) {
            const MaterialDef& md = mats[static_cast<std::size_t>(mi)];
            s.colors.push_back({md.name.empty() ? "Material" : md.name, shownColor(md)});
        }
        return it->second;
    };
    const auto ownMaterial = [](const Entity& e) {
        const auto* mc = e.components.get<MaterialComponent>();
        return mc ? mc->material : fitzel::AssetId{};
    };
    // An edited mesh, one piece per material, as it is uploaded.
    const auto addGroups = [&](Welder& w, const EditMesh& m, const glm::dmat4& mm, const fitzel::AssetId& own) {
        for (const editmesh::Group& g : editmesh::buildGroups(m))
            w.add(g.data, mm, color(g.material.valid() ? g.material : own));
    };

    for (int id : objectsOf(doc, selected)) {
        const Entity& e = *doc.find(id);
        Part part;
        part.name = e.name.empty() ? entityTypeName(e.type) : e.name;
        Welder w{part, {}};
        if (e.type == EntityType::Model) {
            const auto* mdl = e.components.get<ModelComponent>();
            const LoadedModel* lm = models.byId(mdl->modelId);
            if (lm) {
                const glm::dmat4 mm(modelToWorld(e, *lm));
                for (std::size_t i = 0; i < lm->meshes.size(); ++i) {
                    const fitzel::AssetId mat =
                        i < lm->primMaterialId.size() ? lm->primMaterialId[i] : fitzel::AssetId{};
                    w.add(lm->meshes[i].readback(), mm, color(mat));
                }
            }
        } else if (const auto* mc = e.components.get<MeshComponent>()) {
            // What the object shows: its mesh through its modifier stack.
            const modifiers::Shown shown = modifiers::shown(e, *mc);
            if (shown.mesh)
                addGroups(w, *shown.mesh,
                          glm::dmat4(scenegraph::compose(e.center, e.rotation,
                                                         editmesh::fitScale(*shown.mesh, e.half))),
                          ownMaterial(e));
        } else {
            addGroups(w, primitiveOf(e), glm::dmat4(scenegraph::compose(e.center, e.rotation, glm::vec3(1.0f))),
                      ownMaterial(e));
        }
        if (part.tris.empty()) { ++s.skipped; continue; }
        s.parts.push_back(std::move(part));
    }

    bool first = true;
    for (const Part& p : s.parts)
        for (const glm::dvec3& v : p.verts) {
            s.lo = first ? v : glm::min(s.lo, v);
            s.hi = first ? v : glm::max(s.hi, v);
            first = false;
        }
    return s;
}

} // namespace export3mf
