#include "StalkerImport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <stb_image_write.h>

#include <fitzel/asset/AssetId.hpp>

#include "DdsDecode.hpp"
#include "StalkerLife.hpp"
#include "StalkerShared.hpp"
#include "XrArchive.hpp"
#include "XrLevel.hpp"

namespace stalker {

namespace fs = std::filesystem;
using nlohmann::json;

using namespace detail;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// --- what an X-Ray shader becomes ----------------------------------------------

enum class Surface { Opaque, Terrain, Cutout, Decal, Glass, Water, Emissive, Skip };

Surface classify(const xray::LevelShader& sh) {
    const std::string e = lower(sh.engine);
    if (sh.texture.empty()) return Surface::Skip;
    if (startsWith(e, "effects\\glow") || startsWith(e, "effects\\lightplanes") ||
        startsWith(e, "effects\\flare") || startsWith(e, "details\\"))
        return Surface::Skip;
    if (startsWith(e, "effects\\wallmark")) return Surface::Decal;
    if (startsWith(e, "effects\\water")) return Surface::Water;
    if (startsWith(e, "selflight")) return Surface::Emissive;
    if (startsWith(e, "levels\\")) return Surface::Terrain;
    if (e.find("aref") != std::string::npos || e.find("trans") != std::string::npos ||
        startsWith(e, "flora\\leaf"))
        return Surface::Cutout;
    return Surface::Opaque;
}

// An OGF model's shader ("models\model", "models\window", ...).
Surface classifyModel(const std::string& shader) {
    const std::string e = lower(shader);
    for (const char* fx : {"lightplanes", "glow", "flare", "distort", "particle"})
        if (e.find(fx) != std::string::npos) return Surface::Skip;
    if (e.find("window") != std::string::npos || e.find("glass") != std::string::npos)
        return Surface::Glass;
    if (e.find("selflight") != std::string::npos) return Surface::Emissive;
    if (e.find("transparent") != std::string::npos || e.find("aref") != std::string::npos ||
        e.find("trans") != std::string::npos)
        return Surface::Cutout;
    return Surface::Opaque;
}

const char* surfaceSuffix(Surface s) {
    switch (s) {
        case Surface::Cutout:   return "_cut";
        case Surface::Decal:    return "_decal";
        case Surface::Glass:    return "_glass";
        case Surface::Water:    return "_water";
        case Surface::Emissive: return "_glow";
        default:                return "";
    }
}

// Whether a surface keeps the map's alpha. An opaque X-Ray map's alpha is not
// opacity (it carries gloss or nothing), and fitzel would read holes into it.
bool keepsAlpha(Surface s) {
    return s == Surface::Cutout || s == Surface::Decal || s == Surface::Glass;
}
// Drawn blended -- set in the scene, glTF's BLEND is not read.
bool blends(Surface s) { return s == Surface::Decal || s == Surface::Glass || s == Surface::Water; }

// "mtl\mtl_bochka_01" -> "mtl_bochka_01"; the folder is only kept when the
// leaf name alone is taken by another folder's map.
std::string leafName(const std::string& tex) {
    const std::size_t s = tex.find_last_of("\\/");
    return lower(s == std::string::npos ? tex : tex.substr(s + 1));
}
std::string flatName(const std::string& tex) {
    std::string s = lower(tex);
    for (char& c : s)
        if (c == '\\' || c == '/' || c == ' ' || c == '#') c = '_';
    return s;
}

struct ImageJob {
    std::string texture;   // the game's name, no extension
    bool        alpha = false;
    std::string file;      // under the texture folder
    bool        ok = false;
    std::string error;
};

struct Material {
    std::string name;
    Surface     surface = Surface::Opaque;
    int         image = -1;   // into the jobs
};

// --- texture conversion ---------------------------------------------------------

void halve(dds::Image& img) {
    const int w = std::max(1, img.width / 2), h = std::max(1, img.height / 2);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(img.width - 1, 2 * x + dx);
                        const int sy = std::min(img.height - 1, 2 * y + dy);
                        sum += img.rgba[(static_cast<std::size_t>(sy) * img.width + sx) * 4 + c];
                    }
                out[(static_cast<std::size_t>(y) * w + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
            }
    img.width = w;
    img.height = h;
    img.rgba = std::move(out);
}

// Where the game keeps a level's map: its own folder first (terrain, lightmaps),
// then the shared textures/.
bool loadTexture(const xray::FileSystem& fs, const std::string& level, const std::string& tex,
                 dds::Image& img, std::string& error) {
    std::vector<std::uint8_t> bytes;
    const std::string candidates[2] = {"levels/" + level + "/" + tex + ".dds",
                                       "textures/" + tex + ".dds"};
    for (const std::string& path : candidates) {
        if (!fs.read(path, bytes)) continue;
        if (dds::decode(bytes.data(), bytes.size(), img, error)) return true;
        error = path + ": " + error;
        return false;
    }
    error = "no " + candidates[1];
    return false;
}

void convertImage(const xray::FileSystem& fs, const std::string& level, const fs::path& dir,
                  int maxSize, ImageJob& job) {
    job.ok = convertTexture(fs, level, job.texture, dir / job.file, job.alpha, maxSize, job.error);
}

struct Cell {
    int ix = 0, iz = 0;
    std::map<int, std::vector<int>> byMaterial;   // material -> meshes
    // Baked terrain materials: their UVs are the ground position in the map's
    // square (lo, size), seen from above.
    std::map<int, std::pair<glm::vec2, glm::vec2>> planar;
};

json v3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

struct NodeOut {
    std::string name;
    glm::vec3 lo{1e30f}, hi{-1e30f};
    std::vector<int> materials;   // per primitive, in order
};

// What goes into one node: meshes by material; a material in `planar` takes
// its UVs from the ground position (lo, size) instead of the mesh's own.
struct NodeIn {
    std::string name;
    std::map<int, std::vector<const xray::LevelMesh*>> byMaterial;
    std::map<int, std::pair<glm::vec2, glm::vec2>> planar;
};

json materialJson(const Material& m, int tex) {
    json mat = {{"name", m.name}};
    json pbr = {{"metallicFactor", 0.0}, {"roughnessFactor", 0.85}};
    if (tex >= 0) pbr["baseColorTexture"] = {{"index", tex}};
    switch (m.surface) {
        case Surface::Cutout:
        case Surface::Decal:
            mat["alphaMode"] = "MASK";
            mat["alphaCutoff"] = 0.5;
            mat["doubleSided"] = m.surface == Surface::Cutout;
            break;
        case Surface::Water:
            // MASK only so it does not collide; the scene blends it.
            mat["alphaMode"] = "MASK";
            pbr["roughnessFactor"] = 0.1;
            break;
        case Surface::Glass:
            // OPAQUE so a window still stops you; the scene blends it.
            mat["doubleSided"] = true;
            pbr["roughnessFactor"] = 0.15;
            break;
        case Surface::Emissive:
            mat["emissiveFactor"] = json::array({1.0, 1.0, 1.0});
            if (tex >= 0) mat["emissiveTexture"] = {{"index", tex}};
            break;
        default: break;
    }
    mat["pbrMetallicRoughness"] = pbr;
    return mat;
}

// One GLB of `nodes`, with only the materials and images they use. The images
// stay files of their own in `texFolder` beside it.
bool writeModel(const fs::path& path, const std::string& texFolder, const std::vector<NodeIn>& nodes,
                const std::vector<Material>& materials, const std::vector<ImageJob>& jobs,
                std::vector<NodeOut>& out, Report& report, std::size_t& bytes) {
    Glb glb;
    json& g = glb.gltf;
    g["samplers"] = json::array({{{"magFilter", 9729}, {"minFilter", 9987},
                                  {"wrapS", 10497}, {"wrapT", 10497}}});
    g["images"] = json::array();
    g["textures"] = json::array();
    g["materials"] = json::array();
    g["meshes"] = json::array();
    g["nodes"] = json::array();
    std::map<int, int> texOf, matOf;   // job -> texture, material -> index here
    auto materialIndex = [&](int mat) {
        if (auto it = matOf.find(mat); it != matOf.end()) return it->second;
        const Material& m = materials[mat];
        int tex = -1;
        if (m.image >= 0 && jobs[m.image].ok) {
            if (auto it = texOf.find(m.image); it != texOf.end()) {
                tex = it->second;
            } else {
                const ImageJob& j = jobs[m.image];
                g["images"].push_back({{"uri", texFolder + "/" + j.file},
                                       {"mimeType", j.alpha ? "image/png" : "image/jpeg"}});
                g["textures"].push_back({{"sampler", 0}, {"source", g["images"].size() - 1}});
                tex = texOf[m.image] = static_cast<int>(g["textures"].size()) - 1;
            }
        }
        g["materials"].push_back(materialJson(m, tex));
        return matOf[mat] = static_cast<int>(g["materials"].size()) - 1;
    };
    out.clear();
    for (const NodeIn& in : nodes) {
        NodeOut node;
        node.name = in.name;
        json prims = json::array();
        for (const auto& [mat, meshes] : in.byMaterial) {
            std::vector<float> pos, nrm, uv;
            std::vector<std::uint32_t> idx;
            glm::vec3 lo(1e30f), hi(-1e30f);
            const auto planar = in.planar.find(mat);
            for (const xray::LevelMesh* m : meshes) {
                const std::uint32_t base = static_cast<std::uint32_t>(pos.size() / 3);
                for (const xray::LevelVertex& v : m->verts) {
                    pos.insert(pos.end(), {v.pos.x, v.pos.y, v.pos.z});
                    nrm.insert(nrm.end(), {v.normal.x, v.normal.y, v.normal.z});
                    if (planar != in.planar.end()) {
                        const glm::vec2 t = (glm::vec2(v.pos.x, v.pos.z) - planar->second.first) /
                                            planar->second.second;
                        uv.insert(uv.end(), {t.x, t.y});
                    } else {
                        uv.insert(uv.end(), {v.uv.x, v.uv.y});
                    }
                    lo = glm::min(lo, v.pos);
                    hi = glm::max(hi, v.pos);
                }
                for (std::uint32_t i : m->indices) idx.push_back(base + i);
            }
            if (idx.empty()) continue;
            const std::size_t nv = pos.size() / 3;
            const int aPos = glb.accessor(glb.view(pos.data(), pos.size() * 4, 34962), 5126, nv,
                                          "VEC3", v3(lo), v3(hi));
            const int aNrm = glb.accessor(glb.view(nrm.data(), nrm.size() * 4, 34962), 5126, nv, "VEC3");
            const int aUv = glb.accessor(glb.view(uv.data(), uv.size() * 4, 34962), 5126, nv, "VEC2");
            const int aIdx = glb.accessor(glb.view(idx.data(), idx.size() * 4, 34963), 5125,
                                          idx.size(), "SCALAR");
            prims.push_back({{"attributes", {{"POSITION", aPos}, {"NORMAL", aNrm}, {"TEXCOORD_0", aUv}}},
                             {"indices", aIdx}, {"material", materialIndex(mat)}, {"mode", 4}});
            node.materials.push_back(mat);
            node.lo = glm::min(node.lo, lo);
            node.hi = glm::max(node.hi, hi);
            report.triangles += idx.size() / 3;
        }
        if (prims.empty()) continue;
        report.primitives += static_cast<int>(prims.size());
        g["meshes"].push_back({{"name", node.name}, {"primitives", prims}});
        g["nodes"].push_back({{"name", node.name}, {"mesh", g["meshes"].size() - 1}});
        out.push_back(std::move(node));
    }
    json sceneNodes = json::array();
    for (std::size_t i = 0; i < out.size(); ++i) sceneNodes.push_back(i);
    g["scenes"] = json::array({{{"nodes", sceneNodes}}});
    g["scene"] = 0;
    bytes = glb.bin.size();
    return glb.write(path);
}


// Spot direction -> the entity's Euler degrees: a spot shines down local +Z,
// turned about X first, then Y (scenegraph::compose's order, world axes).
glm::vec3 eulerForDirection(glm::vec3 d) {
    const float len = glm::length(d);
    if (len < 1e-6f) return glm::vec3(90.0f, 0.0f, 0.0f);
    d /= len;
    const float pitch = std::asin(glm::clamp(-d.y, -1.0f, 1.0f));
    const float yaw = std::atan2(d.x, d.z);
    return glm::degrees(glm::vec3(pitch, yaw, 0.0f));
}


// --- the terrain -----------------------------------------------------------------
//
// X-Ray draws its terrain from one colour map stretched over the whole level
// (2048 texels across a kilometre and a half) and gets its close-up look from
// four detail maps, mixed by the RGBA of a mask map and multiplied in twice
// over (R2's deffer_impl_flat_dt: base * sum(mask_i * detail_i) * 2). fitzel's
// materials have one colour map, so that sum is baked: each map cell's terrain
// gets a map of its own, `density` texels per metre, laid on from above.

// "levels\escape_grass" -> its four detail maps (shaders.xr, the R2-R..R2-A
// texture properties of each blender).
std::map<std::string, std::array<std::string, 4>> terrainBlenders(const xray::FileSystem& fs) {
    std::map<std::string, std::array<std::string, 4>> out;
    std::vector<std::uint8_t> d;
    if (!fs.read("shaders.xr", d)) return out;
    auto chunks = [](const std::uint8_t* p, std::size_t n, auto&& fn) {
        std::size_t at = 0;
        while (at + 8 <= n) {
            std::uint32_t id, size;
            std::memcpy(&id, p + at, 4);
            std::memcpy(&size, p + at + 4, 4);
            if (size > n - at - 8) break;
            fn(id, p + at + 8, static_cast<std::size_t>(size));
            at += 8 + size;
        }
    };
    chunks(d.data(), d.size(), [&](std::uint32_t id, const std::uint8_t* p, std::size_t n) {
        if (id != 2) return;
        chunks(p, n, [&](std::uint32_t, const std::uint8_t* b, std::size_t len) {
            if (len < 136) return;
            const std::string name = lower(std::string(reinterpret_cast<const char*>(b + 8),
                                                       strnlen(reinterpret_cast<const char*>(b + 8), 128)));
            if (!startsWith(name, "levels\\")) return;
            std::array<std::string, 4> det;
            const char* keys[4] = {"R2-R", "R2-G", "R2-B", "R2-A"};
            const std::string body(reinterpret_cast<const char*>(b), len);
            for (int c = 0; c < 4; ++c) {
                const std::size_t at = body.find(std::string(keys[c]) + '\0');
                if (at == std::string::npos) continue;
                const std::size_t from = at + 5;
                const std::size_t end = body.find('\0', from);
                if (end != std::string::npos) det[c] = lower(body.substr(from, end - from));
            }
            out[name] = det;
        });
    });
    return out;
}

// textures.ltx [association]: "terrain\terrain_escape = detail\detail_grnd_grass,
// 144.000000, usage[diffuse]" -> the detail map and how often it repeats.
std::map<std::string, std::pair<std::string, float>> textureAssociations(const xray::FileSystem& fs) {
    std::map<std::string, std::pair<std::string, float>> out;
    std::vector<std::uint8_t> d;
    if (!fs.read("textures/textures.ltx", d)) return out;
    const std::string text(d.begin(), d.end());
    bool inSection = false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line = line.substr(first);
        if (line[0] == '[') { inSection = lower(line) == "[association]"; continue; }
        if (!inSection || line[0] == ';') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(line.substr(0, eq));
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        const std::string val = line.substr(eq + 1);
        const std::size_t comma = val.find(',');
        if (comma == std::string::npos) continue;
        std::string det = lower(val.substr(0, comma));
        det.erase(0, det.find_first_not_of(" \t"));
        out[key] = {det, static_cast<float>(std::atof(val.c_str() + comma + 1))};
    }
    return out;
}

