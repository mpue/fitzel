#include "TramSystem.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/physics/Physics.hpp>
#include <fitzel/render/Renderer.hpp>   // PointLight

#include "SplineGenDetail.hpp"   // Slot, ensureMaterial
#include "TramCar.hpp"
#include "SplineSystem.hpp"
#include "TramTrack.hpp"

using splinegen::detail::Slot;
using namespace tramsim;
using namespace tramcar;

namespace {

glm::vec3 xform(const glm::mat4& m, const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); }

} // namespace

void TramSystem::buildMeshes() {
    for (int c = 0; c < 2; ++c) {
        Slot slot[MatCount];
        m_hitC[c].clear();
        m_hitH[c].clear();
        tramcar::build(c == 0, slot, m_hitC[c], m_hitH[c]);
        m_mesh[c].clear();
        for (int m = 0; m < MatCount; ++m)
            m_mesh[c].push_back(slot[m].empty() ? fitzel::Mesh{}
                                                : fitzel::Mesh::create(slot[m].data));
    }
    m_built = true;
}

void TramSystem::snapshot() {
    m_shown.resize(static_cast<std::size_t>(m_sim.tramCount()));
    for (int i = 0; i < m_sim.tramCount(); ++i) {
        Shown& s = m_shown[static_cast<std::size_t>(i)];
        for (int c = 0; c < kSections; ++c) {
            s.car[c]  = m_sim.carFrame(i, c);
            s.side[c] = m_sim.doorSide(i, c);
        }
        s.door = m_sim.doors(i);
    }
}

void TramSystem::buildDoors() {
    if (m_doors.size() != m_shown.size()) {
        m_doors.clear();
        m_doors.resize(m_shown.size());
    }
    for (std::size_t i = 0; i < m_shown.size(); ++i) {
        const Shown& s = m_shown[i];
        DoorMeshes& d = m_doors[i];
        for (int c = 0; c < kSections; ++c) {
            const bool fresh = d.glass[c].vertexCount() == 0;
            if (!fresh && std::abs(d.door - s.door) < 1e-4f && d.side[c] == s.side[c]) continue;
            Slot glass, frame;
            buildLeaves(isCab(c), s.side[c], s.door, glass, frame);
            if (fresh) {
                d.glass[c] = fitzel::Mesh::create(glass.data);
                d.frame[c] = fitzel::Mesh::create(frame.data);
            } else {
                d.glass[c].update(glass.data.vertices, glass.data.indices);
                d.frame[c].update(frame.data.vertices, frame.data.indices);
            }
            d.side[c] = s.side[c];
        }
        d.door = s.door;
    }
}

