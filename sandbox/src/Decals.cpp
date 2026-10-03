#include "Decals.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/render/Renderer.hpp>

#include "Component.hpp"   // DecalComponent, MeshComponent, ModelComponent, PhysicsComponent
#include "Document.hpp"
#include "EditMesh.hpp"
#include "ModelLibrary.hpp"
#include "Modifiers.hpp"   // modifiers::shown
#include "SceneGraph.hpp"  // scenegraph::compose

namespace decals {
namespace {

// Keep the part of polygon `in` with s * p[axis] <= 0.5 (one face of the box).
void clipTo(const std::vector<glm::vec3>& in, int axis, float s, std::vector<glm::vec3>& out) {
    out.clear();
    const std::size_t n = in.size();
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec3& a = in[i];
        const glm::vec3& b = in[(i + 1) % n];
        const float da = s * a[axis] - 0.5f, db = s * b[axis] - 0.5f;
        if (da <= 0.0f) out.push_back(a);
        if ((da <= 0.0f) != (db <= 0.0f)) out.push_back(a + (b - a) * (da / (da - db)));
    }
}

// The world box the unit cube is mapped to.
void worldBox(const glm::mat4& m, glm::vec3& lo, glm::vec3& hi) {
    lo = glm::vec3(1e30f);
    hi = glm::vec3(-1e30f);
    for (int k = 0; k < 8; ++k) {
        const glm::vec3 c((k & 1) ? 0.5f : -0.5f, (k & 2) ? 0.5f : -0.5f, (k & 4) ? 0.5f : -0.5f);
        const glm::vec3 w(m * glm::vec4(c, 1.0f));
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
    }
}

bool overlaps(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& c, float r) {
    return c.x + r >= lo.x && c.x - r <= hi.x && c.y + r >= lo.y && c.y - r <= hi.y &&
           c.z + r >= lo.z && c.z - r <= hi.z;
}

// May this object receive a decal at all?
bool receives(const Entity& e, const Receivers& who) {
    if (!who.objects || !e.activeInHierarchy || e.id == who.skip) return false;
    if (e.type == EntityType::Sun || e.type == EntityType::Light || e.type == EntityType::Empty) return false;
    if (e.components.get<DecalComponent>()) return false;
    if (const auto* ph = e.components.get<PhysicsComponent>(); ph && ph->dynamic) return false;
    return e.components.get<MeshComponent>() || e.components.get<ModelComponent>() ||
           isSolidPrimitive(e.type);
}

// A solid never made editable draws as its primitive: the same shape the editor
// gives it when it is made editable (ModelMode's convertToMesh), at its size.
EditMesh primitiveOf(const Entity& e) {
    switch (e.type) {
        case EntityType::Ramp:     return EditMesh::ramp(e.half);
        case EntityType::Cylinder: return EditMesh::cylinder(e.half);
        case EntityType::Sphere:   return EditMesh::sphere(e.half);
        case EntityType::Plane:    return EditMesh::plane(e.half);
        default:                   return EditMesh::box(e.half);
    }
}

std::uint64_t mixIn(std::uint64_t h, const void* p, std::size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
template <class T> std::uint64_t mixIn(std::uint64_t h, const T& v) { return mixIn(h, &v, sizeof v); }

// An imported model's transform as SceneSubmit draws it.
glm::mat4 modelToWorld(const Entity& e, const LoadedModel& lm) {
    const glm::vec3 sz = glm::max(lm.size(), glm::vec3(1e-4f));
    return scenegraph::compose(e.center, e.rotation, (e.half * 2.0f) / sz) *
           glm::translate(glm::mat4(1.0f), -lm.center());
}

} // namespace

fitzel::MeshData project(const glm::mat4& boxToWorld, const std::vector<Tri>& tris, float lift,
                         float maxAngleDeg) {
    fitzel::MeshData out;
    const glm::mat4 toBox = glm::inverse(boxToWorld);
    const glm::vec3 down  = glm::normalize(glm::vec3(boxToWorld * glm::vec4(0.0f, -1.0f, 0.0f, 0.0f)));
    const float cosMax = std::cos(glm::radians(std::clamp(maxAngleDeg, 0.0f, 89.9f)));
    std::vector<glm::vec3> poly, next;
    for (const Tri& t : tris) {
        glm::vec3 n = glm::cross(t.b - t.a, t.c - t.a);
        const float len = glm::length(n);
        if (len < 1e-12f) continue;
        n /= len;
        // Facing the projection, and not too steeply: a wall beside a floor decal
        // would otherwise take the image as a long smear down its face.
        if (glm::dot(n, -down) < cosMax) continue;
        poly = {glm::vec3(toBox * glm::vec4(t.a, 1.0f)), glm::vec3(toBox * glm::vec4(t.b, 1.0f)),
                glm::vec3(toBox * glm::vec4(t.c, 1.0f))};
        for (int axis = 0; axis < 3 && poly.size() >= 3; ++axis)
            for (float s : {-1.0f, 1.0f}) {
                clipTo(poly, axis, s, next);
                poly.swap(next);
                if (poly.size() < 3) break;
            }
        if (poly.size() < 3) continue;
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        for (const glm::vec3& p : poly) {
            fitzel::Vertex v{};
            v.position = glm::vec3(boxToWorld * glm::vec4(p, 1.0f)) + n * lift;
            v.normal   = n;
            v.uv       = glm::vec2(p.x + 0.5f, 0.5f - p.z);
            out.vertices.push_back(v);
        }
        for (std::uint32_t k = 1; k + 1 < poly.size(); ++k) {
            out.indices.push_back(base);
            out.indices.push_back(base + k);
            out.indices.push_back(base + k + 1);
        }
    }
    return out;
}

void gather(const std::vector<Entity>& entities, ModelLibrary& models, const HeightFn& terrain,
            const glm::mat4& boxToWorld, const Receivers& who, std::vector<Tri>& out) {
    glm::vec3 lo, hi;
    worldBox(boxToWorld, lo, hi);
    auto inBox = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        const glm::vec3 tlo = glm::min(a, glm::min(b, c)), thi = glm::max(a, glm::max(b, c));
        return thi.x >= lo.x && tlo.x <= hi.x && thi.y >= lo.y && tlo.y <= hi.y && thi.z >= lo.z && tlo.z <= hi.z;
    };
    // A modelled mesh's faces, fanned into triangles, through `mm`.
    auto addMesh = [&](const EditMesh& m, const glm::mat4& mm) {
        std::vector<glm::vec3> w(m.verts.size());
        for (std::size_t i = 0; i < m.verts.size(); ++i) w[i] = glm::vec3(mm * glm::vec4(m.verts[i], 1.0f));
        for (const auto& f : m.faces)
            for (std::size_t k = 1; k + 1 < f.size(); ++k) {
                const glm::vec3& a = w[static_cast<std::size_t>(f[0])];
                const glm::vec3& b = w[static_cast<std::size_t>(f[k])];
                const glm::vec3& c = w[static_cast<std::size_t>(f[k + 1])];
                if (inBox(a, b, c)) out.push_back({a, b, c});
            }
    };
    for (const Entity& e : entities) {
        if (!receives(e, who) || !overlaps(lo, hi, e.center, glm::length(e.half) * 1.01f)) continue;
        if (const auto* mc = e.components.get<MeshComponent>()) {
            const modifiers::Shown shown = modifiers::shown(e, *mc);
            if (!shown.mesh) continue;
            addMesh(*shown.mesh, scenegraph::compose(e.center, e.rotation, editmesh::fitScale(*shown.mesh, e.half)));
        } else if (const auto* mdl = e.components.get<ModelComponent>()) {
            const LoadedModel* lm = models.byId(mdl->modelId);
            if (!lm || lm->animated) continue;
            const glm::mat4 mm = modelToWorld(e, *lm);
            for (std::size_t i = 0; i + 2 < lm->meshTris.size(); i += 3) {
                const glm::vec3 a(mm * glm::vec4(lm->meshTris[i], 1.0f));
                const glm::vec3 b(mm * glm::vec4(lm->meshTris[i + 1], 1.0f));
                const glm::vec3 c(mm * glm::vec4(lm->meshTris[i + 2], 1.0f));
                if (inBox(a, b, c)) out.push_back({a, b, c});
            }
        } else {
            addMesh(primitiveOf(e), scenegraph::compose(e.center, e.rotation, glm::vec3(1.0f)));
        }
    }
    if (who.terrain && terrain) {
        // The drawn ground under the box, as a grid of quads fine enough for the
        // image to follow it: two dozen cells across the shorter side, no finer
        // than 5 cm, no coarser than a metre, at most 128 a side.
        const float spanX = hi.x - lo.x, spanZ = hi.z - lo.z;
        const float step = std::clamp(std::min(spanX, spanZ) / 24.0f, 0.05f, 1.0f);
        const int nx = std::clamp(static_cast<int>(std::ceil(spanX / step)), 1, 128);
        const int nz = std::clamp(static_cast<int>(std::ceil(spanZ / step)), 1, 128);
        const float sx = spanX / static_cast<float>(nx), sz = spanZ / static_cast<float>(nz);
        std::vector<glm::vec3> g(static_cast<std::size_t>((nx + 1) * (nz + 1)));
        for (int j = 0; j <= nz; ++j)
            for (int i = 0; i <= nx; ++i) {
                const float x = lo.x + sx * static_cast<float>(i), z = lo.z + sz * static_cast<float>(j);
                g[static_cast<std::size_t>(j * (nx + 1) + i)] = glm::vec3(x, terrain(x, z), z);
            }
        auto at = [&](int i, int j) { return g[static_cast<std::size_t>(j * (nx + 1) + i)]; };
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i) {
                const glm::vec3 a = at(i, j), b = at(i, j + 1), c = at(i + 1, j + 1), d = at(i + 1, j);
                if (inBox(a, b, c)) out.push_back({a, b, c});
                if (inBox(a, c, d)) out.push_back({a, c, d});
            }
    }
}