struct Rgb { float r, g, b; };

// Bilinear, texel centres at +0.5; `wrap` repeats, else the edge is clamped.
Rgb sample(const dds::Image& img, float u, float v, bool wrap, float* alpha = nullptr) {
    const float x = u * img.width - 0.5f, y = v * img.height - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    auto at = [&](int xi, int yi) {
        if (wrap) {
            xi %= img.width;  if (xi < 0) xi += img.width;
            yi %= img.height; if (yi < 0) yi += img.height;
        } else {
            xi = std::clamp(xi, 0, img.width - 1);
            yi = std::clamp(yi, 0, img.height - 1);
        }
        return &img.rgba[(static_cast<std::size_t>(yi) * img.width + xi) * 4];
    };
    const std::uint8_t* p[4] = {at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1)};
    const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    float c[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) c[k] += w[i] * p[i][k];
    if (alpha) *alpha = c[3] / 255.0f;
    return {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f};
}

// One X-Ray terrain shader, ready to sample.
struct TerrainSet {
    const dds::Image* base = nullptr;
    const dds::Image* mask = nullptr;
    const dds::Image* detail[4] = {};
    float scale = 1.0f;   // detail repeats per base-map unit
    Rgb at(glm::vec2 uv) const {
        const Rgb b = sample(*base, uv.x, uv.y, false);
        float m[4] = {0.25f, 0.25f, 0.25f, 0.25f};
        if (mask) {
            float a = 0.0f;
            const Rgb mm = sample(*mask, uv.x, uv.y, false, &a);
            m[0] = mm.r; m[1] = mm.g; m[2] = mm.b; m[3] = a;
        }
        const float sum = m[0] + m[1] + m[2] + m[3];
        if (sum < 1e-4f) { m[0] = m[1] = m[2] = m[3] = 0.25f; }
        else for (float& x : m) x /= sum;
        Rgb dt{0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            if (!detail[i] || m[i] <= 0.0f) continue;
            const Rgb d = sample(*detail[i], uv.x * scale, uv.y * scale, true);
            dt.r += m[i] * d.r; dt.g += m[i] * d.g; dt.b += m[i] * d.b;
        }
        return {std::min(1.0f, 2.0f * b.r * dt.r), std::min(1.0f, 2.0f * b.g * dt.g),
                std::min(1.0f, 2.0f * b.b * dt.b)};
    }
};

