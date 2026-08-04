#include "TrailSystem.hpp"

#include <cstddef>

#include <fitzel/graphics/Shader.hpp>
#include <fitzel/render/Renderer.hpp>

TrailSystem::TrailSystem(fitzel::Shader& lit) : m_mat(lit) {
    // Flat cool-white, self-illuminated so it reads as glowing vapour rather than
    // a lit surface the sun could dim. Emission is added after lighting in the
    // shader; the renderer resets uEmission to 0 per draw, so this can't leak onto
    // other geometry. Per-submission opacity makes it a translucent streak;
    // uWaterLevel far below the world disables the wet-darkening branch.
    m_mat.set("uColorMode", 0)
         .set("uAlbedo", color)
         .set("uEmission", color)
         .set("uEmissionStrength", glow)
         .set("uWaterLevel", -1.0e4f);
}

void TrailSystem::emit(int id, const glm::vec3& pos) {
    if (!enabled) return;
    Trail& t = m_trails[id];
    if (t.pts.empty() || glm::length(pos - t.pts.back().pos) >= minStep)
        t.pts.push_back({pos, 0.0f});
}

void TrailSystem::update(float dt, const glm::vec3& camPos) {
    if (!enabled) { m_trails.clear(); return; }

    const float invLife = life > 1e-4f ? 1.0f / life : 1.0f;
    auto halfW = [&](float age) { return width * glm::max(0.0f, 1.0f - age * invLife); };

    for (auto it = m_trails.begin(); it != m_trails.end(); ) {
        Trail& t = it->second;
        for (Pt& p : t.pts) p.age += dt;
        while (!t.pts.empty() && t.pts.front().age > life) t.pts.pop_front();

        if (t.pts.size() < 2) {
            if (t.pts.empty()) { it = m_trails.erase(it); continue; }
            t.verts.clear();
            t.mesh.update(t.verts);
            ++it;
            continue;
        }

        // Rebuild the ribbon: for each pair of points a->b, a camera-facing quad
        // whose half-width tapers with each end's age. Emitted double-sided so
        // backface culling can't hide it whichever way the strip runs.
        t.verts.clear();
        auto vtx = [&](const glm::vec3& p, const glm::vec3& nn) {
            fitzel::Vertex v;
            v.position = p; v.normal = nn; v.uv = glm::vec2(0.0f);
            t.verts.push_back(v);
        };
        const std::size_t n = t.pts.size();
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const Pt& a = t.pts[i];
            const Pt& b = t.pts[i + 1];
            const glm::vec3 seg = b.pos - a.pos;
            if (glm::length(seg) < 1e-5f) continue;
            const glm::vec3 dir = glm::normalize(seg);
            const glm::vec3 ca = camPos - a.pos, cb = camPos - b.pos;
            glm::vec3 sa = glm::cross(dir, ca);
            glm::vec3 sb = glm::cross(dir, cb);
            sa = glm::length(sa) > 1e-5f ? glm::normalize(sa) : glm::vec3(1, 0, 0);
            sb = glm::length(sb) > 1e-5f ? glm::normalize(sb) : glm::vec3(1, 0, 0);
            const float wa = halfW(a.age), wb = halfW(b.age);
            const glm::vec3 aL = a.pos - sa * wa, aR = a.pos + sa * wa;
            const glm::vec3 bL = b.pos - sb * wb, bR = b.pos + sb * wb;
            const glm::vec3 nA = glm::length(ca) > 1e-5f ? glm::normalize(ca) : glm::vec3(0, 1, 0);
            const glm::vec3 nB = glm::length(cb) > 1e-5f ? glm::normalize(cb) : glm::vec3(0, 1, 0);
            // front
            vtx(aL, nA); vtx(aR, nA); vtx(bR, nB);
            vtx(aL, nA); vtx(bR, nB); vtx(bL, nB);
            // back (reverse winding -> double-sided)
            vtx(aL, nA); vtx(bR, nB); vtx(aR, nA);
            vtx(aL, nA); vtx(bL, nB); vtx(bR, nB);
        }
        t.mesh.update(t.verts);
        ++it;
    }
}

void TrailSystem::render(fitzel::Renderer& renderer) {
    if (!enabled) return;
    m_mat.set("uAlbedo", color).set("uEmission", color).set("uEmissionStrength", glow);
    for (auto& kv : m_trails)
        if (!kv.second.verts.empty())
            renderer.submit(kv.second.mesh, m_mat, glm::mat4(1.0f), false, false,
                            opacity, /*forceTransparent=*/true);
}
