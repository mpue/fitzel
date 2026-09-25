#include "WorldTrace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <thread>
#include <unordered_map>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/world/Terrain.hpp>

#include "CloudShadow.hpp"
#include "FarTerrain.hpp"
#include "FrameRender.hpp"
#include "Motes.hpp"
#include "ParticleSystem.hpp"
#include "PathTraceCapture.hpp"
#include "Primitives.hpp"
#include "RainRenderer.hpp"
#include "RiverSystem.hpp"
#include "SpraySystem.hpp"
#include "VegetationSystem.hpp"
#include "Wildlife.hpp"

namespace worldtrace {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// A linear colour handed to a field that the tracer reads as sRGB (it applies
// the same pow(2.2) lit.frag does). The unlit things -- particles, rain, pollen
// -- write straight into the HDR target, so their colours ARE linear already.
glm::vec3 toSrgb(const glm::vec3& linear) {
    return glm::pow(glm::max(linear, glm::vec3(0.0f)), glm::vec3(1.0f / 2.2f));
}

// Materials made up on the spot (a bird's colour, a spark's), deduplicated on
// the fields that tell them apart, quantised so a cloud of particles fading out
// collapses onto a handful rather than one material each.
class MaterialTable {
public:
    explicit MaterialTable(pathtrace::Scene& s) : m_scene(s) {}
    int get(const pathtrace::Material& m) {
        auto q = [](float v) { return static_cast<int>(std::lround(v * 64.0f)); };
        const std::array<int, 16> key = {
            q(m.albedo.r), q(m.albedo.g), q(m.albedo.b), q(m.emission.r),
            q(m.emission.g), q(m.emission.b), q(m.emissionStrength), q(m.opacity),
            q(m.roughness), q(m.translucency), m.texture, m.alphaMode,
            m.additive ? 1 : 0, m.glass ? 1 : 0, q(m.alphaCutoff), q(m.reflectivity)};
        auto it = m_map.find(key);
        if (it != m_map.end()) return it->second;
        const int idx = static_cast<int>(m_scene.materials.size());
        m_scene.materials.push_back(m);
        m_map.emplace(key, idx);
        return idx;
    }
private:
    pathtrace::Scene& m_scene;
    std::map<std::array<int, 16>, int> m_map;
};

// A soft round sprite, generated rather than read: the particle shader draws
// its "no texture" dot procedurally, and so does the spray's and the pollen's.
// `profile` is alpha as a function of the distance from the centre (0..1).
template <typename F>
int dotTexture(pathtrace::Scene& scene, F profile) {
    constexpr int N = 32;
    pathtrace::Image img;
    img.width = img.height = N;
    img.pixels.resize(static_cast<std::size_t>(N) * N * 4);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / N * 2.0f - 1.0f;
            const float v = (static_cast<float>(y) + 0.5f) / N * 2.0f - 1.0f;
            const float a = glm::clamp(profile(std::sqrt(u * u + v * v)), 0.0f, 1.0f);
            unsigned char* p = &img.pixels[(static_cast<std::size_t>(y) * N + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
        }
    scene.textures.push_back(std::move(img));
    return static_cast<int>(scene.textures.size()) - 1;
}

// Two triangles: a quad with corners a, b, c, d (in order around it).
void addQuad(std::vector<pathtrace::Triangle>& out, const glm::vec3& a, const glm::vec3& b,
             const glm::vec3& c, const glm::vec3& d, const glm::vec3& n, int material) {
    pathtrace::Triangle t0, t1;
    t0.p0 = a; t0.p1 = b; t0.p2 = c;
    t0.uv0 = {0, 0}; t0.uv1 = {1, 0}; t0.uv2 = {1, 1};
    t1.p0 = a; t1.p1 = c; t1.p2 = d;
    t1.uv0 = {0, 0}; t1.uv1 = {1, 1}; t1.uv2 = {0, 1};
    t0.n0 = t0.n1 = t0.n2 = t1.n0 = t1.n1 = t1.n2 = n;
    t0.material = t1.material = material;
    out.push_back(t0);
    out.push_back(t1);
}

// --- Trees -------------------------------------------------------------------
// tree.frag's colour correction: hue about the grey axis, contrast about mid
// grey, then brightness, clamped -- applied to (map * tint), in sRGB.
glm::vec3 treeCorrect(glm::vec3 c, float hueRad, float contrast, float brightness) {
    const glm::vec3 k(0.57735026919f);
    const float ch = std::cos(hueRad), sh = std::sin(hueRad);
    c = c * ch + glm::cross(k, c) * sh + k * glm::dot(k, c) * (1.0f - ch);
    c = (c - 0.5f) * contrast + 0.5f;
    c *= brightness;
    return glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
}

void addTrees(pathtrace::Scene& scene, const VegetationSystem& veg, float radius,
              const glm::vec3& eye, std::unordered_map<const fitzel::Texture*, int>& texCache,
              std::vector<std::string>& notes) {
    const auto& species = veg.species();
    std::vector<std::vector<float>> per;
    veg.gatherTrees(glm::vec2(eye.x, eye.z), radius, per);

    const bool corrected = std::fabs(veg.treeBrightness - 1.0f) > 1e-3f ||
                           std::fabs(veg.treeContrast - 1.0f) > 1e-3f ||
                           std::fabs(veg.treeHue) > 1e-3f;
    const float hue = glm::radians(veg.treeHue);

    long long trees = 0, placedTris = 0;
    int meshes = 0;
    for (std::size_t s = 0; s < species.size() && s < per.size(); ++s) {
        const VegetationSystem::TreeSpecies& sp = species[s];
        const std::vector<float>& inst = per[s];
        if (inst.empty() || sp.lods.empty()) continue;

        // One scene mesh per level of detail, built only if a tree needs it.
        std::vector<int> lodMesh(sp.lods.size(), -2);   // -2 unbuilt, -1 unusable
        auto meshFor = [&](std::size_t k) -> int {
            if (lodMesh[k] != -2) return lodMesh[k];
            const VegetationSystem::TreeLOD& lod = sp.lods[k];
            if (lod.cpuVerts.empty() || lod.cpuIdx.empty()) return lodMesh[k] = -1;
            pathtrace::Mesh mesh;
            for (const VegetationSystem::TreeLOD::Prim& pr : lod.prims) {
                // The part's material, as tree.frag draws it: the map (or the
                // swapped one) times the tint, corrected; or the glTF colour
                // where there is no map. Leaves are cut out and let light
                // through; bark does neither. No specular -- the shader has none.
                pathtrace::Material m;
                m.roughness    = 0.85f;
                m.reflectivity = 0.0f;
                const fitzel::Texture* tex =
                    (pr.swapTex && pr.swapTex->isValid()) ? pr.swapTex.get()
                    : (pr.hasTex ? &pr.tex : nullptr);
                int ti = tex ? pathcapture::addTexture(scene, tex, 1024, texCache) : -1;
                if (ti >= 0 && corrected) {
                    // The correction is not a tint -- contrast and hue are not
                    // multiplications -- so the map is corrected texel by texel
                    // into a copy of its own.
                    pathtrace::Image img = scene.textures[static_cast<std::size_t>(ti)];
                    for (std::size_t p = 0; p + 3 < img.pixels.size(); p += 4) {
                        glm::vec3 c(img.pixels[p], img.pixels[p + 1], img.pixels[p + 2]);
                        c = treeCorrect(c / 255.0f * pr.tint, hue, veg.treeContrast,
                                        veg.treeBrightness);
                        for (int ch = 0; ch < 3; ++ch)
                            img.pixels[p + ch] =
                                static_cast<unsigned char>(c[ch] * 255.0f + 0.5f);
                    }
                    scene.textures.push_back(std::move(img));
                    ti = static_cast<int>(scene.textures.size()) - 1;
                    m.tint = glm::vec3(1.0f);
                } else {
                    m.tint = pr.tint;
                }
                m.texture = ti;
                m.albedo  = treeCorrect(pr.baseColor * pr.tint, hue, veg.treeContrast,
                                        veg.treeBrightness);
                if (pr.cutout) {
                    m.alphaMode    = 1;
                    m.alphaCutoff  = pr.cutoff;
                    m.translucency = 0.35f;
                }
                const int mi = static_cast<int>(scene.materials.size());
                scene.materials.push_back(m);

                for (int i = pr.first; i + 2 < pr.first + pr.count; i += 3) {
                    const std::uint32_t ix[3] = {lod.cpuIdx[static_cast<std::size_t>(i)],
                                                 lod.cpuIdx[static_cast<std::size_t>(i + 1)],
                                                 lod.cpuIdx[static_cast<std::size_t>(i + 2)]};
                    pathtrace::Triangle t;
                    glm::vec3* P[3] = {&t.p0, &t.p1, &t.p2};
                    glm::vec3* N[3] = {&t.n0, &t.n1, &t.n2};
                    glm::vec2* U[3] = {&t.uv0, &t.uv1, &t.uv2};
                    bool ok = true;
                    for (int c = 0; c < 3; ++c) {
                        const std::size_t o = static_cast<std::size_t>(ix[c]) * 8;
                        if (o + 7 >= lod.cpuVerts.size()) { ok = false; break; }
                        const float* v = &lod.cpuVerts[o];
                        *P[c] = glm::vec3(v[0], v[1], v[2]);
                        *N[c] = glm::vec3(v[3], v[4], v[5]);
                        *U[c] = glm::vec2(v[6], v[7]);
                    }
                    if (!ok) continue;
                    // Imported models carry zero-area triangles; they cost a
                    // leaf and can only ever be hit edge-on.
                    const glm::vec3 cr = glm::cross(t.p1 - t.p0, t.p2 - t.p0);
                    if (glm::dot(cr, cr) < 1e-20f) continue;
                    t.material = mi;
                    mesh.triangles.push_back(t);
                }
            }
            if (mesh.triangles.empty()) return lodMesh[k] = -1;
            lodMesh[k] = static_cast<int>(scene.meshes.size());
            scene.meshes.push_back(std::move(mesh));
            ++meshes;
            return lodMesh[k];
        };

        for (std::size_t t = 0; t + 4 < inst.size(); t += 5) {
            const glm::vec3 pos(inst[t], inst[t + 1], inst[t + 2]);
            const float yaw = inst[t + 3], scale = inst[t + 4];
            // The level the viewport would draw at this distance -- the finer
            // one in front of each band's edge. Past the last mesh level the
            // viewport switches to impostors; the tracer keeps the coarsest
            // mesh instead, which is the real tree rather than a card of it.
            const float d = glm::distance(pos, eye);
            std::size_t k = 0;
            while (k + 1 < sp.lods.size() && d >= sp.lods[k].dist) ++k;
            int mesh = meshFor(k);
            for (std::size_t j = k; mesh < 0 && j-- > 0;) mesh = meshFor(j);
            if (mesh < 0) continue;
            // tree.vert: scaled, then turned about +Y by the yaw, then placed.
            const float c = std::cos(yaw), sn = std::sin(yaw);
            pathtrace::Instance in;
            in.transform = glm::mat4(glm::vec4(c * scale, 0.0f, sn * scale, 0.0f),
                                     glm::vec4(0.0f, scale, 0.0f, 0.0f),
                                     glm::vec4(-sn * scale, 0.0f, c * scale, 0.0f),
                                     glm::vec4(pos, 1.0f));
            in.mesh = mesh;
            scene.instances.push_back(in);
            placedTris += static_cast<long long>(
                scene.meshes[static_cast<std::size_t>(mesh)].triangles.size());
            ++trees;
        }
    }
    if (trees > 0) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "trees: %lld within %.0f m, %d meshes placed (%.1fM triangles "
                      "as seen, stored once per level)",
                      trees, radius, meshes, static_cast<double>(placedTris) / 1e6);
        notes.emplace_back(buf);
    }
}