void TramSystem::update(const SplineSystem& splines, float dt,
                        std::vector<MaterialDef>& materials,
                        const std::vector<tramsim::Blocker>& blockers,
                        const std::vector<glm::vec3>& people) {
    if (splines.revision() != m_revision) {
        m_revision = splines.revision();
        std::vector<tramsim::Line> lines;
        for (int i = 0; i < static_cast<int>(splines.paths.size()); ++i) {
            const SplineSystem::Path& p = splines.paths[static_cast<std::size_t>(i)];
            if (!p.enabled || p.kind != splinegen::Kind::Rail || p.style.trams <= 0) continue;
            if (splines.line(i).size() < 2) continue;
            tramsim::Line l;
            l.center      = splines.line(i);
            l.onRoad      = splines.onRoad(i);
            l.closed      = p.closed;
            l.tracks      = p.style.tracks;
            l.spacing     = p.style.trackSpacing;
            l.railTopRoad = tramtrack::kRailTop;
            l.railTopOff  = tramtrack::railTopOffRoad(p.style.sleeperHeight, p.style.railHeight);
            l.trams       = p.style.trams;
            l.vmax        = p.style.tramSpeed / 3.6f;
            l.stopEvery   = p.style.stopEvery;
            l.dwell       = p.style.dwell;
            lines.push_back(std::move(l));
        }
        m_sim.setLines(lines);
        if (m_sim.tramCount() > 0) {
            using splinegen::detail::ensureMaterial;
            m_mat[Body]   = ensureMaterial(materials, "Tram Body",   {0.90f, 0.90f, 0.88f}, 0.12f, 0.35f);
            m_mat[Accent] = ensureMaterial(materials, "Tram Accent", {0.72f, 0.08f, 0.07f}, 0.10f, 0.40f);
            m_mat[Glass]  = ensureMaterial(materials, "Tram Glass",  {0.10f, 0.12f, 0.13f}, 0.55f, 0.06f);
            m_mat[Roof]   = ensureMaterial(materials, "Tram Roof",   {0.55f, 0.56f, 0.57f}, 0.05f, 0.60f);
            m_mat[Dark]   = ensureMaterial(materials, "Tram Dark",   {0.07f, 0.07f, 0.08f}, 0.05f, 0.70f);
            m_mat[Floor]  = ensureMaterial(materials, "Tram Floor",  {0.30f, 0.31f, 0.32f}, 0.02f, 0.80f);
            m_mat[Seat]   = ensureMaterial(materials, "Tram Seat",   {0.10f, 0.20f, 0.42f}, 0.0f, 0.90f);
            m_mat[Pole]   = ensureMaterial(materials, "Tram Pole",   {0.95f, 0.72f, 0.05f}, 0.30f, 0.30f);
            m_mat[Light]  = ensureMaterial(materials, "Tram Light",  {0.95f, 0.94f, 0.90f}, 0.0f, 0.50f);
            m_mat[Lining] = ensureMaterial(materials, "Tram Lining", {0.78f, 0.79f, 0.80f}, 0.02f, 0.60f);
            for (MaterialDef& m : materials) {
                if (m.assetId == m_mat[Glass]) {
                    // Tinted panes to look through, both ways.
                    m.alphaMode = AlphaMode::Blend;
                    m.opacity   = 0.35f;
                } else if (m.assetId == m_mat[Light]) {
                    m.emission = {1.0f, 0.93f, 0.80f};
                    m.emissionStrength = 2.5f;
                }
            }
        }
    }
    if (m_sim.tramCount() == 0) {
        m_shown.clear();
        return;
    }
    if (!m_built) buildMeshes();
    // What is drawn this frame is where they stand now; then they drive on.
    snapshot();
    buildDoors();
    m_sim.step(dt, blockers);

    // Someone in an open doorway keeps the doors from closing on them.
    for (int i = 0; i < m_sim.tramCount() && !people.empty(); ++i) {
        const tramsim::Sim::Info in = m_sim.info(i);
        if (in.dwell <= 0.0f || in.door < 0.05f) continue;
        bool held = false;
        for (int c = 0; c < kSections && !held; ++c) {
            const glm::mat4 f = m_sim.carFrame(i, c);
            const float s = static_cast<float>(m_sim.doorSide(i, c));
            float dz[2];
            doorsOf(isCab(c), dz);
            for (float z : dz) {
                const glm::vec3 d = xform(f, {s * kWallIn, kFloor, z});
                for (const glm::vec3& p : people)
                    if (glm::length(glm::vec2(p.x - d.x, p.z - d.z)) < 1.2f && std::abs(p.y - d.y) < 2.0f)
                        held = true;
            }
        }
        if (held) m_sim.holdDoors(i);
    }
}

void TramSystem::forEachDraw(const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                                      const glm::mat4&)>& fn) const {
    if (!m_built) return;
    for (std::size_t i = 0; i < m_shown.size(); ++i) {
        const Shown& s = m_shown[i];
        for (int c = 0; c < kSections; ++c) {
            const glm::mat4& m = s.car[c];
            const std::vector<fitzel::Mesh>& meshes = m_mesh[isCab(c) ? 0 : 1];
            for (int mat = 0; mat < MatCount && mat < static_cast<int>(meshes.size()); ++mat)
                if (meshes[static_cast<std::size_t>(mat)].vertexCount() > 0)
                    fn(meshes[static_cast<std::size_t>(mat)], m_mat[mat], m);
            if (i < m_doors.size()) {
                const DoorMeshes& d = m_doors[i];
                if (d.glass[c].vertexCount() > 0) fn(d.glass[c], m_mat[Glass], m);
                if (d.frame[c].vertexCount() > 0) fn(d.frame[c], m_mat[Body], m);
            }
        }
    }
}

