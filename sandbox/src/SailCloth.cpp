#include "SailCloth.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace sail {
namespace {

constexpr float kPi = 3.14159265f;

// A position (or any vector) rounded so that copies of one corner fall together.
struct Key3 {
    std::int32_t x, y, z;
    bool operator==(const Key3& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct Key3Hash {
    std::size_t operator()(const Key3& k) const {
        return (static_cast<std::size_t>(k.x) * 73856093u) ^
               (static_cast<std::size_t>(k.y) * 19349663u) ^
               (static_cast<std::size_t>(k.z) * 83492791u);
    }
};
Key3 key3(const glm::vec3& v, float q) {
    const glm::vec3 g = glm::round(v / q);
    return {static_cast<std::int32_t>(g.x), static_cast<std::int32_t>(g.y),
            static_cast<std::int32_t>(g.z)};
}
// A whole corner: where it is, which way it faces, where its texture is.
struct CornerKey {
    Key3 p, n, t;
    bool operator==(const CornerKey& o) const { return p == o.p && n == o.n && t == o.t; }
};
struct CornerKeyHash {
    std::size_t operator()(const CornerKey& k) const {
        const Key3Hash h;
        return h(k.p) ^ (h(k.n) * 31u) ^ (h(k.t) * 1009u);
    }
};

} // namespace

bool Cloth::build(const std::vector<fitzel::Vertex>& triangles,
                  const std::vector<glm::vec3>& holders, float hold, float unitIn) {
    *this = Cloth{};
    unit = std::max(unitIn, 1e-4f);
    const std::size_t corners = triangles.size() / 3 * 3;
    if (corners < 3) return false;

    // --- Corners shared, positions welded ------------------------------------
    // Rounded to a tenth of a millimetre (positions) and finer still for the
    // direction and the texture, so a UV seam stays two corners on one node.
    const float qp = 1.0e-4f / unit;
    std::unordered_map<CornerKey, std::uint32_t, CornerKeyHash> cornerAt;
    std::unordered_map<Key3, std::uint32_t, Key3Hash> nodeAt;
    cornerAt.reserve(corners / 3);
    nodeAt.reserve(corners / 4);
    indices.reserve(corners);
    std::vector<std::uint32_t> triNode;
    triNode.reserve(corners);
    for (std::size_t i = 0; i < corners; ++i) {
        const fitzel::Vertex& v = triangles[i];
        const Key3 kp = key3(v.position, qp);
        auto [ci, freshC] = cornerAt.try_emplace(
            CornerKey{kp, key3(v.normal, 1.0e-3f), key3(glm::vec3(v.uv, 0.0f), 1.0e-5f)},
            static_cast<std::uint32_t>(rest.size()));
        auto [ni, freshN] = nodeAt.try_emplace(kp, static_cast<std::uint32_t>(nodePos.size()));
        if (freshN) nodePos.push_back(v.position);
        if (freshC) {
            rest.push_back(v);
            node.push_back(ni->second);
        }
        indices.push_back(ci->second);
        triNode.push_back(ni->second);
    }
    const std::uint32_t nodes = static_cast<std::uint32_t>(nodePos.size());

    // --- The surface: triangles, edges, which edges are the border -----------
    nodeTris.reserve(triNode.size());
    std::unordered_map<std::uint64_t, int> edgeUse;
    edgeUse.reserve(triNode.size());
    const auto edgeKey = [](std::uint32_t a, std::uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };
    for (std::size_t t = 0; t + 2 < triNode.size(); t += 3) {
        const std::uint32_t a = triNode[t], b = triNode[t + 1], c = triNode[t + 2];
        if (a == b || b == c || a == c) continue;   // a sliver with no area
        nodeTris.push_back(a); nodeTris.push_back(b); nodeTris.push_back(c);
        ++edgeUse[edgeKey(a, b)];
        ++edgeUse[edgeKey(b, c)];
        ++edgeUse[edgeKey(c, a)];
    }
    if (nodeTris.empty()) return false;

    // Neighbours, compressed (CSR), and the islands as union-find over edges.
    std::vector<std::uint32_t> start(nodes + 1, 0);
    for (const auto& kv : edgeUse) {
        ++start[static_cast<std::uint32_t>(kv.first >> 32) + 1];
        ++start[static_cast<std::uint32_t>(kv.first & 0xffffffffu) + 1];
    }
    for (std::uint32_t i = 0; i < nodes; ++i) start[i + 1] += start[i];
    std::vector<std::uint32_t> nbr(start[nodes]);
    std::vector<std::uint32_t> fill(start.begin(), start.end() - 1);
    std::vector<std::uint32_t> parent(nodes);
    std::iota(parent.begin(), parent.end(), 0u);
    const auto find = [&](std::uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    std::vector<std::uint8_t> border(nodes, 0);
    for (const auto& kv : edgeUse) {
        const std::uint32_t a = static_cast<std::uint32_t>(kv.first >> 32);
        const std::uint32_t b = static_cast<std::uint32_t>(kv.first & 0xffffffffu);
        nbr[fill[a]++] = b;
        nbr[fill[b]++] = a;
        const std::uint32_t ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
        if (kv.second == 1) border[a] = border[b] = 1;
    }
    island.assign(nodes, -1);
    {
        std::unordered_map<std::uint32_t, int> id;
        for (std::uint32_t n = 0; n < nodes; ++n) {
            auto [it, fresh] = id.try_emplace(find(n), static_cast<int>(islands.size()));
            if (fresh) islands.emplace_back();
            island[n] = it->second;
        }
    }

    // --- Tied where it touches what holds it ---------------------------------
    // The border corners in a grid of cells the size of the reach, and every
    // point of the holders asked about the cells around it: a ship's rigging is
    // millions of corners, the border of its sails a few thousand.
    held.assign(nodes, 0);
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const glm::vec3& p : nodePos) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    if (hold > 0.0f && !holders.empty()) {
        std::unordered_map<Key3, std::vector<std::uint32_t>, Key3Hash> cells;
        const auto cellOf = [&](const glm::vec3& p) {
            const glm::vec3 c = glm::floor(p / hold);
            return Key3{static_cast<std::int32_t>(c.x), static_cast<std::int32_t>(c.y),
                        static_cast<std::int32_t>(c.z)};
        };
        for (std::uint32_t n = 0; n < nodes; ++n)
            if (border[n]) cells[cellOf(nodePos[n])].push_back(n);
        const glm::vec3 reachLo = lo - glm::vec3(hold), reachHi = hi + glm::vec3(hold);
        const float hold2 = hold * hold;
        for (const glm::vec3& p : holders) {
            if (glm::any(glm::lessThan(p, reachLo)) || glm::any(glm::greaterThan(p, reachHi)))
                continue;
            const Key3 c = cellOf(p);
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz) {
                        auto it = cells.find(Key3{c.x + dx, c.y + dy, c.z + dz});
                        if (it == cells.end()) continue;
                        for (std::uint32_t n : it->second) {
                            const glm::vec3 d = nodePos[n] - p;
                            if (glm::dot(d, d) <= hold2) held[n] = 1;
                        }
                    }
        }
    }
    // A piece that touches nothing is tied at its top -- its highest border
    // corner, or its highest corner when it has no border at all (a closed bag).
    {
        std::vector<int> top(islands.size(), -1);
        for (std::uint32_t n = 0; n < nodes; ++n) {
            Island& is = islands[island[n]];
            ++is.nodes;
            if (held[n]) ++is.seeds;
            int& t = top[island[n]];
            const bool better = t < 0 || (border[n] && !border[t]) ||
                                (border[n] == border[t] && nodePos[n].y > nodePos[t].y);
            if (better) t = static_cast<int>(n);
        }
        for (std::size_t i = 0; i < islands.size(); ++i)
            if (islands[i].seeds == 0 && top[i] >= 0) held[top[i]] = 1;
    }

