#include "ShoreMap.hpp"

#include <algorithm>
#include <cmath>

namespace ocean {

namespace {

constexpr float kFar = 1.0e4f;   // "no shore anywhere near", metres

// Two-pass chamfer distance (in cells) from every cell to the nearest cell where
// `seed` holds: the classic 1 / sqrt(2) masks, forward then backward. Within a
// few percent of the true Euclidean distance, which for where a wave breaks is
// far better than needed, and two sweeps of a 256-square grid cost nothing.
void chamfer(int n, const std::vector<char>& seed, std::vector<float>& d) {
    const float inf = 1e9f, diag = 1.41421356f;
    d.assign(static_cast<std::size_t>(n) * n, inf);
    for (std::size_t i = 0; i < d.size(); ++i) if (seed[i]) d[i] = 0.0f;
    auto at = [&](int x, int z) -> float& { return d[static_cast<std::size_t>(z) * n + x]; };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            float v = at(x, z);
            if (x > 0)              v = std::min(v, at(x - 1, z) + 1.0f);
            if (z > 0)              v = std::min(v, at(x, z - 1) + 1.0f);
            if (x > 0 && z > 0)     v = std::min(v, at(x - 1, z - 1) + diag);
            if (x < n - 1 && z > 0) v = std::min(v, at(x + 1, z - 1) + diag);
            at(x, z) = v;
        }
    for (int z = n - 1; z >= 0; --z)
        for (int x = n - 1; x >= 0; --x) {
            float v = at(x, z);
            if (x < n - 1)              v = std::min(v, at(x + 1, z) + 1.0f);
            if (z < n - 1)              v = std::min(v, at(x, z + 1) + 1.0f);
            if (x < n - 1 && z < n - 1) v = std::min(v, at(x + 1, z + 1) + diag);
            if (x > 0 && z < n - 1)     v = std::min(v, at(x - 1, z + 1) + diag);
            at(x, z) = v;
        }
}

} // namespace

ShoreMap::ShoreMap(int cells, float cellSize) : m_n(std::max(cells, 16)), m_cell(cellSize) {}

ShoreMap::~ShoreMap() {
    if (m_worker.joinable()) m_worker.join();
}

void ShoreMap::build(int n, float cell, glm::vec2 origin, float level, const HeightFn& height,
                     std::vector<glm::vec4>& out) {
    const std::size_t nn = static_cast<std::size_t>(n) * n;
    std::vector<float> depth(nn);
    std::vector<char>  land(nn), water(nn);
    bool anyLand = false, anyWater = false;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::size_t i = static_cast<std::size_t>(z) * n + x;
            const float h = height(origin.x + (x + 0.5f) * cell, origin.y + (z + 0.5f) * cell);
            depth[i] = level - h;
            land[i]  = depth[i] <= 0.0f;
            water[i] = !land[i];
            anyLand  = anyLand || land[i];
            anyWater = anyWater || water[i];
        }
    // Distance to the shore: out on the water to the nearest land, up the
    // beach (negative) to the nearest water. Half a cell off each, so the line
    // itself sits between the two cells rather than on one of them.
    std::vector<float> toLand, toWater;
    if (anyLand) chamfer(n, land, toLand);
    if (anyWater) chamfer(n, water, toWater);
    std::vector<float> dist(nn);
    for (std::size_t i = 0; i < nn; ++i) {
        if (water[i]) dist[i] = anyLand ? (toLand[i] - 0.5f) * cell : kFar;
        else          dist[i] = anyWater ? -(toWater[i] - 0.5f) * cell : -kFar;
    }

    // Toward the shore: down the distance field. Then smoothed, twice over five
    // cells, because the field of a ragged coast has creases (where two pieces
    // of shore are equally near) that would send neighbouring waves off at
    // right angles to each other.
    std::vector<glm::vec2> dir(nn, glm::vec2(0.0f)), tmp(nn);
    auto D = [&](int x, int z) {
        x = std::clamp(x, 0, n - 1); z = std::clamp(z, 0, n - 1);
        return dist[static_cast<std::size_t>(z) * n + x];
    };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const glm::vec2 g(D(x + 1, z) - D(x - 1, z), D(x, z + 1) - D(x, z - 1));
            const float l = glm::length(g);
            const float d = dist[static_cast<std::size_t>(z) * n + x];
            if (l > 1e-4f && std::fabs(d) < kFar * 0.5f)
                dir[static_cast<std::size_t>(z) * n + x] = -g / l;
        }
    for (int pass = 0; pass < 2; ++pass) {
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x) {
                glm::vec2 s(0.0f);
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int xx = std::clamp(x + dx, 0, n - 1), zz = std::clamp(z + dz, 0, n - 1);
                        s += dir[static_cast<std::size_t>(zz) * n + xx];
                    }
                tmp[static_cast<std::size_t>(z) * n + x] = s;
            }
        for (std::size_t i = 0; i < nn; ++i) {
            const float l = glm::length(tmp[i]);
            dir[i] = l > 1e-4f ? tmp[i] / l : glm::vec2(0.0f);
        }
    }
    out.resize(nn);
    for (std::size_t i = 0; i < nn; ++i)
        out[i] = glm::vec4(depth[i], std::clamp(dist[i], -kFar, kFar), dir[i].x, dir[i].y);
}