// --- Flowers -------------------------------------------------------------------
// flower.vert, at rest: the per-bloom randoms from the world position, the
// petal count, the cup and the lean, then the yaw and the scale.
void addFlowers(pathtrace::Scene& scene, MaterialTable& mats, const VegetationSystem& veg,
                float radius, const glm::vec3& eye, std::vector<std::string>& notes) {
    const std::vector<float>& inst = veg.flowerInstances();
    if (inst.empty() || !veg.flowerEnabled) return;
    const std::vector<float> base = makeFlowerMesh();   // pos3 nrm3 tint petal
    const auto fract = [](float v) { return v - std::floor(v); };
    const auto rnd = [&](const glm::vec2& p, float salt) {
        return fract(std::sin(glm::dot(p, glm::vec2(12.9898f, 78.233f)) + salt) * 43758.5453f);
    };
    const auto rotY = [](const glm::vec3& v, float c, float s) {
        return glm::vec3(v.x * c - v.z * s, v.y, v.x * s + v.z * c);
    };
    pathtrace::Material stem;
    stem.albedo = glm::vec3(0.11f, 0.20f, 0.06f);
    stem.roughness = 0.8f;
    stem.translucency = 0.25f;
    const int stemMat = mats.get(stem);
    pathtrace::Material centre = stem;
    centre.albedo = glm::vec3(0.96f, 0.82f, 0.16f);
    centre.translucency = 0.0f;
    const int centreMat = mats.get(centre);

    const float r2max = radius * radius;
    long long blooms = 0;
    for (std::size_t f = 0; f + 7 < inst.size(); f += 8) {
        const glm::vec3 iPos(inst[f], inst[f + 1], inst[f + 2]);
        const float dx = iPos.x - eye.x, dz = iPos.z - eye.z;
        if (dx * dx + dz * dz > r2max) continue;
        const float yaw = inst[f + 3], scale = inst[f + 4];
        pathtrace::Material petal = stem;
        petal.albedo = glm::vec3(inst[f + 5], inst[f + 6], inst[f + 7]);
        petal.translucency = 0.3f;
        const int petalMat = mats.get(petal);

        const glm::vec2 xz(iPos.x, iPos.z);
        const float r1 = rnd(xz, 0.0f), r2 = rnd(xz, 1.7f), r3 = rnd(xz, 3.3f),
                    r4 = rnd(xz, 5.1f);
        const float petals = std::floor(5.0f + r1 * 4.0f);
        const float tilt   = (r2 - 0.5f) * 0.7f;
        const float lean   = r3 * 0.20f;
        const float leanA  = r4 * 2.0f * kPi;
        const glm::vec3 ax(std::cos(leanA), 0.0f, std::sin(leanA));
        const float cl = std::cos(lean), sl = std::sin(lean);
        const float cy = std::cos(yaw), sy = std::sin(yaw);

        pathtrace::Triangle tri;
        int corner = 0;
        bool keep = true;
        for (std::size_t v = 0; v + 7 < base.size(); v += 8) {
            glm::vec3 lp(base[v], base[v + 1], base[v + 2]);
            glm::vec3 nrm(base[v + 3], base[v + 4], base[v + 5]);
            const float tint = base[v + 6], pIdx = base[v + 7];
            if (corner == 0) keep = true;
            if (pIdx >= 0.0f) {
                if (pIdx > petals - 0.5f) keep = false;
                const float da = pIdx * (2.0f * kPi / petals - 2.0f * kPi / kFlowerMaxPetals);
                lp  = rotY(lp, std::cos(da), std::sin(da));
                nrm = rotY(nrm, std::cos(da), std::sin(da));
                const glm::vec2 rad(lp.x, lp.z);
                const float rl = glm::length(rad);
                if (rl > 1e-5f) {
                    const glm::vec2 rd = rad / rl, td(-rd.y, rd.x);
                    const float dy = lp.y - kFlowerStemTop;
                    const float ct = std::cos(tilt), st = std::sin(tilt);
                    const float r2d = rl * ct - dy * st;
                    lp = glm::vec3(rd.x * r2d, kFlowerStemTop + rl * st + dy * ct, rd.y * r2d);
                    const float nr = glm::dot(glm::vec2(nrm.x, nrm.z), rd);
                    const float nt = glm::dot(glm::vec2(nrm.x, nrm.z), td);
                    const glm::vec2 nxz = rd * (nr * ct - nrm.y * st) + td * nt;
                    nrm = glm::vec3(nxz.x, nr * st + nrm.y * ct, nxz.y);
                }
            }
            lp  = lp * cl + glm::cross(ax, lp) * sl + ax * glm::dot(ax, lp) * (1.0f - cl);
            nrm = nrm * cl + glm::cross(ax, nrm) * sl + ax * glm::dot(ax, nrm) * (1.0f - cl);
            lp  = rotY(lp, cy, sy) * scale;
            nrm = rotY(nrm, cy, sy);
            glm::vec3* P[3] = {&tri.p0, &tri.p1, &tri.p2};
            glm::vec3* N[3] = {&tri.n0, &tri.n1, &tri.n2};
            *P[corner] = iPos + lp;
            *N[corner] = nrm;
            if (++corner == 3) {
                corner = 0;
                if (!keep) continue;
                const glm::vec3 cr = glm::cross(tri.p1 - tri.p0, tri.p2 - tri.p0);
                if (glm::dot(cr, cr) < 1e-20f) continue;
                tri.material = tint < 0.5f ? stemMat : tint < 1.5f ? petalMat : centreMat;
                scene.triangles.push_back(tri);
            }
        }
        ++blooms;
    }
    if (blooms > 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "flowers: %lld blooms within %.0f m", blooms, radius);
        notes.emplace_back(buf);
    }
}