std::uint64_t signature(const std::vector<Entity>& entities, ModelLibrary& models, const glm::mat4& boxToWorld,
                        const Receivers& who) {
    glm::vec3 lo, hi;
    worldBox(boxToWorld, lo, hi);
    std::uint64_t h = 1469598103934665603ull;
    h = mixIn(h, boxToWorld);
    h = mixIn(h, who.objects);
    h = mixIn(h, who.terrain);
    for (const Entity& e : entities) {
        if (!receives(e, who) || !overlaps(lo, hi, e.center, glm::length(e.half) * 1.01f)) continue;
        h = mixIn(h, e.id);
        h = mixIn(h, e.center);
        h = mixIn(h, e.rotation);
        h = mixIn(h, e.half);
        h = mixIn(h, e.type);
        if (const auto* mc = e.components.get<MeshComponent>()) h = mixIn(h, modifiers::shown(e, *mc).revision);
        else if (const auto* mdl = e.components.get<ModelComponent>()) {
            h = mixIn(h, mdl->modelId);
            const LoadedModel* lm = models.byId(mdl->modelId);
            h = mixIn(h, lm ? lm->meshTris.size() : std::size_t(0));
        }
    }
    return h;
}

glm::mat4 boxAt(const glm::vec3& pos, const glm::vec3& normal, float size, float spinDeg) {
    const glm::vec3 up = glm::length(normal) > 1e-6f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
    // Any direction across the surface, then turned by the spin.
    glm::vec3 x = glm::cross(up, std::abs(up.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
    x = glm::normalize(x);
    const float a = glm::radians(spinDeg);
    x = glm::normalize(x * std::cos(a) + glm::cross(up, x) * std::sin(a));
    const glm::vec3 z = glm::cross(x, up);
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(x * size, 0.0f);
    m[1] = glm::vec4(up * size * 0.5f, 0.0f);
    m[2] = glm::vec4(z * size, 0.0f);
    m[3] = glm::vec4(pos, 1.0f);
    return m;
}

std::vector<unsigned char> bulletHolePixels(int size) {
    size = std::max(size, 8);
    std::vector<unsigned char> px(static_cast<std::size_t>(size) * size * 4);
    auto hash = [](int x, int y) {
        std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xffff) / 65535.0f;
    };
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
            const float r = std::sqrt(u * u + v * v);
            const float ang = std::atan2(v, u);
            // A ragged rim: the radius wobbles round the hole, the soot is spotty.
            const float rim  = 0.16f + 0.03f * std::sin(ang * 7.0f) + 0.02f * std::sin(ang * 13.0f + 1.3f);
            const float soot = 0.55f + 0.12f * std::sin(ang * 5.0f + 0.7f);
            float a, shade;
            if (r < rim) {           // the hole itself
                a = 1.0f;
                shade = 0.06f;
            } else if (r < rim + 0.05f) {   // its torn edge, a little lighter
                a = 1.0f;
                shade = 0.22f;
            } else if (r < soot) {          // the ring of soot, fading out
                const float k = 1.0f - (r - rim - 0.05f) / (soot - rim - 0.05f);
                a = std::clamp(k * k * (0.55f + 0.45f * hash(x, y)), 0.0f, 1.0f);
                shade = 0.12f;
            } else {
                a = 0.0f;
                shade = 0.0f;
            }
            unsigned char* d = &px[(static_cast<std::size_t>(y) * size + x) * 4];
            const auto c = static_cast<unsigned char>(std::clamp(shade * 255.0f, 0.0f, 255.0f));
            d[0] = c;
            d[1] = c;
            d[2] = static_cast<unsigned char>(std::clamp(shade * 240.0f, 0.0f, 255.0f));
            d[3] = static_cast<unsigned char>(std::clamp(a * 255.0f, 0.0f, 255.0f));
        }
    return px;
}