void TramSystem::forEachGlow(const std::function<void(const fitzel::AssetId&, float, float)>& fn) const {
    if (m_sim.tramCount() > 0) fn(m_mat[Light], 2.5f, 0.35f);
}

void TramSystem::collectLights(const glm::vec3& eye, float night,
                               std::vector<fitzel::PointLight>& out, int cap) const {
    if (night <= 0.01f || cap <= 0) return;
    std::vector<std::pair<float, glm::vec3>> near;
    for (const Shown& s : m_shown)
        for (const glm::mat4& m : s.car) {
            const glm::vec3 p = xform(m, {0.0f, kCeiling - 0.35f, 0.0f});
            const float d = glm::length(p - eye);
            if (d < 45.0f) near.emplace_back(d, p);
        }
    std::sort(near.begin(), near.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t k = 0; k < near.size() && static_cast<int>(k) < cap; ++k) {
        fitzel::PointLight l;
        l.position = near[k].second;
        // Faded out towards the edge of the reach, so a light handed on to
        // another car does not pop.
        const float fade = std::clamp((45.0f - near[k].first) / 10.0f, 0.0f, 1.0f);
        l.color = glm::vec3(1.0f, 0.90f, 0.75f) * (2.2f * night * fade);
        l.range = 6.5f;
        out.push_back(l);
    }
}

void TramSystem::dropBodies() {
    m_bodies.clear();
    m_phys = nullptr;
}

void TramSystem::syncBodies(fitzel::PhysicsWorld* physics, float dt) {
    if (!physics || !m_built) { dropBodies(); return; }
    if (physics != m_phys || m_sim.generation() != m_bodyGen) {
        if (physics == m_phys)
            for (const auto& tram : m_bodies)
                for (const CarBodies& b : tram) {
                    physics->removeBody(b.shell);
                    for (std::uint32_t l : b.leaf) physics->removeBody(l);
                }
        m_bodies.clear();
        m_phys = physics;
        m_bodyGen = m_sim.generation();
    }
    const bool fresh = m_bodies.empty();
    m_bodies.resize(static_cast<std::size_t>(m_sim.tramCount()));
    for (int i = 0; i < m_sim.tramCount(); ++i) {
        const float open = m_sim.doors(i);
        for (int c = 0; c < kSections; ++c) {
            CarBodies& b = m_bodies[static_cast<std::size_t>(i)][static_cast<std::size_t>(c)];
            const glm::mat4 f = m_sim.carFrame(i, c);
            const glm::quat q = glm::quat_cast(glm::mat3(f));
            const glm::vec3 p(f[3]);
            const int kind = isCab(c) ? 0 : 1;
            const int side = m_sim.doorSide(i, c);
            if (fresh || b.shell == 0) {
                b.shell = physics->addPlatform(m_hitC[kind].data(), m_hitH[kind].data(),
                                               static_cast<int>(m_hitC[kind].size()), p, q);
            } else {
                physics->setKinematicTarget(b.shell, p, q, dt);
            }
            for (int n = 0; n < 8; ++n) {
                int j;
                float sx, k;
                leafOf(n, j, sx, k);
                const glm::vec3 lp = xform(f, leafAt(isCab(c), j, sx, k,
                                                     sx == static_cast<float>(side) ? open : 0.0f));
                if (fresh || b.leaf[n] == 0) {
                    const glm::vec3 zero(0.0f);
                    b.leaf[n] = physics->addPlatform(&zero, &kLeafHalf, 1, lp, q);
                } else {
                    physics->setKinematicTarget(b.leaf[n], lp, q, dt);
                }
            }
        }
    }
}