// --- Water -----------------------------------------------------------------------
// Water as the tracer understands it: glass with a BODY (Material::absorption,
// mediumColor). water.frag mixes the water's own colour over what it refracts by
// thickness with exactly these extinction coefficients; here the thickness is
// simply how far the ray went.
pathtrace::Material waterMaterial(const glm::vec3& colour, float clarity, float ior) {
    pathtrace::Material m;
    m.albedo      = glm::vec3(1.0f);
    m.roughness   = 0.02f;
    m.glass       = true;
    m.ior         = std::max(1.0f, ior);
    m.absorption  = glm::vec3(0.30f, 0.14f, 0.10f) / std::max(clarity, 0.05f);
    m.mediumColor = colour;
    m.alphaMode   = 2;
    return m;
}

void addLake(pathtrace::Scene& scene, const Sources& src, float extent, const glm::vec3& eye,
             std::vector<std::string>& notes) {
    if (src.waterLevel <= -999.0f) return;
    const int mi = static_cast<int>(scene.materials.size());
    scene.materials.push_back(waterMaterial(src.waterColor, src.waterClarity, src.waterIor));
    const float y = src.waterLevel, e = extent;
    // A grid of quads rather than two vast triangles: the accelerator splits a
    // big flat surface into boxes that cull, and one 30-km triangle is a box
    // every ray in the picture opens.
    constexpr int kCells = 16;
    const float step = 2.0f * e / kCells;
    for (int j = 0; j < kCells; ++j)
        for (int i = 0; i < kCells; ++i) {
            const float x0 = eye.x - e + i * step, z0 = eye.z - e + j * step;
            addQuad(scene.triangles, {x0, y, z0 + step}, {x0 + step, y, z0 + step},
                    {x0 + step, y, z0}, {x0, y, z0}, {0, 1, 0}, mi);
        }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "lake: a flat water surface at %.1f m, %.0f m out "
                  "(the viewport's waves are not traced)", y, e);
    notes.emplace_back(buf);
}

