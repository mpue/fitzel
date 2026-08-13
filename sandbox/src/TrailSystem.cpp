#include "TrailSystem.hpp"

#include <cmath>
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

        // A perpendicular to `d` that depends only on `d`: continuous as the
        // segment turns, and never the zero vector. The axis furthest from `d`
        // is the one that survives the cross product with room to spare.
        auto perpOf = [](const glm::vec3& d) {
            const glm::vec3 up = std::abs(d.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                      : glm::vec3(1.0f, 0.0f, 0.0f);
            return glm::normalize(glm::cross(d, up));
        };
        // Which way the ribbon opens at `p`. Facing the camera where that means
        // something, easing to the fixed perpendicular above where it does not.
        //
        // cross(dir, toCam) has length sin(angle between them), so looking ALONG
        // a segment drives it to zero -- and normalising a near-zero vector
        // returns whatever round-off happened to be in it, a direction that
        // swings right around the segment from one frame to the next. The ribbon
        // then strobes and sweeps across the screen, which is exactly what the
        // trail of the craft AHEAD of you does: that is the one trail you are
        // permanently looking straight down. The old guard could not catch it --
        // it compared against a fixed 1e-5, while the cross product is scaled by
        // the distance to the camera, so at fifty metres a hopelessly ill-
        // conditioned frame still measured well above the threshold.
        auto sideAt = [&](const glm::vec3& p, const glm::vec3& dir) {
            glm::vec3 toCam = camPos - p;
            const float d = glm::length(toCam);
            const glm::vec3 stable0 = perpOf(dir);
            if (d < 1e-4f) return stable0;
            toCam /= d;
            glm::vec3 cs = glm::cross(dir, toCam);
            const float sinA = glm::length(cs);   // 0 = dead in line with the view
            if (sinA < 1e-4f) return stable0;
            cs /= sinA;
            // Take the fixed perpendicular on the same side as the camera-facing
            // one, or blending the two would cancel them out instead of easing
            // between them.
            const glm::vec3 stable = glm::dot(stable0, cs) < 0.0f ? -stable0 : stable0;
            // Eased, not switched: a hard swap at a threshold is its own visible
            // pop. Below ~3 degrees it is entirely the stable frame, above ~17
            // entirely camera-facing.
            const glm::vec3 mixed = glm::mix(stable, cs,
                                             glm::smoothstep(0.06f, 0.30f, sinA));
            const float ml = glm::length(mixed);
            return ml > 1e-4f ? mixed / ml : stable;
        };
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
            const glm::vec3 sa = sideAt(a.pos, dir);
            glm::vec3 sb = sideAt(b.pos, dir);
            // Both ends of one quad have to open the same way. They are computed
            // independently, and near the ill-conditioned case they can land on
            // opposite sides -- which ties the quad into a bow tie.
            if (glm::dot(sa, sb) < 0.0f) sb = -sb;
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
