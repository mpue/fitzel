#include "fitzel/world/Terrain.hpp"

#include <algorithm>
#include <cmath>

#include <stb_perlin.h>

namespace fitzel {

namespace {

// fBm height in world units for a given (x, z) using the supplied params.
float sampleNoise(const TerrainParams& p, float x, float z) {
    const float n = stb_perlin_fbm_noise3(
        x * p.frequency + p.seed, 0.0f, z * p.frequency + p.seed,
        p.lacunarity, p.gain, p.octaves);
    return n * p.heightScale;
}

} // namespace

Terrain Terrain::generate(const TerrainParams& params) {
    Terrain terrain;
    terrain.m_params = params;

    const int   res  = std::max(2, params.resolution);
    const float half = params.worldSize * 0.5f;
    const float step = params.worldSize / static_cast<float>(res - 1);

    // 1) Sample the heightfield.
    auto& heights = terrain.m_heights;
    heights.resize(static_cast<std::size_t>(res) * res);
    for (int z = 0; z < res; ++z) {
        for (int x = 0; x < res; ++x) {
            const float wx = -half + x * step;
            const float wz = -half + z * step;
            heights[static_cast<std::size_t>(z) * res + x] = sampleNoise(params, wx, wz);
        }
    }

    auto h = [&](int x, int z) -> float {
        x = std::clamp(x, 0, res - 1);
        z = std::clamp(z, 0, res - 1);
        return heights[static_cast<std::size_t>(z) * res + x];
    };

    // 2) Build vertices with normals from central differences.
    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<std::size_t>(res) * res);
    for (int z = 0; z < res; ++z) {
        for (int x = 0; x < res; ++x) {
            const float wx = -half + x * step;
            const float wz = -half + z * step;

            const float hl = h(x - 1, z);
            const float hr = h(x + 1, z);
            const float hd = h(x, z - 1);
            const float hu = h(x, z + 1);
            const glm::vec3 normal =
                glm::normalize(glm::vec3(hl - hr, 2.0f * step, hd - hu));

            Vertex v;
            v.position = {wx, h(x, z), wz};
            v.normal   = normal;
            v.uv       = {static_cast<float>(x) / (res - 1),
                          static_cast<float>(z) / (res - 1)};
            vertices.push_back(v);
        }
    }

    // 3) Triangulate the grid.
    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(res - 1) * (res - 1) * 6);
    for (int z = 0; z < res - 1; ++z) {
        for (int x = 0; x < res - 1; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(z * res + x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + res;
            const std::uint32_t i3 = i2 + 1;
            indices.insert(indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }

    terrain.m_mesh = Mesh::create(vertices, indices);
    return terrain;
}

float Terrain::heightAt(float worldX, float worldZ) const {
    const int   res  = std::max(2, m_params.resolution);
    const float half = m_params.worldSize * 0.5f;
    const float step = m_params.worldSize / static_cast<float>(res - 1);

    // Map world space to grid space.
    const float gx = (worldX + half) / step;
    const float gz = (worldZ + half) / step;
    if (gx < 0.0f || gz < 0.0f || gx > res - 1 || gz > res - 1) {
        return 0.0f;
    }

    const int x0 = static_cast<int>(gx);
    const int z0 = static_cast<int>(gz);
    const int x1 = std::min(x0 + 1, res - 1);
    const int z1 = std::min(z0 + 1, res - 1);
    const float tx = gx - x0;
    const float tz = gz - z0;

    auto at = [&](int x, int z) {
        return m_heights[static_cast<std::size_t>(z) * res + x];
    };

    const float top = glm::mix(at(x0, z0), at(x1, z0), tx);
    const float bot = glm::mix(at(x0, z1), at(x1, z1), tx);
    return glm::mix(top, bot, tz);
}

} // namespace fitzel