void addRivers(pathtrace::Scene& scene, const RiverSystem& rivers,
               std::vector<std::string>& notes) {
    const auto& runs = rivers.runs();
    int n = 0;
    long long tris = 0;
    for (std::size_t i = 0; i < runs.size() && i < rivers.paths.size(); ++i) {
        if (!rivers.paths[i].enabled || runs[i].mesh.vertexCount() == 0) continue;
        const rivergen::Style& st = rivers.paths[i].style;
        const int mi = static_cast<int>(scene.materials.size());
        // The river's deep colour is the body's colour; its clarity how far the
        // bed shows through (river.frag: exp(-depth / clarity)).
        pathtrace::Material m = waterMaterial(st.deep, st.clarity / 1.5f, 1.33f);
        scene.materials.push_back(m);
        const fitzel::MeshData data = runs[i].mesh.readback();   // world space
        const std::size_t count = data.indices.empty() ? data.vertices.size() / 3
                                                       : data.indices.size() / 3;
        for (std::size_t t = 0; t < count; ++t) {
            std::uint32_t ix[3];
            for (int c = 0; c < 3; ++c)
                ix[c] = data.indices.empty() ? static_cast<std::uint32_t>(t * 3 + c)
                                             : data.indices[t * 3 + static_cast<std::size_t>(c)];
            if (ix[0] >= data.vertices.size() || ix[1] >= data.vertices.size() ||
                ix[2] >= data.vertices.size())
                continue;
            pathtrace::Triangle tri;
            tri.p0 = data.vertices[ix[0]].position; tri.n0 = data.vertices[ix[0]].normal;
            tri.p1 = data.vertices[ix[1]].position; tri.n1 = data.vertices[ix[1]].normal;
            tri.p2 = data.vertices[ix[2]].position; tri.n2 = data.vertices[ix[2]].normal;
            tri.uv0 = data.vertices[ix[0]].uv; tri.uv1 = data.vertices[ix[1]].uv;
            tri.uv2 = data.vertices[ix[2]].uv;
            tri.material = mi;
            scene.triangles.push_back(tri);
            ++tris;
        }
        ++n;
    }
    if (n > 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "rivers: %d watercourses, %lld triangles", n, tris);
        notes.emplace_back(buf);
    }
}

