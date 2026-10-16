#include "WaveSim.hpp"

#include <algorithm>
#include <cmath>

namespace ocean {

namespace {
constexpr float kStep   = 1.0f / 60.0f;  // fixed, so the waves do not depend on the frame rate
constexpr int   kSponge = 14;            // cells at the edge that soak waves up instead of reflecting them
constexpr float kMaxH   = 2.5f;
} // namespace

WaveSim::WaveSim(int cells, float cellSize)
    : m_n(std::max(cells, 16)), m_cell(std::max(cellSize, 0.05f)) {
    clear();
}

void WaveSim::clear() {
    const std::size_t nn = static_cast<std::size_t>(m_n) * m_n;
    m_h.assign(nn, 0.0f);
    m_v.assign(nn, 0.0f);
    m_tex.assign(nn, glm::vec4(0.0f));
    m_last.clear();
    ++m_gen;
}

glm::vec2 WaveSim::origin() const { return glm::vec2(m_originCell) * m_cell; }

void WaveSim::follow(glm::vec2 xz) {
    const glm::ivec2 want(static_cast<int>(std::floor(xz.x / m_cell)) - m_n / 2,
                          static_cast<int>(std::floor(xz.y / m_cell)) - m_n / 2);
    const glm::ivec2 d = want - m_originCell;
    // Not every cell the camera crosses: a shift copies the whole patch, and
    // a few metres of slack cost nothing.
    if (std::abs(d.x) >= 8 || std::abs(d.y) >= 8) shift(d.x, d.y);
}

void WaveSim::shift(int dx, int dz) {
    const int n = m_n;
    std::vector<float> h(m_h.size(), 0.0f), v(m_v.size(), 0.0f);
    for (int z = 0; z < n; ++z) {
        const int sz = z + dz;
        if (sz < 0 || sz >= n) continue;
        for (int x = 0; x < n; ++x) {
            const int sx = x + dx;
            if (sx < 0 || sx >= n) continue;
            h[static_cast<std::size_t>(z) * n + x] = m_h[static_cast<std::size_t>(sz) * n + sx];
            v[static_cast<std::size_t>(z) * n + x] = m_v[static_cast<std::size_t>(sz) * n + sx];
        }
    }
    m_h.swap(h);
    m_v.swap(v);
    m_originCell += glm::ivec2(dx, dz);
}

void WaveSim::imprint(const Body& b, float sign) {
    if (b.depth <= 0.0f || b.radius <= 0.0f) return;
    const float r  = std::max(b.radius, m_cell * 0.75f);
    const float cx = b.xz.x / m_cell - 0.5f - static_cast<float>(m_originCell.x);
    const float cz = b.xz.y / m_cell - 0.5f - static_cast<float>(m_originCell.y);
    const float rc = r / m_cell;
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - rc)));
    const int x1 = std::min(m_n - 1, static_cast<int>(std::ceil(cx + rc)));
    const int z0 = std::max(0, static_cast<int>(std::floor(cz - rc)));
    const int z1 = std::min(m_n - 1, static_cast<int>(std::ceil(cz + rc)));
    const float d = std::min(b.depth, r) * strength * sign;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const float qx = (static_cast<float>(x) - cx) / rc, qz = (static_cast<float>(z) - cz) / rc;
            const float q2 = qx * qx + qz * qz;
            if (q2 >= 1.0f) continue;
            const float p = (1.0f - q2) * (1.0f - q2);   // a smooth dent, deepest in the middle
            m_h[static_cast<std::size_t>(z) * m_n + x] += d * p;
        }
}

void WaveSim::step(float dt, const std::vector<Body>& bodies) {
    // What changed since last frame: each body fills the dent it left and
    // presses the one it is in now. A body gone from the water fills its dent.
    std::unordered_map<std::uint64_t, Body> now;
    for (const Body& b : bodies) {
        if (auto it = m_last.find(b.id); it != m_last.end()) imprint(it->second, +1.0f);
        imprint(b, -1.0f);
        now[b.id] = b;
    }
    for (const auto& [id, b] : m_last)
        if (!now.count(id)) imprint(b, +1.0f);
    m_last.swap(now);

    const int n = m_n;
    const float c2 = waveSpeed * waveSpeed / (m_cell * m_cell);
    m_accum = std::min(m_accum + std::max(dt, 0.0f), 0.1f);
    while (m_accum >= kStep) {
        m_accum -= kStep;
        for (int z = 1; z < n - 1; ++z)
            for (int x = 1; x < n - 1; ++x) {
                const std::size_t i = static_cast<std::size_t>(z) * n + x;
                const float lap = m_h[i - 1] + m_h[i + 1] + m_h[i - n] + m_h[i + n] - 4.0f * m_h[i];
                // A sponge at the rim: the patch is a window onto open water,
                // and a wave reaching its edge must leave rather than bounce.
                const int edge = std::min(std::min(x, n - 1 - x), std::min(z, n - 1 - z));
                const float soak = edge < kSponge
                                 ? 6.0f * (1.0f - static_cast<float>(edge) / kSponge) : 0.0f;
                m_v[i] += (c2 * lap - (damping + soak) * m_v[i]) * kStep;
            }
        for (std::size_t i = 0; i < m_h.size(); ++i)
            m_h[i] = std::clamp(m_h[i] + m_v[i] * kStep, -kMaxH, kMaxH);
    }

    const float inv2 = 1.0f / (2.0f * m_cell);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const std::size_t i = static_cast<std::size_t>(z) * n + x;
            const float hx = (m_h[static_cast<std::size_t>(z) * n + std::min(x + 1, n - 1)] -
                              m_h[static_cast<std::size_t>(z) * n + std::max(x - 1, 0)]) * inv2;
            const float hz = (m_h[static_cast<std::size_t>(std::min(z + 1, n - 1)) * n + x] -
                              m_h[static_cast<std::size_t>(std::max(z - 1, 0)) * n + x]) * inv2;
            m_tex[i] = glm::vec4(m_h[i], hx, hz, 0.0f);
        }
    ++m_gen;
}

float WaveSim::heightAt(float x, float z) const {
    const float fx = x / m_cell - 0.5f - static_cast<float>(m_originCell.x);
    const float fz = z / m_cell - 0.5f - static_cast<float>(m_originCell.y);
    if (fx < 0.0f || fz < 0.0f || fx >= m_n - 1 || fz >= m_n - 1) return 0.0f;
    const int x0 = static_cast<int>(fx), z0 = static_cast<int>(fz);
    const float ax = fx - x0, az = fz - z0;
    auto at = [&](int xi, int zi) { return m_h[static_cast<std::size_t>(zi) * m_n + xi]; };
    return (at(x0, z0) * (1 - ax) + at(x0 + 1, z0) * ax) * (1 - az) +
           (at(x0, z0 + 1) * (1 - ax) + at(x0 + 1, z0 + 1) * ax) * az;
}

} // namespace ocean