struct TerrainBake {
    glm::vec2 lo{0.0f}, size{1.0f};   // the map's square on the ground (x, z)
    std::string file;
    bool ok = false;
};

// One cell's terrain, rasterised from above at twice the resolution and
// averaged down; texels no triangle covers take their neighbours' colour, so
// the map's own mip chain does not bleed the void into the edges.
void bakeTerrainCell(const xray::Level& level, const std::vector<int>& meshes,
                     const std::vector<const TerrainSet*>& setOf, float density, int maxSize,
                     const fs::path& path, TerrainBake& out) {
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (int mi : meshes)
        for (const xray::LevelVertex& v : level.meshes[mi].verts) {
            lo = glm::min(lo, glm::vec2(v.pos.x, v.pos.z));
            hi = glm::max(hi, glm::vec2(v.pos.x, v.pos.z));
        }
    const glm::vec2 size = glm::max(hi - lo, glm::vec2(0.01f));
    const int w = std::clamp(static_cast<int>(std::ceil(size.x * density)), 8, maxSize);
    const int h = std::clamp(static_cast<int>(std::ceil(size.y * density)), 8, maxSize);
    const int sw = w * 2, sh = h * 2;
    std::vector<glm::vec2> uv(static_cast<std::size_t>(sw) * sh);
    std::vector<float> top(uv.size(), -1e30f);
    std::vector<const TerrainSet*> set(uv.size(), nullptr);
    for (int mi : meshes) {
        const xray::LevelMesh& m = level.meshes[mi];
        const TerrainSet* ts = setOf[m.shader];
        for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const xray::LevelVertex* v[3] = {&m.verts[m.indices[t]], &m.verts[m.indices[t + 1]],
                                             &m.verts[m.indices[t + 2]]};
            glm::vec2 p[3];
            for (int k = 0; k < 3; ++k)
                p[k] = (glm::vec2(v[k]->pos.x, v[k]->pos.z) - lo) / size * glm::vec2(sw, sh);
            const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
            if (std::abs(area) < 1e-8f) continue;   // seen edge-on from above
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
            const int x1 = std::min(sw - 1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
            const int y1 = std::min(sh - 1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const glm::vec2 q(x + 0.5f, y + 0.5f);
                    const float b0 = ((p[1].x - q.x) * (p[2].y - q.y) - (p[2].x - q.x) * (p[1].y - q.y)) / area;
                    const float b1 = ((p[2].x - q.x) * (p[0].y - q.y) - (p[0].x - q.x) * (p[2].y - q.y)) / area;
                    const float b2 = 1.0f - b0 - b1;
                    if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f) continue;
                    const std::size_t i = static_cast<std::size_t>(y) * sw + x;
                    // Where terrain folds over itself, the upper surface wins.
                    const float y3 = b0 * v[0]->pos.y + b1 * v[1]->pos.y + b2 * v[2]->pos.y;
                    if (y3 < top[i]) continue;
                    top[i] = y3;
                    uv[i] = b0 * v[0]->uv + b1 * v[1]->uv + b2 * v[2]->uv;
                    set[i] = ts;
                }
        }
    }
    std::vector<float> rgb(static_cast<std::size_t>(w) * h * 3, 0.0f);
    std::vector<std::uint8_t> have(static_cast<std::size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Rgb acc{0, 0, 0};
            int n = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const std::size_t i = static_cast<std::size_t>(2 * y + dy) * sw + 2 * x + dx;
                    if (!set[i]) continue;
                    const Rgb c = set[i]->at(uv[i]);
                    acc.r += c.r; acc.g += c.g; acc.b += c.b;
                    ++n;
                }
            if (!n) continue;
            const std::size_t o = static_cast<std::size_t>(y) * w + x;
            rgb[o * 3] = acc.r / n; rgb[o * 3 + 1] = acc.g / n; rgb[o * 3 + 2] = acc.b / n;
            have[o] = 1;
        }
    for (int pass = 0; pass < 16; ++pass) {
        bool grew = false;
        std::vector<std::uint8_t> next = have;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t o = static_cast<std::size_t>(y) * w + x;
                if (have[o]) continue;
                float acc[3] = {0, 0, 0};
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        const std::size_t q = static_cast<std::size_t>(yy) * w + xx;
                        if (!have[q]) continue;
                        for (int k = 0; k < 3; ++k) acc[k] += rgb[q * 3 + k];
                        ++n;
                    }
                if (!n) continue;
                for (int k = 0; k < 3; ++k) rgb[o * 3 + k] = acc[k] / n;
                next[o] = 1;
                grew = true;
            }
        have.swap(next);
        if (!grew) break;
    }
    std::vector<std::uint8_t> px(rgb.size());
    for (std::size_t i = 0; i < rgb.size(); ++i)
        px[i] = static_cast<std::uint8_t>(std::lround(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f));
    out.lo = lo;
    out.size = size;
    out.ok = stbi_write_jpg(path.string().c_str(), w, h, 3, px.data(), 92) != 0;
}

} // namespace