// --- The far terrain -------------------------------------------------------------
// FarTerrain's rings, rebuilt from the height function itself: ring i has cells
// of 8 * 2^i metres and reaches 1024 * 2^i out, and leaves a hole where the ring
// inside it (or the streamed near terrain) already covers the ground. A vertex in
// the hole is SUNK rather than dropped, as farterrain.vert sinks it, so the edge
// of a ring hangs down behind the finer ground instead of leaving a crack.
void addFarTerrain(pathtrace::Scene& scene, const Sources& src, float reach,
                   const glm::vec3& eye, std::vector<std::string>& notes) {
    if (!src.terrain || !src.farTerrainOn) return;
    // The far ground's own material: farterrain.frag's colour, worked out per
    // hit from height, slope, moisture and the ecology (Material::farGround).
    // Its look is the far terrain's, taken from the far terrain.
    pathtrace::GroundLook& g = scene.ground;
    if (src.farLook) {
        g.ecoOn        = src.farLook->eco.enabled;
        g.ecoCover     = src.farLook->eco.cover;
        g.ecoStandSize = std::max(1.0f, src.farLook->eco.standSize);
        g.ecoTreeLine  = src.farLook->eco.treeLine;
        g.ecoWater     = src.farLook->eco.waterLevel;
        g.ecoSolitary  = src.farLook->eco.solitary;
        g.ecoSlopeLove = src.farLook->eco.slopeLove;
        g.farTreeLine  = src.farLook->treeLine;
        g.farSnowLevel = src.farLook->snowLevel;
        g.canopy       = src.farLook->canopy;
        g.grassTint    = src.farLook->grassTint;
        g.waterLevel   = src.farLook->waterLevel;
    }
    g.airSun = scene.fog.sunColor;
    pathtrace::Material fm;
    fm.farGround = true;
    fm.roughness = 0.95f;
    const int mat = static_cast<int>(scene.materials.size());
    scene.materials.push_back(fm);

    struct Ring { float cell, half; glm::vec2 origin; glm::vec2 holeLo, holeHi; };
    std::vector<Ring> rings;
    glm::vec2 innerLo = src.nearMin, innerHi = src.nearMax;
    for (int i = 0; i < 5; ++i) {
        Ring r;
        r.cell = 8.0f * static_cast<float>(1 << i);
        r.half = 1024.0f * static_cast<float>(1 << i);
        if (r.half * 0.5f > reach) break;
        // Snapped to two cells, as FarTerrain snaps it.
        const float snap = 2.0f * r.cell;
        r.origin = glm::vec2(std::floor((eye.x - r.half) / snap) * snap,
                             std::floor((eye.z - r.half) / snap) * snap);
        r.holeLo = innerLo + glm::vec2(1.5f * r.cell);
        r.holeHi = innerHi - glm::vec2(1.5f * r.cell);
        rings.push_back(r);
        innerLo = r.origin;
        innerHi = r.origin + glm::vec2(2.0f * r.half);
    }
    if (rings.empty()) return;

    // Heights and moisture, one ring per thread: a quarter of a million
    // evaluations each of functions that walk every edit on the terrain.
    constexpr int kGrid = 257;
    std::vector<std::vector<float>> heights(rings.size()), moist(rings.size());
    {
        std::vector<std::thread> pool;
        for (std::size_t ri = 0; ri < rings.size(); ++ri)
            pool.emplace_back([&, ri] {
                const Ring& r = rings[ri];
                std::vector<float>& h = heights[ri];
                std::vector<float>& m = moist[ri];
                h.resize(static_cast<std::size_t>(kGrid) * kGrid);
                m.resize(h.size());
                for (int j = 0; j < kGrid; ++j)
                    for (int i = 0; i < kGrid; ++i) {
                        const float x = r.origin.x + i * r.cell, z = r.origin.y + j * r.cell;
                        const std::size_t k = static_cast<std::size_t>(j) * kGrid + i;
                        h[k] = fitzel::terrainHeight(*src.terrain, x, z);
                        m[k] = fitzel::terrainMoisture(*src.terrain, x, z);
                    }
            });
        for (std::thread& t : pool) t.join();
    }

    long long tris = 0;
    for (std::size_t ri = 0; ri < rings.size(); ++ri) {
        const Ring& r = rings[ri];
        const std::vector<float>& h = heights[ri];
        auto inHole = [&](float x, float z) {
            return x > r.holeLo.x && x < r.holeHi.x && z > r.holeLo.y && z < r.holeHi.y;
        };
        auto vert = [&](int i, int j) {
            const float x = r.origin.x + i * r.cell, z = r.origin.y + j * r.cell;
            float y = h[static_cast<std::size_t>(j) * kGrid + i];
            if (inHole(x, z)) y -= 40.0f + 4.0f * r.cell;
            return glm::vec3(x, y, z);
        };
        auto normal = [&](int i, int j) {
            const int i0 = std::max(i - 1, 0), i1 = std::min(i + 1, kGrid - 1);
            const int j0 = std::max(j - 1, 0), j1 = std::min(j + 1, kGrid - 1);
            const float dx = h[static_cast<std::size_t>(j) * kGrid + i1] -
                             h[static_cast<std::size_t>(j) * kGrid + i0];
            const float dz = h[static_cast<std::size_t>(j1) * kGrid + i] -
                             h[static_cast<std::size_t>(j0) * kGrid + i];
            return glm::normalize(glm::vec3(-dx / ((i1 - i0) * r.cell), 1.0f,
                                            -dz / ((j1 - j0) * r.cell)));
        };
        for (int j = 0; j + 1 < kGrid; ++j)
            for (int i = 0; i + 1 < kGrid; ++i) {
                const float x0 = r.origin.x + i * r.cell, z0 = r.origin.y + j * r.cell;
                const float x1 = x0 + r.cell, z1 = z0 + r.cell;
                // A cell wholly inside the hole is the inner ground's business.
                if (inHole(x0, z0) && inHole(x1, z0) && inHole(x0, z1) && inHole(x1, z1))
                    continue;
                // Beyond the reach asked for: nothing.
                const float cx = 0.5f * (x0 + x1) - eye.x, cz = 0.5f * (z0 + z1) - eye.z;
                if (cx * cx + cz * cz > reach * reach) continue;
                const glm::vec3 a = vert(i, j), b = vert(i + 1, j);
                const glm::vec3 c = vert(i + 1, j + 1), d = vert(i, j + 1);
                pathtrace::Triangle t0, t1;
                t0.p0 = a; t0.p1 = d; t0.p2 = c;
                t0.n0 = normal(i, j); t0.n1 = normal(i, j + 1); t0.n2 = normal(i + 1, j + 1);
                t1.p0 = a; t1.p1 = c; t1.p2 = b;
                t1.n0 = t0.n0; t1.n1 = t0.n2; t1.n2 = normal(i + 1, j);
                // The moisture rides in uv.x (the far ground has no map to
                // spend its uvs on), interpolated across the cell like the
                // ring texture it stands for.
                auto mo = [&](int ii, int jj) {
                    return glm::vec2(moist[ri][static_cast<std::size_t>(jj) * kGrid + ii], 0.0f);
                };
                t0.uv0 = t1.uv0 = mo(i, j);
                t0.uv1 = mo(i, j + 1); t0.uv2 = t1.uv1 = mo(i + 1, j + 1);
                t1.uv2 = mo(i + 1, j);
                t0.material = t1.material = mat;
                scene.triangles.push_back(t0);
                scene.triangles.push_back(t1);
                tris += 2;
            }
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "far terrain: %zu rings to %.0f m, %lld triangles, "
                  "coloured and hazed as the far terrain colours them", rings.size(),
                  reach, tris);
    notes.emplace_back(buf);
}

// --- Creatures --------------------------------------------------------------------
// creature.vert at rest (wings spread, tail straight): bank about the body's
// forward axis, then pitch, then yaw, then the scale and the position.
void addCreatures(pathtrace::Scene& scene, MaterialTable& mats, const Wildlife& wl,
                  std::vector<std::string>& notes) {
    int count = 0;
    for (int kind = 0; kind < 3; ++kind) {
        const std::vector<float>& inst = wl.traceInstances(kind);
        if (inst.size() < 13) continue;
        const std::vector<float> v = Wildlife::traceMesh(kind);
        pathtrace::Mesh mesh;
        for (std::size_t i = 0; i + 17 < v.size(); i += 18) {
            pathtrace::Triangle t;
            t.p0 = glm::vec3(v[i], v[i + 1], v[i + 2]);
            t.p1 = glm::vec3(v[i + 6], v[i + 7], v[i + 8]);
            t.p2 = glm::vec3(v[i + 12], v[i + 13], v[i + 14]);
            const glm::vec3 n = glm::cross(t.p1 - t.p0, t.p2 - t.p0);
            if (glm::dot(n, n) < 1e-14f) continue;
            t.n0 = t.n1 = t.n2 = glm::normalize(n);
            t.uv0 = glm::vec2(v[i + 4], v[i + 5]);
            t.uv1 = glm::vec2(v[i + 10], v[i + 11]);
            t.uv2 = glm::vec2(v[i + 16], v[i + 17]);
            mesh.triangles.push_back(t);
        }
        if (mesh.triangles.empty()) continue;
        const int mi = static_cast<int>(scene.meshes.size());
        scene.meshes.push_back(std::move(mesh));
        for (std::size_t i = 0; i + 12 < inst.size(); i += 13) {
            const glm::vec3 pos(inst[i], inst[i + 1], inst[i + 2]);
            const float yaw = inst[i + 3], pitch = inst[i + 4], bank = inst[i + 5];
            const float scale = inst[i + 8];
            const float cb = std::cos(bank), sb = std::sin(bank);
            const float cp = std::cos(pitch), sp = std::sin(pitch);
            const float cy = std::cos(yaw), sy = std::sin(yaw);
            const glm::mat3 B(glm::vec3(cb, sb, 0), glm::vec3(-sb, cb, 0), glm::vec3(0, 0, 1));
            const glm::mat3 P(glm::vec3(1, 0, 0), glm::vec3(0, cp, -sp), glm::vec3(0, sp, cp));
            const glm::mat3 Y(glm::vec3(cy, 0, -sy), glm::vec3(0, 1, 0), glm::vec3(sy, 0, cy));
            const glm::mat3 R = Y * P * B * scale;
            pathtrace::Instance in;
            in.transform = glm::mat4(glm::vec4(R[0], 0.0f), glm::vec4(R[1], 0.0f),
                                     glm::vec4(R[2], 0.0f), glm::vec4(pos, 1.0f));
            in.mesh = mi;
            pathtrace::Material m;
            m.albedo    = glm::vec3(inst[i + 9], inst[i + 10], inst[i + 11]);
            m.roughness = kind == 2 ? 0.35f : 0.8f;
            m.reflectivity = kind == 2 ? 0.15f : 0.0f;
            m.translucency = kind == 1 ? 0.3f : 0.0f;
            in.material = mats.get(m);
            scene.instances.push_back(in);
            ++count;
        }
    }
    if (count > 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "wildlife: %d creatures, posed at rest", count);
        notes.emplace_back(buf);
    }
}