std::shared_ptr<fitzel::Texture> bulletHoleTexture() {
    const std::vector<unsigned char> px = bulletHolePixels(128);
    return std::make_shared<fitzel::Texture>(fitzel::Texture::fromPixels(px.data(), 128, 128, 4));
}

System::System()  = default;
System::~System() = default;

void System::update(const std::vector<Entity>& entities, ModelLibrary& models, const HeightFn& terrain) {
    for (auto& [id, p] : m_placed) p.seen = false;
    for (const Entity& e : entities) {
        const auto* dc = e.components.get<DecalComponent>();
        if (!dc || !e.activeInHierarchy) continue;
        const glm::mat4 box = scenegraph::compose(e.center, e.rotation, glm::max(e.half * 2.0f, glm::vec3(1e-3f)));
        Receivers who;
        who.objects = dc->onObjects;
        who.terrain = dc->onTerrain && static_cast<bool>(terrain);
        who.skip    = e.id;
        std::uint64_t sig = signature(entities, models, box, who);
        sig = mixIn(sig, dc->lift);
        sig = mixIn(sig, dc->maxAngle);
        Placed& p = m_placed[e.id];
        p.seen = true;
        if (p.sig == sig) continue;
        p.sig = sig;
        std::vector<Tri> tris;
        gather(entities, models, terrain, box, who, tris);
        const fitzel::MeshData md = project(box, tris, dc->lift, dc->maxAngle);
        p.has = !md.indices.empty();
        if (p.has) p.mesh = fitzel::Mesh::create(md);
    }
    for (auto it = m_placed.begin(); it != m_placed.end();)
        it = it->second.seen ? std::next(it) : m_placed.erase(it);
}

