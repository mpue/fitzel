#include "RoadSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <unordered_set>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/world/Terrain.hpp>

#include "CameraPath.hpp" // catmull()

namespace {
// Number of subdivisions per control-point span when sampling the spline.
constexpr int kSpanSub = 14;

// A road surface must be a colour/albedo map. We can't require "diff" in the
// name (the content folder's PNGs are all support maps and its albedos are .jpg,
// so that would hide every PNG); instead reject the known non-colour maps so
// custom jpg/png road textures still show up.
bool isRoadAlbedo(const std::string& filename) {
    const std::string ext = std::filesystem::path(filename).extension().string();
    if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") return false;
    for (const char* tok : {"_nor", "_disp", "_spec", "_rough", "_ao", "_arm",
                            "_metal", "_height", "_bump", "_mask", "_gl",
                            "translucent", "billboar", "tree"})
        if (filename.find(tok) != std::string::npos) return false;
    return true;
}

// Squared distance from point p to segment [a,b], plus the projection param t.
float distToSeg(glm::vec2 p, glm::vec2 a, glm::vec2 b, float& t) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    t = (len2 > 1e-8f) ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    const glm::vec2 proj = a + ab * t;
    return glm::dot(p - proj, p - proj);
}
} // namespace

RoadSystem::RoadSystem(fitzel::Shader& lit, fitzel::AssetDatabase& assetDb,
                       fitzel::TerrainStreamer& streamer, const std::string& texDir)
    : m_assetDb(assetDb), m_streamer(streamer), m_texDir(texDir), m_mat(lit) {
    m_mat.set("uColorMode", 2);
    // Populate the picker from the built-in content textures (no project yet).
    refreshTextures(std::string());
    // Default surface. Loaded through the asset database (cached/deduplicated);
    // held as a shared handle so it stays alive while the material binds it.
    m_tex = m_assetDb.loadTexture(m_texDir + "/asphalt_02_diff_4k.jpg");
    if (m_tex) m_mat.setTexture("uTexture", *m_tex, 0);
    for (int i = 0; i < static_cast<int>(texFiles.size()); ++i)
        if (texFiles[i].find("asphalt") != std::string::npos) texSel = i;
}

void RoadSystem::refreshTextures(const std::string& projectDir) {
    // Remember the current selection so a project switch doesn't reset it.
    const std::string prevSel =
        (texSel >= 0 && texSel < static_cast<int>(texFiles.size()))
            ? texFiles[texSel] : std::string();

    texFiles.clear();
    m_texPaths.clear();
    std::unordered_set<std::string> seen; // dedupe by display name

    auto add = [&](const std::filesystem::path& p) {
        const std::string name = p.filename().string();
        if (!isRoadAlbedo(name) || !seen.insert(name).second) return;
        texFiles.push_back(name);
        m_texPaths.push_back(p.generic_string());
    };

    // Built-in content textures (flat scan, as before).
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(m_texDir, ec))
        add(e.path());
    // Project-local textures (recursive), so surfaces dropped into the open
    // project show up too. Names already present in content are kept (not shadowed).
    if (!projectDir.empty() && std::filesystem::is_directory(projectDir, ec))
        for (const auto& e :
             std::filesystem::recursive_directory_iterator(projectDir, ec))
            if (!e.is_directory()) add(e.path());

    // Sort display names and keep the parallel path list aligned.
    std::vector<int> order(texFiles.size());
    for (int i = 0; i < static_cast<int>(order.size()); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return texFiles[a] < texFiles[b]; });
    std::vector<std::string> sf, sp;
    sf.reserve(order.size()); sp.reserve(order.size());
    for (int i : order) { sf.push_back(texFiles[i]); sp.push_back(m_texPaths[i]); }
    texFiles.swap(sf);
    m_texPaths.swap(sp);

    // Restore the selection by name (else clamp into range).
    texSel = 0;
    for (int i = 0; i < static_cast<int>(texFiles.size()); ++i)
        if (texFiles[i] == prevSel) { texSel = i; break; }
}

void RoadSystem::setSurface(const std::string& file) {
    // Resolve the display name to a full path via the scanned list; fall back to
    // the content dir for names not in the list (e.g. a scene saved with a
    // texture that is no longer present, or loaded before the list is built).
    std::string path;
    for (std::size_t i = 0; i < texFiles.size(); ++i)
        if (texFiles[i] == file) { path = m_texPaths[i]; break; }
    if (path.empty()) path = m_texDir + "/" + file;
    if (auto t = m_assetDb.loadTexture(path)) {
        m_tex = t;
        m_mat.setTexture("uTexture", *m_tex, 0);
    }
}