namespace detail {

std::string modelGuid(const fs::path& glb) {
    const fs::path meta = glb.string() + ".meta";
    std::ifstream in(meta);
    if (in) {
        try {
            json j = json::parse(in);
            const std::string g = j.value("guid", std::string());
            if (fitzel::AssetId::fromString(g).valid()) return g;
        } catch (...) {}
    }
    return fitzel::AssetId::generate().toString();
}

bool convertTexture(const xray::FileSystem& fs, const std::string& level, const std::string& tex,
                    const fs::path& file, bool alpha, int maxSize, std::string& error) {
    dds::Image img;
    if (!loadTexture(fs, level, tex, img, error)) return false;
    while (std::max(img.width, img.height) > maxSize) halve(img);
    const std::string path = file.string();
    bool ok;
    if (alpha) {
        ok = stbi_write_png(path.c_str(), img.width, img.height, 4, img.rgba.data(), img.width * 4) != 0;
    } else {
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(img.width) * img.height * 3);
        for (std::size_t i = 0, n = rgb.size() / 3; i < n; ++i)
            std::memcpy(&rgb[i * 3], &img.rgba[i * 4], 3);
        ok = stbi_write_jpg(path.c_str(), img.width, img.height, 3, rgb.data(), 92) != 0;
    }
    if (!ok) error = "cannot write " + path;
    return ok;
}

} // namespace detail

