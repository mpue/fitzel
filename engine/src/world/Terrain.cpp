#include "fitzel/world/Terrain.hpp"

#include <algorithm>
#include <cmath>

#include <stb_perlin.h>

namespace fitzel {

float terrainHeight(const TerrainSettings& s, float worldX, float worldZ) {
    // 1) Domain warp: displace the sample point by a low-frequency noise field
    //    so features bend and meander instead of looking grid-aligned.
    const float wf = s.warpFrequency;
    const float wx = worldX + s.warpStrength *
        stb_perlin_fbm_noise3(worldX * wf, 0.0f, worldZ * wf, 2.0f, 0.5f, 4);
    const float wz = worldZ + s.warpStrength *
        stb_perlin_fbm_noise3((worldX + 101.7f) * wf, 0.0f,
                              (worldZ + 57.1f) * wf, 2.0f, 0.5f, 4);

    const float f = s.frequency;

    // 2) Rolling base terrain (signed fBm).
    const float base = stb_perlin_fbm_noise3(
        wx * f + s.seed, 0.0f, wz * f + s.seed, s.lacunarity, s.gain, s.octaves);

    // 3) Sharp mountain ridges (ridged multifractal), raised on the highlands so
    //    ranges grow out of higher ground rather than the whole field.
    const float ridge = stb_perlin_ridge_noise3(
        wx * f * 1.9f + s.seed, 0.0f, wz * f * 1.9f + s.seed,
        2.0f, 0.5f, 1.0f, s.octaves);
    const float mask = glm::smoothstep(-0.1f, 0.55f, base);

    return base * s.heightScale + ridge * s.ridgeScale * mask;
}

TerrainChunk TerrainChunk::generate(const TerrainSettings& s, glm::ivec2 coord) {
    TerrainChunk chunk;
    chunk.m_coord = coord;

    const int   verts  = std::max(2, s.resolution + 1);
    const float step   = s.chunkSize / static_cast<float>(s.resolution);
    const float originX = coord.x * s.chunkSize;
    const float originZ = coord.y * s.chunkSize;

    // World-space height sampler; used for both positions and normals so chunk
    // edges line up exactly with their neighbours.
    auto height = [&](float wx, float wz) { return terrainHeight(s, wx, wz); };

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<std::size_t>(verts) * verts);
    for (int z = 0; z < verts; ++z) {
        for (int x = 0; x < verts; ++x) {
            const float wx = originX + x * step;
            const float wz = originZ + z * step;

            // Central differences in world space (continuous across chunks).
            const float hl = height(wx - step, wz);
            const float hr = height(wx + step, wz);
            const float hd = height(wx, wz - step);
            const float hu = height(wx, wz + step);
            const glm::vec3 normal =
                glm::normalize(glm::vec3(hl - hr, 2.0f * step, hd - hu));

            Vertex v;
            v.position = {wx, height(wx, wz), wz};
            v.normal   = normal;
            v.uv       = {static_cast<float>(x) / s.resolution,
                          static_cast<float>(z) / s.resolution};
            vertices.push_back(v);
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(s.resolution) * s.resolution * 6);
    for (int z = 0; z < verts - 1; ++z) {
        for (int x = 0; x < verts - 1; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(z * verts + x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + verts;
            const std::uint32_t i3 = i2 + 1;
            indices.insert(indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }

    chunk.m_mesh = Mesh::create(vertices, indices);
    return chunk;
}

TerrainStreamer::TerrainStreamer(const TerrainSettings& settings, int radius)
    : m_settings(settings), m_radius(std::max(0, radius)) {}

std::int64_t TerrainStreamer::key(glm::ivec2 c) {
    return (static_cast<std::int64_t>(c.x) << 32) ^
           (static_cast<std::uint32_t>(c.y));
}

glm::ivec2 TerrainStreamer::chunkCoordOf(const glm::vec3& pos) const {
    return {static_cast<int>(std::floor(pos.x / m_settings.chunkSize)),
            static_cast<int>(std::floor(pos.z / m_settings.chunkSize))};
}

int TerrainStreamer::update(const glm::vec3& cameraPos) {
    const glm::ivec2 c = chunkCoordOf(cameraPos);
    if (c == m_center && !m_dirty) {
        return 0;
    }
    m_center = c;
    m_dirty  = false;

    // Drop chunks outside the radius.
    for (auto it = m_chunks.begin(); it != m_chunks.end();) {
        const glm::ivec2 cc = it->second.coord();
        if (std::abs(cc.x - c.x) > m_radius || std::abs(cc.y - c.y) > m_radius) {
            it = m_chunks.erase(it);
        } else {
            ++it;
        }
    }

    // Generate any missing chunks within the radius.
    int generated = 0;
    for (int dz = -m_radius; dz <= m_radius; ++dz) {
        for (int dx = -m_radius; dx <= m_radius; ++dx) {
            const glm::ivec2 cc{c.x + dx, c.y + dz};
            const std::int64_t k = key(cc);
            if (m_chunks.find(k) == m_chunks.end()) {
                m_chunks.emplace(k, TerrainChunk::generate(m_settings, cc));
                ++generated;
            }
        }
    }

    refreshVisible();
    return generated;
}

void TerrainStreamer::rebuild() {
    m_chunks.clear();
    m_dirty = true;
}

void TerrainStreamer::refreshVisible() {
    m_visible.clear();
    m_visible.reserve(m_chunks.size());
    for (const auto& [k, chunk] : m_chunks) {
        m_visible.push_back(&chunk);
    }
}

} // namespace fitzel