void ShoreMap::update(glm::vec2 eye, float level, double now, HeightFn height, bool enabled) {
    // A finished build becomes the map.
    if (m_busy && m_done.load()) {
        m_worker.join();
        m_busy = false;
        m_tex.swap(m_next);
        m_origin = m_nextOrigin;
        m_valid  = true;
        ++m_gen;
    }
    if (!enabled) {
        m_valid = false;
        m_builtAt = -1e9;
        return;
    }
    if (m_busy) return;
    // Rebuild when the eye has moved a good way into the map, the water was
    // moved, or now and then anyway: the ground can be sculpted under the sea.
    const bool moved   = glm::length(eye - m_builtEye) > m_cell * m_n * 0.15f;
    const bool levelCh = std::fabs(level - m_builtLevel) > 0.01f;
    const bool stale   = now - m_builtAt > 4.0;
    if (!(moved || levelCh || stale || !m_valid)) return;
    // Snapped to the cell grid, so a rebuild in place samples the same points.
    const glm::vec2 origin = glm::floor(eye / m_cell - glm::vec2(m_n * 0.5f)) * m_cell;
    m_builtEye   = eye;
    m_builtLevel = level;
    m_builtAt    = now;
    m_nextOrigin = origin;
    m_busy = true;
    m_done.store(false);
    m_worker = std::thread([this, origin, level, height = std::move(height)] {
        build(m_n, m_cell, origin, level, height, m_next);
        m_done.store(true);
    });
}

glm::vec4 ShoreMap::sample(float x, float z) const {
    if (!m_valid || m_tex.empty()) return glm::vec4(1e3f, kFar, 0.0f, 0.0f);
    const float fx = (x - m_origin.x) / m_cell - 0.5f, fz = (z - m_origin.y) / m_cell - 0.5f;
    if (fx < 0.0f || fz < 0.0f || fx >= m_n - 1 || fz >= m_n - 1)
        return glm::vec4(1e3f, kFar, 0.0f, 0.0f);
    const int x0 = static_cast<int>(fx), z0 = static_cast<int>(fz);
    const float ax = fx - x0, az = fz - z0;
    auto at = [&](int xi, int zi) { return m_tex[static_cast<std::size_t>(zi) * m_n + xi]; };
    return glm::mix(glm::mix(at(x0, z0), at(x0 + 1, z0), ax),
                    glm::mix(at(x0, z0 + 1), at(x0 + 1, z0 + 1), ax), az);
}

float ShoreMap::inMap(float x, float z) const {
    if (!m_valid) return 0.0f;
    const glm::vec2 uv = (glm::vec2(x, z) - m_origin) / extent();
    const float e = std::min(std::min(uv.x, uv.y), std::min(1.0f - uv.x, 1.0f - uv.y));
    const float t = std::clamp(e / 0.06f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace ocean