    // --- How far each corner is from its tie, across the cloth ---------------
    std::vector<float> dist(nodes, 1e30f);
    using QE = std::pair<float, std::uint32_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    for (std::uint32_t n = 0; n < nodes; ++n)
        if (held[n]) { dist[n] = 0.0f; open.push({0.0f, n}); }
    while (!open.empty()) {
        const auto [d, n] = open.top();
        open.pop();
        if (d > dist[n]) continue;
        for (std::uint32_t k = start[n]; k < start[n + 1]; ++k) {
            const std::uint32_t m = nbr[k];
            const float nd = d + glm::length(nodePos[m] - nodePos[n]);
            if (nd < dist[m]) { dist[m] = nd; open.push({nd, m}); }
        }
    }

    // --- Each piece: its size, the way it faces, its ripples ------------------
    for (std::uint32_t n = 0; n < nodes; ++n) {
        Island& is = islands[island[n]];
        is.centroid += nodePos[n];
        if (dist[n] < 1e29f) is.length = std::max(is.length, dist[n]);
    }
    for (std::size_t t = 0; t + 2 < nodeTris.size(); t += 3) {
        const glm::vec3& a = nodePos[nodeTris[t]];
        islands[island[nodeTris[t]]].normal +=
            glm::cross(nodePos[nodeTris[t + 1]] - a, nodePos[nodeTris[t + 2]] - a);
    }
    for (std::size_t i = 0; i < islands.size(); ++i) {
        Island& is = islands[i];
        is.centroid /= static_cast<float>(std::max(is.nodes, 1));
        const float nl = glm::length(is.normal);
        is.normal = nl > 1e-12f ? is.normal / nl : glm::vec3(0.0f, 0.0f, 1.0f);
        // Ripples a quarter of the cloth long, within what canvas does: a flag
        // shows a few waves at once, a course one or two.
        is.wave = std::clamp(0.25f * is.length * unit + 0.8f, 1.2f, 5.0f) / unit;
        // Each piece starts at its own point in its swaying, or a whole ship's
        // sails breathe in step.
        is.sway = 2.3999632f * static_cast<float>(i);
    }
    weight.assign(nodes, 0.0f);
    wave.assign(nodes, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    for (std::uint32_t n = 0; n < nodes; ++n) {
        const Island& is = islands[island[n]];
        const float d = dist[n] < 1e29f ? dist[n] : 0.0f;
        weight[n] = is.length > 1e-6f ? std::clamp(d / is.length, 0.0f, 1.0f) : 0.0f;
        // The wavefronts are not straight: a phase that wanders across the
        // cloth (in metres, so a big sail and a small one wander alike).
        const glm::vec3 m = nodePos[n] * unit;
        const float psi = 0.9f * std::sin(0.37f * m.x + 0.61f * m.z) +
                          0.6f * std::sin(0.53f * m.y - 0.29f * m.x);
        const float a = 2.0f * kPi * d / is.wave + psi;
        const float b = 1.73f * 2.0f * kPi * d / is.wave + 2.1f + 1.7f * psi;
        wave[n] = glm::vec4(std::sin(a), std::cos(a), std::sin(b), std::cos(b));
    }
    return true;
}

void Cloth::pose(const Air& air, float billow, float flutter, float dt,
                 std::vector<fitzel::Vertex>& out) {
    const std::size_t nodes = nodePos.size();
    if (!nodes) return;

    // Per piece: how hard the air presses it, which way, how fast its ripples run.
    struct Now { float bellyAmp, edgeAmp, breath, s1, c1, s2, c2, side; };
    std::vector<Now> now(islands.size());
    for (std::size_t i = 0; i < islands.size(); ++i) {
        Island& is = islands[i];
        const float g = air.gust ? air.gust[i] : 1.0f;
        const float u = air.speed * g;
        // A fresh gale (14 m/s) is the full billow and flutter; a storm more.
        const float q = std::clamp(u / 14.0f, 0.0f, 2.2f);
        const float facing = glm::dot(is.normal, air.dir);
        const float f = std::fabs(facing);
        // Ripples run at a third of the wind; how fast they pass is that over
        // their length.
        const float c = std::clamp(0.35f * u, 1.5f, 9.0f);
        const float w = 2.0f * kPi * c / std::max(is.wave * unit, 1e-3f);
        is.phase1 = std::fmod(is.phase1 + w * dt, 2.0f * kPi);
        is.phase2 = std::fmod(is.phase2 + 1.6f * w * dt, 2.0f * kPi);
        is.sway   = std::fmod(is.sway + 2.0f * kPi * 0.23f * dt, 2.0f * kPi);
        Now& n = now[i];
        // Square to the wind it bellies; edge-on (a flag streaming) it flaps.
        n.bellyAmp = billow  / unit * q * (0.35f + 0.65f * f);
        n.edgeAmp  = flutter / unit * q * (0.5f + 0.5f * (1.0f - f));
        // The belly swells in a gust and slackens in the lull behind it.
        n.breath = 0.8f * (g - 1.0f) + 0.25f * std::sin(is.sway);
        n.s1 = std::sin(is.phase1); n.c1 = std::cos(is.phase1);
        n.s2 = std::sin(is.phase2); n.c2 = std::cos(is.phase2);
        n.side = facing >= 0.0f ? 1.0f : -1.0f;
    }

    // Rest normals per node, once: the direction each corner is pushed in.
    if (m_restN.size() != nodes) {
        m_restN.assign(nodes, glm::vec3(0.0f));
        for (std::size_t t = 0; t + 2 < nodeTris.size(); t += 3) {
            const std::uint32_t a = nodeTris[t], b = nodeTris[t + 1], c = nodeTris[t + 2];
            const glm::vec3 fn = glm::cross(nodePos[b] - nodePos[a], nodePos[c] - nodePos[a]);
            m_restN[a] += fn; m_restN[b] += fn; m_restN[c] += fn;
        }
        for (glm::vec3& n : m_restN) {
            const float l = glm::length(n);
            n = l > 1e-12f ? n / l : glm::vec3(0.0f, 1.0f, 0.0f);
        }
        // A corner whose own normal faces the other way from its winding keeps
        // facing that way.
        m_flip.assign(rest.size(), 1);
        for (std::size_t i = 0; i < rest.size(); ++i)
            if (glm::dot(rest[i].normal, m_restN[node[i]]) < 0.0f) m_flip[i] = -1;
    }

    m_pos.resize(nodes);
    for (std::size_t n = 0; n < nodes; ++n) {
        const Now& k = now[island[n]];
        const float w = weight[n];
        const glm::vec4& wv = wave[n];
        // sin(a - phase): the ripple travels away from the tie.
        const float r1 = wv.x * k.c1 - wv.y * k.s1;
        const float r2 = wv.z * k.c2 - wv.w * k.s2;
        const float belly = std::sin(0.5f * kPi * w);
        const float edge  = w * std::sqrt(w);
        const float d = k.bellyAmp * belly * k.breath + k.edgeAmp * edge * (0.65f * r1 + 0.35f * r2);
        m_pos[n] = nodePos[n] + m_restN[n] * (d * k.side);
    }

    // The normals of the posed cloth, so the ripples catch the light.
    m_nrm.assign(nodes, glm::vec3(0.0f));
    for (std::size_t t = 0; t + 2 < nodeTris.size(); t += 3) {
        const std::uint32_t a = nodeTris[t], b = nodeTris[t + 1], c = nodeTris[t + 2];
        const glm::vec3 fn = glm::cross(m_pos[b] - m_pos[a], m_pos[c] - m_pos[a]);
        m_nrm[a] += fn; m_nrm[b] += fn; m_nrm[c] += fn;
    }
    out.resize(rest.size());
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::uint32_t n = node[i];
        out[i] = rest[i];
        out[i].position = m_pos[n];
        const glm::vec3& a = m_nrm[n];
        const float l2 = glm::dot(a, a);
        out[i].normal = l2 > 1e-24f ? a * (static_cast<float>(m_flip[i]) / std::sqrt(l2))
                                    : rest[i].normal;
    }
}

} // namespace sail