std::vector<glm::vec2> RoadSystem::sampleCenterlineXZ() const {
    std::vector<glm::vec2> center;
    const int n = static_cast<int>(roadPts.size());
    if (n < 2) return center;
    // A closed loop needs >= 3 points to be more than a back-and-forth. When
    // looping, control points wrap around (modulo n) so the tangents are
    // continuous across the seam; the extra segment n-1 -> 0 closes the ring.
    const bool loop = closed && n >= 3;
    auto pt = [&](int i) -> glm::vec2 {
        return loop ? roadPts[((i % n) + n) % n]
                    : roadPts[std::clamp(i, 0, n - 1)];
    };
    const int segs = loop ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const glm::vec2 p0 = pt(i - 1);
        const glm::vec2 p1 = pt(i);
        const glm::vec2 p2 = pt(i + 1);
        const glm::vec2 p3 = pt(i + 2);
        // Each span drops its final sample (it repeats the next span's first);
        // only the very last segment keeps it, to terminate the open line or to
        // land back on the start point and close the loop.
        const int last = (i == segs - 1) ? kSpanSub : kSpanSub - 1;
        for (int s = 0; s <= last; ++s)
            center.push_back(catmull(p0, p1, p2, p3,
                                     static_cast<float>(s) / kSpanSub));
    }
    return center;
}