const fitzel::Mesh* System::meshFor(int id) const {
    const auto it = m_placed.find(id);
    return it != m_placed.end() && it->second.has ? &it->second.mesh : nullptr;
}

bool System::spawn(const fitzel::AssetId& material, const glm::vec3& pos, const glm::vec3& normal, float size,
                   float spinDeg, const std::vector<Entity>& entities, ModelLibrary& models, const HeightFn& terrain) {
    const glm::mat4 box = boxAt(pos, normal, std::max(size, 0.01f), spinDeg);
    Receivers who;
    who.terrain = static_cast<bool>(terrain);
    std::vector<Tri> tris;
    gather(entities, models, terrain, box, who, tris);
    // A hole lies flat on its surface: anything steeper than this is a corner.
    const fitzel::MeshData md = project(box, tris, 0.004f, 60.0f);
    if (md.indices.empty()) return false;
    Thrown t{material, fitzel::Mesh::create(md)};
    if (m_thrown.size() < kMaxThrown) {
        m_thrown.push_back(std::move(t));
    } else {
        m_thrown[m_next] = std::move(t);
        m_next = (m_next + 1) % kMaxThrown;
    }
    return true;
}

void System::submitThrown(fitzel::Renderer& renderer, const std::vector<fitzel::Material>& gpuMats,
                          const std::vector<MaterialDef>& materials, const Document& document) const {
    for (const Thrown& t : m_thrown) {
        const int mi = document.materialIndex(t.material);
        if (mi < 0 || mi >= static_cast<int>(gpuMats.size()) || mi >= static_cast<int>(materials.size())) continue;
        const MaterialDef& md = materials[static_cast<std::size_t>(mi)];
        renderer.submit(t.mesh, gpuMats[static_cast<std::size_t>(mi)], glm::mat4(1.0f),
                        /*castsPointShadow=*/false, /*reflective=*/false, md.opacity,
                        /*forceTransparent=*/md.alphaMode == AlphaMode::Blend || md.glass);
    }
}

void System::clearThrown() {
    m_thrown.clear();
    m_next = 0;
}

} // namespace decals