bool importLevel(const xray::FileSystem& gameFs, const Options& o, Report& report,
                 std::string& error, const Log& log) {
    report = Report{};
    auto say = [&](const std::string& s) { if (log) log(s); };
    char buf[512];

    xray::Level level;
    if (!xray::readLevel(gameFs, o.level, level, error)) return false;
    std::size_t srcTris = 0;
    for (const auto& m : level.meshes) srcTris += m.indices.size() / 3;
    std::snprintf(buf, sizeof(buf), "%s: format %d, %zu shaders, %zu meshes (%zu triangles), %zu lights",
                  o.level.c_str(), level.version, level.shaders.size(), level.meshes.size(), srcTris,
                  level.lights.size());
    say(buf);
    if (level.skippedVisuals)
        report.warnings.push_back(std::to_string(level.skippedVisuals) +
                                  " visuals point outside the level's buffers and were skipped");

    // A scene without fitzel's terrain still plays on a ground: a flat, unseen
    // collider at y = 0 (heightAt answers 0 when there is no terrain). X-Ray
    // levels reach tens of metres below it -- Escape's bunker would be under
    // the floor -- so the whole level is lifted clear of it.
    if (o.liftAboveGround) {
        float minY = 1e30f;
        for (const xray::LevelMesh& m : level.meshes) minY = std::min(minY, m.lo.y);
        if (minY < 1.0f && minY > -1e29f) report.lift = std::ceil(1.0f - minY);
    }
    if (report.lift != 0.0f) {
        const glm::vec3 up(0.0f, report.lift, 0.0f);
        for (xray::LevelMesh& m : level.meshes) {
            for (xray::LevelVertex& v : m.verts) v.pos += up;
            m.lo += up;
            m.hi += up;
        }
        for (xray::LevelLight& l : level.lights) l.position += up;
        for (xray::LevelGlow& gl : level.glows) gl.position += up;
        for (xray::LevelSpawn& sp : level.spawns) sp.position += up;
        for (xray::PatrolPath& p : level.paths)
            for (xray::PatrolPoint& pt : p.points) pt.position += up;
        for (xray::StaticSound& ss : level.sounds) ss.position += up;
        std::snprintf(buf, sizeof(buf), "lifted %.0f m clear of fitzel's ground (game y = scene y - %.0f)",
                      report.lift, report.lift);
        say(buf);
    }

    // Folders.
    std::error_code ec;
    const fs::path modelDir = o.project / "models";
    const fs::path texDir = modelDir / o.level;
    fs::create_directories(texDir, ec);
    if (ec) { error = "cannot create " + texDir.string() + ": " + ec.message(); return false; }

    // Materials and the images they need.
    std::vector<ImageJob> jobs;
    std::map<std::pair<std::string, bool>, int> jobOf;
    std::unordered_map<std::string, std::string> leafOwner;   // file leaf -> texture
    std::vector<Material> materials;
    std::map<std::pair<std::string, Surface>, int> materialOf;
    std::vector<int> shaderMaterial(level.shaders.size(), -1);
    auto materialFor = [&](const std::string& texture, Surface surf) -> int {
        const std::string tex = lower(texture);
        const auto mk = std::make_pair(tex, surf);
        if (auto it = materialOf.find(mk); it != materialOf.end()) return it->second;
        const bool alpha = keepsAlpha(surf);
        const auto jk = std::make_pair(tex, alpha);
        int ji;
        if (auto it = jobOf.find(jk); it != jobOf.end()) {
            ji = it->second;
        } else {
            std::string leaf = leafName(tex);
            auto [own, fresh] = leafOwner.emplace(leaf, tex);
            if (!fresh && own->second != tex) leaf = flatName(tex);
            ImageJob job;
            job.texture = tex;
            job.alpha = alpha;
            job.file = leaf + (alpha ? ".png" : ".jpg");
            ji = static_cast<int>(jobs.size());
            jobs.push_back(job);
            jobOf[jk] = ji;
        }
        Material m;
        m.name = leafName(tex) + surfaceSuffix(surf);
        m.surface = surf;
        m.image = ji;
        // Two materials of one name would merge in fitzel's library.
        for (const Material& other : materials)
            if (other.name == m.name) { m.name = flatName(tex) + surfaceSuffix(surf); break; }
        const int index = static_cast<int>(materials.size());
        materialOf[mk] = index;
        materials.push_back(m);
        return index;
    };
    for (std::size_t s = 0; s < level.shaders.size(); ++s) {
        const Surface surf = classify(level.shaders[s]);
        if (surf != Surface::Skip) shaderMaterial[s] = materialFor(level.shaders[s].texture, surf);
    }

    // The props: what level.spawn puts down with a model of its own -- crates,
    // barrels, lamps, a helicopter wreck -- each where the spawn has it, in a
    // model of their own. Creatures, weapons and pickups are the game's to
    // bring; only the physics props and the level's own breakables come.
    struct Prop {
        std::string name;
        std::vector<xray::LevelMesh> meshes;
        std::vector<int> materials;
    };
    std::vector<Prop> props;
    if (o.props) {
        std::map<std::string, std::vector<xray::ModelPart>> models;
        std::set<std::string> missing;
        std::map<std::string, int> names;
        const glm::vec3 up(0.0f, report.lift, 0.0f);
        for (const xray::LevelSpawn& sp : level.spawns) {
            // A helicopter is a scripted flyer, hung in the air where it starts.
            if (sp.section == "helicopter") continue;
            std::string vis = lower(sp.visual);
            for (char& c : vis) if (c == '/') c = '\\';
            if (vis.size() > 4 && vis.compare(vis.size() - 4, 4, ".ogf") == 0) vis.resize(vis.size() - 4);
            const bool local = startsWith(vis, "meshes\\");
            if (!local && !startsWith(vis, "physics\\") && !startsWith(vis, "dynamics\\")) continue;
            const std::string path = local ? "levels/" + o.level + "/" + vis + ".ogf" : "meshes/" + vis + ".ogf";
            auto it = models.find(path);
            if (it == models.end()) {
                std::vector<xray::ModelPart> parts;
                std::string e;
                if (!xray::readModel(gameFs, path, parts, e) && missing.insert(path).second)
                    report.warnings.push_back("prop " + e);
                it = models.emplace(path, std::move(parts)).first;
            }
            if (it->second.empty()) continue;
            const glm::mat3 rot = xray::objectRotation(sp.angle);
            Prop prop;
            std::string base = sp.name.empty() ? sp.section : sp.name;
            for (char& c : base)
                if (c == '\\' || c == '/' || c == ' ' || c == '#' || c == '.') c = '_';
            const int n = names[base]++;
            prop.name = n ? base + "_" + std::to_string(n) : base;
            for (const xray::ModelPart& part : it->second) {
                const Surface surf = classifyModel(part.shader);
                if (surf == Surface::Skip || part.texture.empty()) continue;
                xray::LevelMesh m;
                m.verts.reserve(part.verts.size());
                for (xray::LevelVertex v : part.verts) {
                    const glm::vec3 wp = rot * v.pos + sp.gamePosition;
                    glm::vec3 wn = rot * v.normal;
                    const float len = glm::length(wn);
                    wn = len > 1e-6f ? wn / len : glm::vec3(0.0f, 1.0f, 0.0f);
                    v.pos = glm::vec3(wp.x, wp.y, -wp.z) + up;
                    v.normal = glm::vec3(wn.x, wn.y, -wn.z);
                    m.lo = glm::min(m.lo, v.pos);
                    m.hi = glm::max(m.hi, v.pos);
                    m.verts.push_back(v);
                }
                for (std::size_t t = 0; t + 2 < part.indices.size(); t += 3)
                    m.indices.insert(m.indices.end(),
                                     {part.indices[t], part.indices[t + 2], part.indices[t + 1]});
                prop.meshes.push_back(std::move(m));
                prop.materials.push_back(materialFor(part.texture, surf));
            }
            if (!prop.meshes.empty()) props.push_back(std::move(prop));
        }
        say(std::to_string(props.size()) + " props from level.spawn (" + std::to_string(models.size()) +
            " models)");
    }

    // The textures, on every core.
    {
        say("converting " + std::to_string(jobs.size()) + " textures");
        std::atomic<std::size_t> next{0};
        unsigned n = o.threads ? o.threads : std::max(1u, std::thread::hardware_concurrency());
        n = std::min<unsigned>(n, static_cast<unsigned>(std::max<std::size_t>(1, jobs.size())));
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < n; ++t)
            pool.emplace_back([&] {
                for (std::size_t i; (i = next++) < jobs.size();)
                    convertImage(gameFs, o.level, texDir, o.maxTexture, jobs[i]);
            });
        for (std::thread& t : pool) t.join();
        for (const ImageJob& j : jobs) {
            if (j.ok) ++report.textures;
            else report.warnings.push_back(j.texture + ": " + j.error);
        }
    }

    // Meshes into cells by their middle.
    std::map<std::pair<int, int>, Cell> cells;
    for (std::size_t mi = 0; mi < level.meshes.size(); ++mi) {
        const xray::LevelMesh& m = level.meshes[mi];
        const int mat = shaderMaterial[m.shader];
        if (mat < 0) continue;
        const glm::vec3 c = 0.5f * (m.lo + m.hi);
        const int ix = static_cast<int>(std::floor(c.x / o.cell));
        const int iz = static_cast<int>(std::floor(c.z / o.cell));
        Cell& cell = cells[{ix, iz}];
        cell.ix = ix;
        cell.iz = iz;
        cell.byMaterial[mat].push_back(static_cast<int>(mi));
    }

    // The terrain, baked per cell (bakeTerrainCell).
    bool anyTerrain = false;
    for (const Material& m : materials) anyTerrain |= m.surface == Surface::Terrain;
    if (anyTerrain && o.terrainDensity > 0.0f) {
        const auto blenders = terrainBlenders(gameFs);
        const auto assoc = textureAssociations(gameFs);
        // How many base-map units a metre is: the terrain's UVs over its extent.
        glm::vec2 uvLo(1e30f), uvHi(-1e30f), xzLo(1e30f), xzHi(-1e30f);
        for (const xray::LevelMesh& m : level.meshes) {
            const int mat = shaderMaterial[m.shader];
            if (mat < 0 || materials[mat].surface != Surface::Terrain) continue;
            for (const xray::LevelVertex& v : m.verts) {
                uvLo = glm::min(uvLo, v.uv); uvHi = glm::max(uvHi, v.uv);
                xzLo = glm::min(xzLo, glm::vec2(v.pos.x, v.pos.z));
                xzHi = glm::max(xzHi, glm::vec2(v.pos.x, v.pos.z));
            }
        }
        const glm::vec2 perMetre = (uvHi - uvLo) / glm::max(xzHi - xzLo, glm::vec2(1.0f));
        const float uvPerMetre = std::max(perMetre.x, perMetre.y);

        std::map<std::string, std::unique_ptr<dds::Image>> images;
        auto image = [&](const std::string& tex, float halveAbove) -> const dds::Image* {
            const std::string key = tex + "@" + std::to_string(halveAbove);
            if (auto it = images.find(key); it != images.end()) return it->second.get();
            auto img = std::make_unique<dds::Image>();
            std::string e;
            if (!loadTexture(gameFs, o.level, tex, *img, e)) {
                report.warnings.push_back("terrain: " + e);
                img.reset();
            } else {
                // A detail map finer than the bake samples it would only alias.
                while (halveAbove > 0.0f && img->width > 4 && img->width * halveAbove > 2.0f * 2.0f * o.terrainDensity)
                    halve(*img);
            }
            return (images[key] = std::move(img)).get();
        };
        std::vector<std::unique_ptr<TerrainSet>> sets;
        std::vector<const TerrainSet*> setOf(level.shaders.size(), nullptr);
        for (std::size_t si = 0; si < level.shaders.size(); ++si) {
            const int mat = shaderMaterial[si];
            if (mat < 0 || materials[mat].surface != Surface::Terrain) continue;
            const std::string base = lower(level.shaders[si].texture);
            const auto as = assoc.find(base);
            std::array<std::string, 4> det;
            if (auto b = blenders.find(lower(level.shaders[si].engine)); b != blenders.end()) det = b->second;
            for (std::string& d : det)
                if (d.empty() && as != assoc.end()) d = as->second.first;
            auto set = std::make_unique<TerrainSet>();
            set->scale = as != assoc.end() && as->second.second > 0.0f ? as->second.second : 1.0f;
            set->base = image(base, 0.0f);
            const std::string maskName = base + "_mask";
            if (gameFs.exists("textures/" + maskName + ".dds") ||
                gameFs.exists("levels/" + o.level + "/" + maskName + ".dds"))
                set->mask = image(maskName, 0.0f);
            int details = 0;
            for (int c = 0; c < 4; ++c)
                if (!det[c].empty() && (set->detail[c] = image(det[c], uvPerMetre * set->scale))) ++details;
            if (!set->base || !details) continue;   // keeps the stretched colour map
            setOf[si] = set.get();
            sets.push_back(std::move(set));
        }

        struct Job { Cell* cell; std::vector<int> meshes; TerrainBake bake; };
        std::vector<Job> bakeJobs;
        for (auto& [key, cell] : cells) {
            Job j{&cell, {}, {}};
            for (const auto& [mat, meshes] : cell.byMaterial)
                for (int mi : meshes)
                    if (setOf[level.meshes[mi].shader]) j.meshes.push_back(mi);
            if (j.meshes.empty()) continue;
            std::snprintf(buf, sizeof(buf), "terrain_%+03d_%+03d.jpg", cell.ix, cell.iz);
            j.bake.file = buf;
            bakeJobs.push_back(std::move(j));
        }
        std::atomic<std::size_t> next{0};
        const unsigned n = o.threads ? o.threads : std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < n; ++t)
            pool.emplace_back([&] {
                for (std::size_t i; (i = next++) < bakeJobs.size();)
                    bakeTerrainCell(level, bakeJobs[i].meshes, setOf, o.terrainDensity, o.maxTexture,
                                    texDir / bakeJobs[i].bake.file, bakeJobs[i].bake);
            });
        for (std::thread& t : pool) t.join();

        int baked = 0;
        for (Job& j : bakeJobs) {
            if (!j.bake.ok) { report.warnings.push_back("cannot write " + j.bake.file); continue; }
            ImageJob img;
            img.texture = "(terrain bake)";
            img.file = j.bake.file;
            img.ok = true;
            jobs.push_back(img);
            Material m;
            m.name = o.level + "_" + j.bake.file.substr(0, j.bake.file.size() - 4);
            m.surface = Surface::Terrain;
            m.image = static_cast<int>(jobs.size()) - 1;
            const int mat = static_cast<int>(materials.size());
            materials.push_back(m);
            // The cell's terrain leaves its shaders' materials for the bake.
            const std::set<int> moved(j.meshes.begin(), j.meshes.end());
            for (auto it = j.cell->byMaterial.begin(); it != j.cell->byMaterial.end();) {
                auto& list = it->second;
                list.erase(std::remove_if(list.begin(), list.end(), [&](int mi) { return moved.count(mi) != 0; }),
                           list.end());
                it = list.empty() ? j.cell->byMaterial.erase(it) : std::next(it);
            }
            j.cell->byMaterial[mat] = j.meshes;
            j.cell->planar[mat] = {j.bake.lo, j.bake.size};
            ++baked;
            ++report.textures;
        }
        std::snprintf(buf, sizeof(buf), "baked the terrain of %d cells at %.1f texels per metre", baked,
                      o.terrainDensity);
        say(buf);
    }

    // The models: the level, a node per map cell; the props, a node each.
    std::vector<NodeIn> levelIn;
    for (auto& [key, cell] : cells) {
        NodeIn in;
        std::snprintf(buf, sizeof(buf), "%s_%+03d_%+03d", o.level.c_str(), cell.ix, cell.iz);
        in.name = buf;
        for (const auto& [mat, meshes] : cell.byMaterial)
            for (int mi : meshes) in.byMaterial[mat].push_back(&level.meshes[mi]);
        in.planar = cell.planar;
        levelIn.push_back(std::move(in));
    }
    std::vector<NodeIn> propIn;
    for (const Prop& prop : props) {
        NodeIn in;
        in.name = prop.name;
        for (std::size_t k = 0; k < prop.meshes.size(); ++k)
            in.byMaterial[prop.materials[k]].push_back(&prop.meshes[k]);
        propIn.push_back(std::move(in));
    }

    std::vector<NodeOut> nodes, propNodes;
    std::size_t bytes = 0;
    report.model = modelDir / (o.level + ".glb");
    const std::string guid = modelGuid(report.model);
    if (!writeModel(report.model, o.level, levelIn, materials, jobs, nodes, report, bytes)) {
        error = "cannot write " + report.model.string();
        return false;
    }
    std::ofstream(report.model.string() + ".meta") << json({{"guid", guid}, {"type", "Model"}}).dump(2) << "\n";
    report.nodes = static_cast<int>(nodes.size());
    std::snprintf(buf, sizeof(buf), "wrote %s: %d nodes, %d primitives, %zu triangles, %.1f MB",
                  report.model.filename().string().c_str(), report.nodes, report.primitives,
                  report.triangles, bytes / 1048576.0);
    say(buf);
    std::string propGuid;
    if (!propIn.empty()) {
        report.propModel = modelDir / (o.level + "_props.glb");
        propGuid = modelGuid(report.propModel);
        const int prims = report.primitives;
        const std::size_t tris = report.triangles;
        if (!writeModel(report.propModel, o.level, propIn, materials, jobs, propNodes, report, bytes)) {
            error = "cannot write " + report.propModel.string();
            return false;
        }
        std::ofstream(report.propModel.string() + ".meta")
            << json({{"guid", propGuid}, {"type", "Model"}}).dump(2) << "\n";
        report.props = static_cast<int>(propNodes.size());
        std::snprintf(buf, sizeof(buf), "wrote %s: %d props, %d primitives, %zu triangles, %.1f MB",
                      report.propModel.filename().string().c_str(), report.props,
                      report.primitives - prims, report.triangles - tris, bytes / 1048576.0);
        say(buf);
    }
    {
        std::set<int> used;
        for (const auto* list : {&nodes, &propNodes})
            for (const NodeOut& n : *list) used.insert(n.materials.begin(), n.materials.end());
        report.materials = static_cast<int>(used.size());
    }

    if (!o.writeScene) return true;

    // --- the scene -------------------------------------------------------------
    json ents = json::array();
    int nextId = 0;
    const xray::LevelSpawn* actor = nullptr;
    for (const xray::LevelSpawn& s : level.spawns)
        if (s.section == "actor") { actor = &s; break; }
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const NodeOut& n : nodes) { lo = glm::min(lo, n.lo); hi = glm::max(hi, n.hi); }
    // Only the game's first level spawns the actor. Elsewhere: the AI graph
    // point nearest the middle of the map -- those stand on walkable ground.
    const xray::LevelSpawn* point = nullptr;
    if (!actor) {
        const glm::vec2 mid(0.5f * (lo.x + hi.x), 0.5f * (lo.z + hi.z));
        float best = 1e30f;
        for (const xray::LevelSpawn& s : level.spawns) {
            if (s.section != "graph_point") continue;
            const float d = glm::length(glm::vec2(s.position.x, s.position.z) - mid);
            if (d < best) { best = d; point = &s; }
        }
    }
    const glm::vec3 start = actor ? actor->position : point ? point->position : 0.5f * (lo + hi);
    // No terrain: a level underground, which the sun must not light.
    const bool indoor = !anyTerrain;

    ents.push_back({{"active", true}, {"center", v3({0.0f, 0.0f, 0.0f})},
                    {"components", json::array({{{"type", "sun"}, {"color", v3({1.0f, 0.95f, 0.86f})},
                                                 {"intensity", indoor ? 0.0 : 1.0}}})},
                    {"half", v3(glm::vec3(1.0f))}, {"id", nextId++}, {"name", "Sun"},
                    {"parent", -1}, {"rotation", v3(glm::vec3(0.0f))}, {"type", 5}});

    // The actor's heading: X-Ray turns +Z (north) by angle.y; mirrored, that is
    // 180 degrees less the angle about fitzel's Y.
    const float yaw = actor ? 180.0f - glm::degrees(actor->angle.y) : 0.0f;
    ents.push_back({{"active", true}, {"center", v3(start + glm::vec3(0.0f, 0.95f, 0.0f))},
                    {"components", json::array({{{"type", "player_start"}, {"moveSpeed", 4.0},
                                                 {"runSpeed", 8.0}, {"stepHeight", 0.5},
                                                 {"stepLength", 2.1}, {"headMotion", 1.0},
                                                 {"canSwim", true}}})},
                    {"half", v3({0.3f, 0.9f, 0.3f})}, {"id", nextId++}, {"name", "PlayerStart"},
                    {"parent", -1}, {"rotation", v3({0.0f, yaw, 0.0f})}, {"type", 7}});

    // A model's group: a root whose static Physics makes every part below it
    // collide as its triangles, and an entity per node where the file has it.
    auto addGroup = [&](const std::string& name, const std::string& modelGuidStr,
                        const fs::path& file, const std::vector<NodeOut>& list) {
        const int root = nextId++;
        ents.push_back({{"active", true}, {"center", v3(glm::vec3(0.0f))},
                        {"components", json::array({{{"type", "physics"}, {"dynamic", false}, {"mass", 1.0}}})},
                        {"half", v3(glm::vec3(1.0f))}, {"id", root}, {"name", name},
                        {"parent", -1}, {"rotation", v3(glm::vec3(0.0f))}, {"type", 6}});
        for (std::size_t i = 0; i < list.size(); ++i) {
            const NodeOut& n = list[i];
            const glm::vec3 c = 0.5f * (n.lo + n.hi), h = 0.5f * (n.hi - n.lo);
            ents.push_back({{"active", true}, {"center", v3(c)},
                            {"components", json::array({{{"type", "model"}, {"model", modelGuidStr},
                                                         {"modelFile", file.filename().string()},
                                                         {"node", i}, {"nodeName", n.name}, {"scale", 1.0},
                                                         {"fileCenter", v3(c)}, {"fileHalf", v3(h)}}})},
                            {"half", v3(h)}, {"id", nextId++}, {"name", n.name}, {"parent", root},
                            {"rotation", v3(glm::vec3(0.0f))}, {"type", 6}});
        }
    };
    addGroup(o.level, guid, report.model, nodes);
    if (!propNodes.empty()) addGroup(o.level + "_props", propGuid, report.propModel, propNodes);

    // The level's lights. X-Ray's diffuse is the light's colour and strength
    // in one (often above 1); fitzel keeps them apart.
    if (!level.lights.empty()) {
        const int group = nextId++;
        ents.push_back({{"active", true}, {"center", v3(glm::vec3(0.0f))}, {"components", json::array()},
                        {"half", v3(glm::vec3(0.5f))}, {"id", group}, {"name", "Lights"},
                        {"parent", -1}, {"rotation", v3(glm::vec3(0.0f))}, {"type", 7}});
        int li = 0;
        for (const xray::LevelLight& L : level.lights) {
            if (L.type != 1 && L.type != 2) continue;
            const float peak = std::max({L.color.r, L.color.g, L.color.b, 1e-3f});
            json lc = {{"type", "light"}, {"lightType", L.type == 2 ? 1 : 0},
                       {"color", v3(L.color / peak)}, {"intensity", 4.0f * peak},
                       {"range", std::max(L.range, 0.5f)}, {"castShadows", false},
                       {"bakedOnly", false}, {"shadowBias", 0.003}};
            glm::vec3 rot(0.0f);
            if (L.type == 2) {
                const float outer = glm::degrees(0.5f * L.phi);
                lc["spotAngle"] = glm::clamp(outer, 1.0f, 89.0f);
                lc["spotBlend"] = L.phi > 0.0f ? glm::clamp(1.0f - L.theta / L.phi, 0.0f, 1.0f) : 0.2f;
                rot = eulerForDirection(L.direction);
            }
            ents.push_back({{"active", true}, {"center", v3(L.position)},
                            {"components", json::array({lc})}, {"half", v3(glm::vec3(0.15f))},
                            {"id", nextId++}, {"name", "light_" + std::to_string(li++)},
                            {"parent", group}, {"rotation", v3(rot)}, {"type", 4}});
            ++report.lights;
        }
    }

    // Settings: what a level of a few square kilometres without fitzel's own
    // terrain needs. Everything else stays at the editor's values.
    const glm::vec3 eye = start + glm::vec3(0.0f, 1.7f, 0.0f);
    json settings = {
        {"terrainEntity", true},   // no terrain migrated into the scene
        {"farPlaneAuto", false}, {"farPlane", 2500.0},
        {"waterLevel", -1000.0},
        {"grassEnabled", false}, {"treeEnabled", false}, {"flowerEnabled", false},
        {"birdsEnabled", false}, {"fireflyEnabled", false}, {"wildlife", false},
        {"herdCount", 0}, {"farTerrain", false}, {"ecoForest", false},
        {"volFog", false}, {"timeOfDay", indoor ? 0.5 : 14.5}, {"timeFlows", false},
        {"fogDensity", 0.0012}, {"fogFalloff", 0.004},
        {"moveSpeed", 12.0}, {"cursorX", start.x}, {"cursorY", start.y}, {"cursorZ", start.z},
        {"editorCamera", {{"x", eye.x}, {"y", eye.y}, {"z", eye.z}, {"yaw", 90.0f - yaw}, {"pitch", -5.0}}},
    };

    // Material overrides fitzel cannot take from glTF: blending (decals and
    // water) and no shadows from either. The key names one part using the
    // material; the material is shared, so one entry sets it for all.
    json ov = json::object();
    std::set<int> overridden;
    auto addOverrides = [&](const std::string& modelGuidStr, const std::vector<NodeOut>& list) {
        for (const NodeOut& n : list)
            for (std::size_t p = 0; p < n.materials.size(); ++p) {
                const Material& m = materials[n.materials[p]];
                if (!blends(m.surface) || !overridden.insert(n.materials[p]).second) continue;
                const bool water = m.surface == Surface::Water, glass = m.surface == Surface::Glass;
                json e = {{"name", m.name}, {"alphaMode", 2}, {"castsShadow", false}, {"pbr", true},
                          {"roughness", water ? 0.08 : glass ? 0.15 : 0.9},
                          {"reflectivity", water ? 0.4 : glass ? 0.2 : 0.0}};
                if (water) e["opacity"] = 0.75;
                if (glass) e["doubleSided"] = true;
                ov[modelGuidStr + "|" + n.name + "|" + std::to_string(p)] = e;
            }
    };
    addOverrides(guid, nodes);
    addOverrides(propGuid, propNodes);
    settings["modelMaterialOverrides"] = ov;

    if (o.life) {
        Session own;
        LifeReport life;
        if (!addLife(gameFs, o, level, report.lift, indoor, o.session ? *o.session : own, ents, nextId, settings,
                     report, life, log))
            report.warnings.push_back("the level's life is incomplete");
    }

    json scene = {{"version", 3}, {"app", "stalkerimport"}, {"entities", ents}, {"settings", settings}};
    report.scene = o.project / (o.level + ".fitzel");
    {
        std::ofstream out(report.scene);
        if (!out) { error = "cannot write " + report.scene.string(); return false; }
        out << scene.dump(1) << "\n";
    }
    const fs::path game = o.project / "game.json";
    if (!fs::exists(game, ec)) {
        std::ofstream out(game);
        out << json({{"startScene", o.level}, {"exportScenes", json::array({o.level})},
                     {"scripting", "lua"}, {"productName", o.level}, {"exeName", o.level}})
                   .dump(1)
            << "\n";
    }
    say("wrote " + report.scene.filename().string() + ": " + std::to_string(ents.size()) +
        " entities, " + std::to_string(report.lights) + " lights, " + std::to_string(report.props) + " props" +
        (actor ? ", player at the actor's spawn" : point ? ", player at a graph point" : "") +
        (indoor ? ", indoors (no sun)" : ""));
    return true;
}

} // namespace stalker