void RoadSystem::loft(const std::vector<glm::vec2>& center,
                      const std::vector<float>& height) {
    fitzel::MeshData md;
    m_centerline = center; // flat centre for vegetation masking

    const float half = width * 0.5f;
    float vlen = 0.0f;
    for (std::size_t i = 0; i < center.size(); ++i) {
        glm::vec2 fwd = (i == 0)                 ? center[1] - center[0]
                      : (i + 1 == center.size()) ? center[i] - center[i - 1]
                                                 : center[i + 1] - center[i - 1];
        if (glm::length(fwd) < 1e-4f) fwd = glm::vec2(0, 1);
        fwd = glm::normalize(fwd);
        // Perpendicular in XZ, matching cross((0,1,0), fwd) so the ribbon winds
        // front-face-up (see the index order below).
        const glm::vec2 side(fwd.y, -fwd.x);
        if (i > 0) vlen += glm::length(center[i] - center[i - 1]);
        const float v = vlen / texTile;
        const glm::vec3 Lp(center[i].x - side.x * half, height[i],
                           center[i].y - side.y * half);
        const glm::vec3 Rp(center[i].x + side.x * half, height[i],
                           center[i].y + side.y * half);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        md.vertices.push_back({Lp, up, {0.0f, v}});
        md.vertices.push_back({Rp, up, {width / texTile, v}});
    }
    // Two triangles per rung, wound CCW-from-above (front faces up).
    for (std::uint32_t i = 0; i + 1 < center.size(); ++i) {
        const std::uint32_t a = 2 * i;
        md.indices.insert(md.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
    }
    m_mesh  = fitzel::Mesh::create(md);
    m_verts = static_cast<int>(md.vertices.size());

    // Keep the CPU geometry for the physics mesh collider (Play mode).
    m_collVerts.clear();
    m_collVerts.reserve(md.vertices.size());
    for (const fitzel::Vertex& vtx : md.vertices) m_collVerts.push_back(vtx.position);
    m_collIndices = md.indices;
}

void RoadSystem::rebuildMesh() {
    needsBuild = false;
    const std::vector<glm::vec2> center = sampleCenterlineXZ();
    if (center.size() < 2) {
        m_mesh = fitzel::Mesh(); m_verts = 0;
        m_collVerts.clear(); m_collIndices.clear(); m_centerline.clear();
        return;
    }
    // Drape on the current terrain (which already holds the graded corridor after
    // a build/scene-load), lifted a touch so the ribbon reads above the ground.
    std::vector<float> h(center.size());
    for (std::size_t i = 0; i < center.size(); ++i)
        h[i] = m_streamer.heightAt(center[i].x, center[i].y) + 0.06f;
    loft(center, h);
}

bool RoadSystem::build(fitzel::TerrainEditField& edit, glm::vec2& outMin,
                       glm::vec2& outMax) {
    needsBuild = false;
    vegDirty   = true; // vegetation must re-evaluate against the new road

    const std::vector<glm::vec2> center = sampleCenterlineXZ();
    if (center.size() < 2) {
        m_mesh = fitzel::Mesh(); m_verts = 0;
        m_collVerts.clear(); m_collIndices.clear(); m_centerline.clear();
        return false;
    }

    const fitzel::TerrainSettings& s = m_streamer.settings();

    // 1) Longitudinal profile: start from the *base* (procedural) terrain height
    //    under each sample, then low-pass it so the road grades smoothly instead of
    //    following every bump. Anchor the ends to the ground so the road meets the
    //    terrain where it begins/ends. Working from the base makes rebuilds
    //    idempotent (independent of any corridor already baked in).
    std::vector<float> prof(center.size());
    for (std::size_t i = 0; i < center.size(); ++i)
        prof[i] = terrainBaseHeight(s, center[i].x, center[i].y);
    const int passes = 3 + static_cast<int>(grade * 30.0f);
    std::vector<float> tmp = prof;
    for (int p = 0; p < passes; ++p) {
        for (std::size_t i = 1; i + 1 < prof.size(); ++i)
            tmp[i] = 0.5f * prof[i] + 0.25f * (prof[i - 1] + prof[i + 1]);
        std::swap(prof, tmp); // ends stay fixed (anchored to the terrain)
    }

    // 2) Loft the ribbon on the graded profile (lifted a hair above the surface).
    std::vector<float> surf(prof.size());
    for (std::size_t i = 0; i < prof.size(); ++i) surf[i] = prof[i] + 0.06f;
    loft(center, surf);

    // 3) Grade a corridor into the terrain edit field: cells within half-width get
    //    the road height; a `shoulder` band eases back to the natural ground. All
    //    deltas are stored relative to the base terrain, so the corridor is fully
    //    owned/overwritten here (repeatable, and it flattens whatever was under it).
    const float half   = width * 0.5f;
    const float reach  = half + shoulder;
    glm::vec2 lo(center[0]), hi(center[0]);
    for (const glm::vec2& c : center) {
        lo = glm::min(lo, c); hi = glm::max(hi, c);
    }
    lo -= glm::vec2(reach); hi += glm::vec2(reach);

    const float cell = edit.cell;
    const int ix0 = static_cast<int>(std::floor(lo.x / cell));
    const int ix1 = static_cast<int>(std::ceil (hi.x / cell));
    const int iz0 = static_cast<int>(std::floor(lo.y / cell));
    const int iz1 = static_cast<int>(std::ceil (hi.y / cell));
    for (int iz = iz0; iz <= iz1; ++iz) {
        for (int ix = ix0; ix <= ix1; ++ix) {
            const glm::vec2 w(ix * cell, iz * cell);
            // Nearest point on the centreline + the road height there.
            float bestD2 = reach * reach + 1.0f, roadH = 0.0f;
            for (std::size_t k = 0; k + 1 < center.size(); ++k) {
                float t;
                const float d2 = distToSeg(w, center[k], center[k + 1], t);
                if (d2 < bestD2) {
                    bestD2 = d2;
                    roadH  = glm::mix(prof[k], prof[k + 1], t);
                }
            }
            const float d = std::sqrt(bestD2);
            if (d > reach) continue; // outside the corridor -> leave the ground be
            const float base = terrainBaseHeight(s, w.x, w.y);
            float target = roadH;
            if (d > half) {
                const float e = glm::clamp((d - half) / shoulder, 0.0f, 1.0f);
                target = glm::mix(roadH, base, e * e * (3.0f - 2.0f * e));
            }
            edit.deltas[fitzel::TerrainEditField::cellKey(ix, iz)] = target - base;
        }
    }

    outMin = lo - glm::vec2(cell);
    outMax = hi + glm::vec2(cell);
    return true;
}

RoadSystem::Preview RoadSystem::previewGeometry() const {
    Preview pv;
    const std::vector<glm::vec2> center = sampleCenterlineXZ();
    if (center.size() < 2) return pv;
    const float half = width * 0.5f;
    pv.center.reserve(center.size());
    pv.left.reserve(center.size());
    pv.right.reserve(center.size());
    for (std::size_t i = 0; i < center.size(); ++i) {
        glm::vec2 fwd = (i == 0)                 ? center[1] - center[0]
                      : (i + 1 == center.size()) ? center[i] - center[i - 1]
                                                 : center[i + 1] - center[i - 1];
        if (glm::length(fwd) < 1e-4f) fwd = glm::vec2(0, 1);
        fwd = glm::normalize(fwd);
        const glm::vec2 side(fwd.y, -fwd.x);
        const glm::vec2 c = center[i];
        const glm::vec2 l = c - side * half;
        const glm::vec2 r = c + side * half;
        pv.center.push_back({c.x, m_streamer.heightAt(c.x, c.y) + 0.10f, c.y});
        pv.left.push_back  ({l.x, m_streamer.heightAt(l.x, l.y) + 0.10f, l.y});
        pv.right.push_back ({r.x, m_streamer.heightAt(r.x, r.y) + 0.10f, r.y});
    }
    return pv;
}
