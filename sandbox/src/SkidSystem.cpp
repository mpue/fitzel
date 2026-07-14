#include "SkidSystem.hpp"

#include <algorithm>
#include <cmath>

#include <fitzel/graphics/Shader.hpp>
#include <fitzel/physics/Physics.hpp>
#include <fitzel/render/Renderer.hpp>

namespace {
// Cap each wheel's trail (triangle-soup vertices). At minStep spacing this is a
// few hundred metres of skid; past it the oldest segments are dropped so memory
// stays bounded through a long drift-heavy session.
constexpr std::size_t kMaxVerts  = 6 * 4000; // 4000 segments per wheel
constexpr std::size_t kDropChunk = 6 * 600;  // segments removed when over cap
constexpr float       kLift      = 0.03f;    // raise marks above the ground (m)
} // namespace

SkidSystem::SkidSystem(fitzel::Shader& lit) : m_mat(lit) {
    // Flat near-black surface; per-submission opacity makes it a translucent
    // streak. uWaterLevel far below the world disables the wet-darkening branch.
    m_mat.set("uColorMode", 0)
         .set("uAlbedo", glm::vec3(0.03f, 0.03f, 0.035f))
         .set("uWaterLevel", -1.0e4f);
}

void SkidSystem::clear() {
    for (Trail& t : m_trail) {
        t.verts.clear();
        t.mesh = fitzel::Mesh();
        t.active = false;
        t.dirty  = false;
    }
}

void SkidSystem::extend(Trail& t, const glm::vec3& pos, const glm::vec3& normal,
                        const glm::vec3& lateral) {
    const glm::vec3 up = glm::length(normal) > 1e-4f ? glm::normalize(normal)
                                                     : glm::vec3(0, 1, 0);
    glm::vec3 side = lateral;
    if (glm::length(side) < 1e-4f) side = glm::vec3(1, 0, 0);
    side = glm::normalize(side);
    const glm::vec3 base = pos + up * kLift;
    const glm::vec3 L = base - side * markHalfW;
    const glm::vec3 R = base + side * markHalfW;

    if (!t.active) { // begin a new strip: no triangles until the second rung
        t.prevL = L; t.prevR = R; t.active = true; return;
    }
    // Only extend once the contact has travelled far enough (keeps the strip from
    // piling up degenerate slivers while a wheel spins in place).
    const glm::vec3 prevMid = (t.prevL + t.prevR) * 0.5f;
    if (glm::length(base - prevMid) < minStep) return;

    // Two triangles bridging the previous rung (prevL,prevR) to (L,R), wound so
    // the face points along `up` (regardless of which way `side` happened to run).
    auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
        glm::vec3 n = glm::cross(b - a, c - a);
        if (glm::dot(n, up) < 0.0f) std::swap(b, c);
        n = glm::cross(b - a, c - a);
        n = glm::length(n) > 1e-8f ? glm::normalize(n) : up;
        t.verts.push_back({a, n, {0.0f, 0.0f}});
        t.verts.push_back({b, n, {0.0f, 0.0f}});
        t.verts.push_back({c, n, {0.0f, 0.0f}});
    };
    tri(t.prevL, t.prevR, L);
    tri(t.prevR, R, L);

    // Bound memory: drop the oldest segments once the cap is exceeded.
    if (t.verts.size() > kMaxVerts)
        t.verts.erase(t.verts.begin(),
                      t.verts.begin() + static_cast<std::ptrdiff_t>(kDropChunk));

    t.prevL = L; t.prevR = R; t.dirty = true;
}

void SkidSystem::update(const fitzel::PhysicsWorld& physics) {
    if (!enabled || !physics.hasVehicle()) return;
    for (int i = 0; i < 4; ++i) {
        glm::vec3 pos, normal, lateral;
        float longSlip = 0.0f, latSlip = 0.0f;
        bool  onGround = false;
        if (!physics.getWheelContact(i, pos, normal, lateral, longSlip, latSlip,
                                     onGround))
            continue;
        const bool slipping = onGround &&
            (std::abs(longSlip) > slipThresh || std::abs(latSlip) > slipThresh);
        if (slipping) extend(m_trail[i], pos, normal, lateral);
        else          m_trail[i].active = false; // break the strip
    }
    for (Trail& t : m_trail)
        if (t.dirty) { t.mesh.update(t.verts); t.dirty = false; }
}

void SkidSystem::render(fitzel::Renderer& renderer) {
    if (!enabled) return;
    for (Trail& t : m_trail)
        if (!t.verts.empty())
            renderer.submit(t.mesh, m_mat, glm::mat4(1.0f), false, false,
                            opacity, /*forceTransparent=*/true);
}