// --- Particles, spray, pollen, rain ----------------------------------------------
// All of them are billboards the viewport turns to face the camera; here they are
// turned to face the camera the still is taken from, which is the same thing.
// Unlit, so their colour is emission; additive ones light nothing and block
// nothing (Material::additive), blended ones cover by their alpha.

// World size of something the viewport draws `px` pixels across at `dist`.
float pixelsToWorld(float px, float dist, float fovDeg) {
    return px * dist * 2.0f * std::tan(glm::radians(fovDeg) * 0.5f) / 1080.0f;
}

void addParticles(pathtrace::Scene& scene, MaterialTable& mats, const Sources& src,
                  const glm::vec3& eye, const glm::vec3& camRight, const glm::vec3& camUp,
                  float fovDeg, std::unordered_map<const fitzel::Texture*, int>& texCache,
                  std::vector<std::string>& notes) {
    long long n = 0;
    int dotTex = -1;
    auto dot = [&] {
        if (dotTex < 0)
            dotTex = dotTexture(scene, [](float d) { const float a = 1.0f - d * d;
                                                     return a > 0.0f ? a * a : 0.0f; });
        return dotTex;
    };

    if (src.particles) {
        for (const ParticleSystem::Batch& b : src.particles->batches()) {
            const int tex = b.tex ? pathcapture::addTexture(scene, b.tex.get(), 512, texCache)
                                  : dot();
            for (std::size_t i = 0; i + 11 < b.data.size(); i += 12) {
                const float* p = &b.data[i];
                const glm::vec3 pos(p[0], p[1], p[2]);
                const float size = p[3], rot = p[4];
                const glm::vec3 rgb(p[5], p[6], p[7]);
                const float a = p[8];
                const glm::vec3 vel(p[9], p[10], p[11]);
                if (size <= 0.0f || a <= 0.002f) continue;
                glm::vec3 right = camRight, up = camUp;
                float sx = size, sy = size;
                if (b.stretch > 0.0f && glm::length(vel) > 1e-4f) {
                    // Stretched along the velocity, as the shader stretches it.
                    up = glm::normalize(vel);
                    const glm::vec3 viewDir = glm::normalize(pos - eye);
                    const glm::vec3 r = glm::cross(viewDir, up);
                    if (glm::dot(r, r) > 1e-10f) right = glm::normalize(r);
                    sy = size + glm::length(vel) * b.stretch;
                } else if (rot != 0.0f) {
                    const float c = std::cos(rot), s = std::sin(rot);
                    const glm::vec3 r2 = camRight * c + camUp * s;
                    up = -camRight * s + camUp * c;
                    right = r2;
                }
                pathtrace::Material m;
                m.albedo    = glm::vec3(0.0f);
                m.emission  = toSrgb(rgb);
                m.additive  = b.additive;
                m.alphaMode = 2;
                m.texture   = tex;
                m.opacity   = glm::clamp(a, 0.0f, 1.0f);
                const int mi = mats.get(m);
                const glm::vec3 hx = right * (0.5f * sx), hy = up * (0.5f * sy);
                addQuad(scene.triangles, pos - hx - hy, pos + hx - hy, pos + hx + hy,
                        pos - hx + hy, glm::normalize(eye - pos), mi);
                ++n;
            }
        }
    }

    if (src.spray) {
        // Droplets are round points a fixed number of PIXELS across at a given
        // distance (spray.vert); foam lies flat on the water.
        const int drop = dotTexture(scene, [](float d) { return 1.0f - d * d; });
        for (const SprayP& p : src.spray->pool().particles()) {
            const float life = glm::clamp(p.life / std::max(p.life0, 1e-4f), 0.0f, 1.0f);
            if (life <= 0.0f) continue;
            const float dist = std::max(glm::distance(p.pos, eye), 1.0f);
            const float px = glm::clamp(260.0f / dist * p.size * src.spray->sizeScale,
                                        1.0f, 200.0f);
            const float w = pixelsToWorld(px, dist, fovDeg);
            pathtrace::Material m;
            m.albedo    = glm::vec3(0.0f);
            m.alphaMode = 2;
            m.texture   = drop;
            if (p.flat > 0.5f) {
                m.emission = toSrgb(glm::vec3(0.94f, 0.97f, 1.0f));
                m.opacity  = life * life * 0.55f;
                const int mi = mats.get(m);
                const glm::vec3 c(p.pos.x, src.waterLevel + 0.03f, p.pos.z);
                const float h = 0.5f * w;
                addQuad(scene.triangles, c + glm::vec3(-h, 0, -h), c + glm::vec3(h, 0, -h),
                        c + glm::vec3(h, 0, h), c + glm::vec3(-h, 0, h), {0, 1, 0}, mi);
            } else {
                m.emission = toSrgb(glm::mix(glm::vec3(0.72f, 0.86f, 0.96f),
                                             glm::vec3(1.0f), life));
                m.opacity  = life * 0.8f;
                const int mi = mats.get(m);
                const glm::vec3 hx = camRight * (0.5f * w), hy = camUp * (0.5f * w);
                addQuad(scene.triangles, p.pos - hx - hy, p.pos + hx - hy, p.pos + hx + hy,
                        p.pos - hx + hy, glm::normalize(eye - p.pos), mi);
            }
            ++n;
        }
    }

    if (src.motes) {
        // mote.vert: light scattered forward towards the eye, a disc never
        // smaller than a pixel (and dimmer for being made bigger), gone near the
        // eye and at the edge of the box. Additive, like the viewport.
        const std::vector<float>& d = src.motes->traceData();
        const int gauss = dotTexture(scene, [](float r) {
            return std::max(std::exp(-3.0f * r * r) - 0.05f, 0.0f) / 0.95f; });
        const float clear = src.motes->traceClear();
        const glm::vec3 L = glm::normalize(src.sunDir);
        auto hg = [](float c, float g) {
            const float g2 = g * g;
            return (1.0f - g2) / std::pow(1.0f + g2 - 2.0f * g * c, 1.5f);
        };
        for (std::size_t i = 0; i + 5 < d.size(); i += 6) {
            const glm::vec3 pos(d[i], d[i + 1], d[i + 2]);
            const float size = d[i + 3], kind = d[i + 4];
            const glm::vec3 toM = pos - eye;
            const float dist = std::max(glm::length(toM), 1e-3f);
            const float cosT = glm::dot(toM / dist, L);
            const float drawR = std::max(size, 1.1f * pixelsToWorld(1.0f, dist, fovDeg));
            const float spread = (size * size) / (drawR * drawR);
            glm::vec3 tint; float scatter;
            if (kind < 0.5f)      { tint = {1.0f, 0.86f, 0.55f}; scatter = 0.9f * hg(cosT, 0.78f) + 0.05f; }
            else if (kind < 1.5f) { tint = glm::vec3(1.0f);      scatter = 0.45f * hg(cosT, 0.55f) + 0.30f; }
            else                  { tint = {0.95f, 0.92f, 0.88f}; scatter = 1.2f * hg(cosT, 0.85f); }
            const float fade = glm::smoothstep(0.35f, 1.4f, dist) *
                               (1.0f - glm::smoothstep(src.motesRadius * 0.55f,
                                                       src.motesRadius * 0.95f, dist));
            const glm::vec3 col = tint * (src.sunColor * scatter + src.ambient * 0.25f) *
                                  spread * fade * clear * 3.0f * 1.6f * 0.95f;
            if (glm::max(col.r, glm::max(col.g, col.b)) < 1e-4f) continue;
            pathtrace::Material m;
            m.albedo    = glm::vec3(0.0f);
            m.emission  = toSrgb(col);
            m.additive  = true;
            m.alphaMode = 2;
            m.texture   = gauss;
            const int mi = mats.get(m);
            const glm::vec3 right = glm::normalize(glm::cross(toM / dist, glm::vec3(0, 1, 0)) +
                                                   glm::vec3(1e-5f, 0, 0));
            const glm::vec3 up = glm::cross(right, toM / dist);
            addQuad(scene.triangles, pos - (right + up) * drawR, pos + (right - up) * drawR,
                    pos + (right + up) * drawR, pos - (right - up) * drawR, -toM / dist, mi);
            ++n;
        }
    }
    if (n > 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "particles: %lld sprites (emitters, spray, pollen)", n);
        notes.emplace_back(buf);
    }
}

