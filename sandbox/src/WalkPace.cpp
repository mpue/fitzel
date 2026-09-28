#include "WalkPace.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace walkpace {

float stanceSpeed(const fitzel::ModelData& m, int clip, glm::vec3 fwd) {
    if (clip < 0 || clip >= static_cast<int>(m.animations.size()) || m.primitives.empty())
        return 0.0f;
    const float dur = m.animations[clip].duration;
    if (dur <= 1e-3f) return 0.0f;
    std::size_t big = 0;
    for (std::size_t i = 1; i < m.primitives.size(); ++i)
        if (m.primitives[i].vertexCount() > m.primitives[big].vertexCount()) big = i;
    const int kN = 96;
    std::vector<std::vector<fitzel::Vertex>> frames(kN);
    for (int k = 0; k < kN; ++k)
        fitzel::skinPrimitive(m.primitives[big], fitzel::sampleSkeleton(m, clip, dur * k / kN),
                              frames[k]);
    const std::size_t nv = frames[0].size();
    if (nv == 0) return 0.0f;
    // Each vertex's lowest point in the cycle; the hooves are what comes
    // within a couple of percent of the lowest of all.
    std::vector<float> low(nv, 1e30f);
    for (const auto& f : frames)
        for (std::size_t i = 0; i < nv && i < f.size(); ++i) low[i] = std::min(low[i], f[i].position.y);
    const float floorY = *std::min_element(low.begin(), low.end());
    const float H = std::max(m.height(), 1e-3f);
    glm::vec3 c(0.0f);
    for (const auto& v : frames[0]) c += v.position;
    c /= static_cast<float>(nv);
    const glm::vec3 side(fwd.z, 0.0f, -fwd.x);
    // One representative per corner (front/back x left/right): the vertex
    // that gets lowest there.
    int rep[4] = {-1, -1, -1, -1};
    for (std::size_t i = 0; i < nv; ++i) {
        if (low[i] > floorY + 0.02f * H) continue;
        const glm::vec3 d = frames[0][i].position - c;
        const int q = (glm::dot(d, fwd) > 0.0f ? 1 : 0) + (glm::dot(d, side) > 0.0f ? 2 : 0);
        if (rep[q] < 0 || low[i] < low[static_cast<std::size_t>(rep[q])]) rep[q] = static_cast<int>(i);
    }
    const float dt = dur / kN;
    double sum = 0.0;
    int n = 0;
    for (int q = 0; q < 4; ++q) {
        if (rep[q] < 0) continue;
        const std::size_t i = static_cast<std::size_t>(rep[q]);
        float hi = -1e30f;
        for (int k = 0; k < kN; ++k) hi = std::max(hi, frames[k][i].position.y);
        for (int k = 0; k < kN; ++k) {
            const glm::vec3 p0 = frames[k][i].position, p1 = frames[(k + 1) % kN][i].position;
            // Down: in the lowest quarter of its own lift, and going back. (A
            // clip's body bobs, so "down" cannot be a fixed height.)
            const float cut = low[i] + 0.25f * (hi - low[i]);
            const float back = -glm::dot(p1 - p0, fwd);
            if (p0.y > cut || p1.y > cut || back <= 0.0f) continue;
            sum += back / dt;
            ++n;
        }
    }
    return n > 0 ? static_cast<float>(sum / n) : 0.0f;
}

int cycles(const fitzel::ModelData& m, int clip, float start, float end) {
    if (clip < 0 || clip >= static_cast<int>(m.animations.size()) || end - start <= 1e-3f) return 1;
    // The pose at kM instants; a candidate count n repeats when the pose one
    // n-th of the span later is (nearly) the same, all the way through.
    constexpr int kM = 240;   // divisible by every candidate below
    std::vector<std::vector<glm::mat4>> at(kM);
    for (int i = 0; i < kM; ++i)
        at[i] = fitzel::sampleSkeleton(m, clip, start + (end - start) * static_cast<float>(i) / kM);
    if (at[0].empty()) return 1;
    // Joint orientations only: a walk in place still bobs.
    auto apart = [&](int a, int b) {
        double d = 0.0;
        for (std::size_t j = 0; j < at[a].size(); ++j)
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 3; ++r) d += std::abs(at[a][j][c][r] - at[b][j][c][r]);
        return d / static_cast<double>(at[a].size());
    };
    constexpr int kCounts[] = {2, 3, 4, 5, 6, 8};
    double err[6], worst = 0.0;
    for (int k = 0; k < 6; ++k) {
        const int shift = kM / kCounts[k];
        double s = 0.0;
        for (int i = 0; i + shift < kM; ++i) s += apart(i, i + shift);
        err[k] = s / (kM - shift);
        worst  = std::max(worst, err[k]);
    }
    // A true period is far closer than the shifts that land mid-stride (a half
    // stride swaps the legs); the shortest one that repeats is the stride pair.
    int n = 1;
    for (int k = 0; k < 6; ++k)
        if (err[k] < 0.3 * worst) n = kCounts[k];
    return n;
}

fitzel::ModelPrimitive weld(const fitzel::ModelPrimitive& p, std::vector<std::uint32_t>& indices) {
    fitzel::ModelPrimitive out;
    indices.clear();
    const int n = p.vertexCount();
    const bool skinned = p.skin.size() == static_cast<std::size_t>(n);
    std::unordered_map<std::string, std::uint32_t> seen;
    seen.reserve(static_cast<std::size_t>(n));
    std::string key;
    for (int i = 0; i < n; ++i) {
        const float* v = &p.vertices[static_cast<std::size_t>(i) * 8];
        key.assign(reinterpret_cast<const char*>(v), 8 * sizeof(float));
        if (skinned)
            key.append(reinterpret_cast<const char*>(&p.skin[static_cast<std::size_t>(i)]),
                       sizeof(fitzel::VertexSkin));
        const auto [it, fresh] = seen.try_emplace(key, static_cast<std::uint32_t>(out.vertexCount()));
        if (fresh) {
            out.vertices.insert(out.vertices.end(), v, v + 8);
            if (skinned) out.skin.push_back(p.skin[static_cast<std::size_t>(i)]);
        }
        indices.push_back(it->second);
    }
    return out;
}

} // namespace walkpace
