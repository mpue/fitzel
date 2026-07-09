#include "RoadSystem.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <system_error>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/world/Terrain.hpp>

#include "CameraPath.hpp" // catmull()

RoadSystem::RoadSystem(fitzel::Shader& lit, fitzel::AssetDatabase& assetDb,
                       fitzel::TerrainStreamer& streamer, const std::string& texDir)
    : m_assetDb(assetDb), m_streamer(streamer), m_texDir(texDir), m_mat(lit) {
    // Loaded through the asset database (cached/deduplicated); held as a shared
    // handle so it stays alive while the material binds it.
    m_tex = m_assetDb.loadTexture(m_texDir + "/asphalt_02_diff_4k.jpg");
    m_mat.set("uColorMode", 2);
    if (m_tex) m_mat.setTexture("uTexture", *m_tex, 0);

    // Selectable surface: gather the diffuse/albedo textures from the texture dir.
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(m_texDir, ec)) {
        const std::string name = e.path().filename().string();
        const std::string ext  = e.path().extension().string();
        if ((ext == ".jpg" || ext == ".jpeg" || ext == ".png") &&
            name.find("diff") != std::string::npos)
            texFiles.push_back(name);
    }
    std::sort(texFiles.begin(), texFiles.end());
    for (int i = 0; i < static_cast<int>(texFiles.size()); ++i)
        if (texFiles[i].find("asphalt") != std::string::npos) texSel = i;
}

void RoadSystem::setSurface(const std::string& file) {
    if (auto t = m_assetDb.loadTexture(m_texDir + "/" + file)) {
        m_tex = t;
        m_mat.setTexture("uTexture", *m_tex, 0);
    }
}

void RoadSystem::rebuild() {
    dirty    = false;
    vegDirty = true; // vegetation must re-evaluate against the new road
    m_centerline.clear();
    fitzel::MeshData md;
    const int n = static_cast<int>(roadPts.size());
    if (n < 2) {
        m_mesh = fitzel::Mesh(); m_verts = 0;
        m_collVerts.clear(); m_collIndices.clear();
        return;
    }

    // Smooth centreline: Catmull-Rom through the points, draped on terrain.
    std::vector<glm::vec3> center;
    const int SUB = 14;
    for (int i = 0; i < n - 1; ++i) {
        const glm::vec2 p0 = roadPts[std::max(0, i - 1)];
        const glm::vec2 p1 = roadPts[i];
        const glm::vec2 p2 = roadPts[i + 1];
        const glm::vec2 p3 = roadPts[std::min(n - 1, i + 2)];
        const int last = (i == n - 2) ? SUB : SUB - 1;
        for (int s = 0; s <= last; ++s) {
            const glm::vec2 c = catmull(p0, p1, p2, p3, static_cast<float>(s) / SUB);
            center.push_back({c.x, m_streamer.heightAt(c.x, c.y), c.y});
        }
    }

    // Keep the flat centreline for vegetation masking.
    m_centerline.reserve(center.size());
    for (const glm::vec3& p : center) m_centerline.push_back({p.x, p.z});

    // Loft left/right edges perpendicular to the path (in the XZ plane).
    const float half = width * 0.5f;
    float vlen = 0.0f;
    for (std::size_t i = 0; i < center.size(); ++i) {
        glm::vec3 fwd = (i == 0)                 ? center[1] - center[0]
                      : (i + 1 == center.size()) ? center[i] - center[i - 1]
                                                 : center[i + 1] - center[i - 1];
        fwd.y = 0.0f;
        if (glm::length(fwd) < 1e-4f) fwd = glm::vec3(0, 0, 1);
        fwd = glm::normalize(fwd);
        const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0, 1, 0), fwd));
        if (i > 0) vlen += glm::length(center[i] - center[i - 1]);
        const float v = vlen / texTile;
        glm::vec3 Lp = center[i] - side * half;
        glm::vec3 Rp = center[i] + side * half;
        Lp.y = m_streamer.heightAt(Lp.x, Lp.z) + 0.10f; // lift off the ground
        Rp.y = m_streamer.heightAt(Rp.x, Rp.z) + 0.10f;
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