void addRain(pathtrace::Scene& scene, MaterialTable& mats, const Sources& src,
             const glm::vec3& eye, float fovDeg, std::vector<std::string>& notes) {
    if (!src.rain) return;
    const std::vector<RainRenderer::Streak> streaks =
        src.rain->streaks(eye, src.time, src.weather);
    if (streaks.empty()) return;
    const float fall = rainIntensityFor(src.weather) *
                       glm::clamp(src.rain->amount, 0.0f, RainRenderer::kMaxAmount);
    const float intensity = glm::mix(0.55f, 1.0f, glm::min(fall, 1.0f));
    const glm::vec3 col = RainRenderer::color(src.ambient, src.sunColor);
    for (const RainRenderer::Streak& s : streaks) {
        // A line in the viewport, one pixel wide at whatever distance: a thin
        // ribbon here, as wide as a pixel is at its distance, turned to the eye.
        const glm::vec3 mid = 0.5f * (s.a + s.b);
        const float dist = std::max(glm::distance(mid, eye), 0.5f);
        const float w = std::max(pixelsToWorld(1.0f, dist, fovDeg), 0.002f);
        const glm::vec3 along = s.b - s.a;
        glm::vec3 side = glm::cross(along, mid - eye);
        if (glm::dot(side, side) < 1e-12f) continue;
        side = glm::normalize(side) * (0.5f * w);
        pathtrace::Material m;
        m.albedo    = glm::vec3(0.0f);
        m.emission  = toSrgb(col);
        m.alphaMode = 2;
        m.opacity   = 0.35f * intensity * s.fade;
        if (m.opacity <= 0.01f) continue;
        const int mi = mats.get(m);
        addQuad(scene.triangles, s.a - side, s.a + side, s.b + side, s.b - side,
                glm::normalize(eye - mid), mi);
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "rain: %zu streaks, frozen at the still's instant",
                  streaks.size());
    notes.emplace_back(buf);
}

// --- Clouds -------------------------------------------------------------------------
void addCloudShadow(pathtrace::Scene& scene, const CloudShadow& clouds,
                    std::vector<std::string>& notes) {
    std::vector<float> values;
    if (!clouds.readback(values)) return;
    const CloudShadowInfo& ci = cloudShadowInfo();
    pathtrace::SunMask& m = scene.sunMask;
    m.values = std::move(values);
    m.width  = CloudShadow::kRes;
    m.height = CloudShadow::kRes;
    m.origin = ci.origin;
    m.size   = ci.size;
    m.refY   = ci.refY;
    m.sunDir = glm::normalize(ci.sunDir);
    notes.emplace_back("clouds: their shadow on the ground, from the viewport's own map");
}

// --- The sky --------------------------------------------------------------------------
// The viewport's background rendered six times into the faces of a cube around
// the eye, and resampled into the equirectangular panorama the tracer reads. The
// faces are ordinary 90-degree views, each remembered by its own view-projection,
// so the resampling needs no cube-map convention -- it just projects.
bool captureSky(const Sources& src, const glm::vec3& eye, pathtrace::Environment& env) {
    constexpr int N = 256;          // per face
    constexpr int W = 1024, H = 512;
    GLint prevFbo = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    GLuint fbo = 0, tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, N, N, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    const glm::vec3 fwd[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const glm::vec3 up[6]  = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    glm::mat4 vps[6];
    std::vector<float> faces[6];
    if (complete) {
        glViewport(0, 0, N, N);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10000.0f);
        for (int f = 0; f < 6; ++f) {
            vps[f] = proj * glm::lookAt(eye, eye + fwd[f], up[f]);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            src.drawSky(glm::inverse(vps[f]), eye);
            faces[f].resize(static_cast<std::size_t>(N) * N * 4);
            glReadPixels(0, 0, N, N, GL_RGBA, GL_FLOAT, faces[f].data());
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (blend) glEnable(GL_BLEND);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    if (!complete) return false;

    env.backdropWidth  = W;
    env.backdropHeight = H;
    env.backdrop.assign(static_cast<std::size_t>(W) * H * 3, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            // The tracer's panorama layout: row 0 is the ground (bottom-up).
            const float u = (static_cast<float>(x) + 0.5f) / W;
            const float v = (static_cast<float>(y) + 0.5f) / H;
            const float elev = kPi * (v - 0.5f), phi = 2.0f * kPi * (u - 0.5f);
            const glm::vec3 d(std::cos(elev) * std::cos(phi), std::sin(elev),
                              std::cos(elev) * std::sin(phi));
            int best = 0;
            for (int f = 1; f < 6; ++f)
                if (glm::dot(d, fwd[f]) > glm::dot(d, fwd[best])) best = f;
            const glm::vec4 clip = vps[best] * glm::vec4(eye + d, 1.0f);
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            const float fx = (ndc.x * 0.5f + 0.5f) * N - 0.5f;
            const float fy = (ndc.y * 0.5f + 0.5f) * N - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, N - 1);
            const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, N - 1);
            const int x1 = std::min(x0 + 1, N - 1), y1 = std::min(y0 + 1, N - 1);
            const float tx = glm::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            const float ty = glm::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
            const std::vector<float>& F = faces[best];
            auto at = [&](int xx, int yy) {
                const std::size_t o = (static_cast<std::size_t>(yy) * N + xx) * 4;
                return glm::vec3(F[o], F[o + 1], F[o + 2]);
            };
            const glm::vec3 c = glm::mix(glm::mix(at(x0, y0), at(x1, y0), tx),
                                         glm::mix(at(x0, y1), at(x1, y1), tx), ty);
            float* o = &env.backdrop[(static_cast<std::size_t>(y) * W + x) * 3];
            // Non-finite texels (a sky shader dividing by a horizon) become black
            // rather than poisoning every pixel that sees them.
            for (int k = 0; k < 3; ++k) o[k] = std::isfinite(c[k]) ? std::max(c[k], 0.0f) : 0.0f;
        }
    return true;
}

} // namespace

void append(pathtrace::Scene& scene, const Sources& src, const Options& opt,
            const glm::vec3& eye, const glm::vec3& camRight, const glm::vec3& camUp,
            float fovDeg, bool preview, std::vector<std::string>& notes) {
    // The live preview re-harvests on every edit and has a frame budget; a
    // still has minutes. So the preview takes the near world only.
    const float treeR   = preview ? std::min(opt.treeRadius, 300.0f) : opt.treeRadius;
    const float flowerR = preview ? std::min(opt.flowerRadius, 25.0f) : opt.flowerRadius;
    const float farR    = preview ? std::min(opt.farRadius, 4000.0f) : opt.farRadius;

    std::unordered_map<const fitzel::Texture*, int> texCache;
    MaterialTable mats(scene);

    if (opt.trees && src.veg) {
        addTrees(scene, *src.veg, treeR, eye, texCache, notes);
        // Past the traced forest the floor takes the canopy's colour, as the
        // viewport's does past its tree meshes (GroundLook).
        scene.ground.canopyFrom = std::max(0.0f, treeR - 70.0f);
        scene.ground.canopyTo   = treeR;
    }
    if (src.moistTex && scene.ground.meadowOn()) {
        const int n = static_cast<int>(src.moistRect.w);
        if (n > 0) {
            std::vector<float> rg(static_cast<std::size_t>(n) * n * 2);
            GLint prevAlign = 4;
            glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glBindTexture(GL_TEXTURE_2D, src.moistTex);
            GLint w = 0, h = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
            if (w == n && h == n) {
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, rg.data());
                pathtrace::GroundLook& g = scene.ground;
                g.moisture.resize(static_cast<std::size_t>(n) * n);
                for (std::size_t i = 0; i < g.moisture.size(); ++i) g.moisture[i] = rg[i * 2 + 1];
                g.moistSamples = n;
                g.moistOrigin  = glm::vec2(src.moistRect.x, src.moistRect.y);
                g.moistCell    = src.moistRect.z;
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, prevAlign);
        }
    }
    if (opt.flowers && src.veg) addFlowers(scene, mats, *src.veg, flowerR, eye, notes);
    if (opt.water) {
        addLake(scene, src, src.farTerrainOn && opt.farTerrain ? farR : 3000.0f, eye, notes);
        if (src.rivers) addRivers(scene, *src.rivers, notes);
    }
    if (opt.farTerrain) addFarTerrain(scene, src, farR, eye, notes);
    if (opt.creatures && src.wildlife) addCreatures(scene, mats, *src.wildlife, notes);
    if (opt.particles)
        addParticles(scene, mats, src, eye, camRight, camUp, fovDeg, texCache, notes);
    if (opt.rain) addRain(scene, mats, src, eye, fovDeg, notes);
    if (opt.clouds && src.clouds) addCloudShadow(scene, *src.clouds, notes);
    if (opt.sky && src.drawSky && !src.skyIsLightingHdri) {
        if (captureSky(src, eye, scene.env))
            notes.emplace_back("sky: the viewport's own sky is what the camera sees; "
                               "the light still comes from the ambient it is lit by");
    }

    // The tracer keeps three terrain-paint weights per WORLD triangle, parallel
    // to the list; everything added above has none, but the table has to grow
    // with the list or the GPU tracer drops the paint for the whole scene.
    if (!scene.vertexPaint.empty())
        scene.vertexPaint.resize(scene.triangles.size() * 3, glm::vec4(0.0f));

    notes.emplace_back("not traced: wind (everything stands at rest), the lake's waves, "
                       "volumetric fog volumes");
}

} // namespace worldtrace
