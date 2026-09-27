#include "TownTraffic.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include "CitySystem.hpp"
#include "EditMesh.hpp"
#include "ModelLibrary.hpp"
#include "PrefabSystem.hpp"
#include "SandboxMath.hpp"
#include "SceneGraph.hpp"
#include "VehicleRig.hpp"

#include <fitzel/physics/Physics.hpp>

namespace traffic {

namespace {

using fitzel::AssetId;
using fitzel::Vertex;
constexpr float kPi = 3.14159265358979f;

// --- Placeholder geometry -----------------------------------------------------------

void addQuad(std::vector<Vertex>& v, std::vector<std::uint32_t>& ix, glm::vec3 a, glm::vec3 b,
             glm::vec3 c, glm::vec3 d, glm::vec3 n) {
    const auto base = static_cast<std::uint32_t>(v.size());
    for (const glm::vec3& p : {a, b, c, d}) {
        Vertex x{};
        x.position = p;
        x.normal   = n;
        x.uv       = glm::vec2(p.x + p.z, p.y);
        v.push_back(x);
    }
    ix.insert(ix.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void box(std::vector<Vertex>& v, std::vector<std::uint32_t>& ix, glm::vec3 lo, glm::vec3 hi) {
    const glm::vec3 a = lo, b = hi;
    addQuad(v, ix, {b.x, a.y, a.z}, {b.x, b.y, a.z}, {b.x, b.y, b.z}, {b.x, a.y, b.z}, {1, 0, 0});
    addQuad(v, ix, {a.x, a.y, a.z}, {a.x, a.y, b.z}, {a.x, b.y, b.z}, {a.x, b.y, a.z}, {-1, 0, 0});
    addQuad(v, ix, {a.x, b.y, a.z}, {a.x, b.y, b.z}, {b.x, b.y, b.z}, {b.x, b.y, a.z}, {0, 1, 0});
    addQuad(v, ix, {a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, a.y, b.z}, {a.x, a.y, b.z}, {0, -1, 0});
    addQuad(v, ix, {a.x, a.y, b.z}, {b.x, a.y, b.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}, {0, 0, 1});
    addQuad(v, ix, {a.x, a.y, a.z}, {a.x, b.y, a.z}, {b.x, b.y, a.z}, {b.x, a.y, a.z}, {0, 0, -1});
}

// A wheel: a cylinder across the vehicle (axis along z), both ends capped.
void wheel(std::vector<Vertex>& v, std::vector<std::uint32_t>& ix, glm::vec3 c, float r, float w) {
    const int seg = 8;
    const float z0 = c.z - 0.5f * w, z1 = c.z + 0.5f * w;
    for (int i = 0; i < seg; ++i) {
        const float a0 = 2.0f * kPi * i / seg, a1 = 2.0f * kPi * (i + 1) / seg;
        const glm::vec3 p0(c.x + r * std::cos(a0), c.y + r * std::sin(a0), 0.0f);
        const glm::vec3 p1(c.x + r * std::cos(a1), c.y + r * std::sin(a1), 0.0f);
        const float am = 0.5f * (a0 + a1);
        addQuad(v, ix, {p0.x, p0.y, z0}, {p1.x, p1.y, z0}, {p1.x, p1.y, z1}, {p0.x, p0.y, z1},
                {std::cos(am), std::sin(am), 0.0f});
    }
    for (int side = 0; side < 2; ++side) {
        const float z = side ? z1 : z0;
        const glm::vec3 n(0.0f, 0.0f, side ? 1.0f : -1.0f);
        const auto base = static_cast<std::uint32_t>(v.size());
        for (int i = 0; i < seg; ++i) {
            const float a = 2.0f * kPi * i / seg;
            Vertex x{};
            x.position = {c.x + r * std::cos(a), c.y + r * std::sin(a), z};
            x.normal   = n;
            v.push_back(x);
        }
        for (int i = 1; i + 1 < seg; ++i) {
            if (side) ix.insert(ix.end(), {base, base + static_cast<std::uint32_t>(i),
                                           base + static_cast<std::uint32_t>(i + 1)});
            else      ix.insert(ix.end(), {base, base + static_cast<std::uint32_t>(i + 1),
                                           base + static_cast<std::uint32_t>(i)});
        }
    }
}

// An upright cylinder (a person's body), capped on top.
void column(std::vector<Vertex>& v, std::vector<std::uint32_t>& ix, float r, float y0, float y1) {
    const int seg = 8;
    for (int i = 0; i < seg; ++i) {
        const float a0 = 2.0f * kPi * i / seg, a1 = 2.0f * kPi * (i + 1) / seg;
        const float am = 0.5f * (a0 + a1);
        const glm::vec3 p0(r * std::cos(a0), 0.0f, r * std::sin(a0));
        const glm::vec3 p1(r * std::cos(a1), 0.0f, r * std::sin(a1));
        addQuad(v, ix, {p0.x, y0, p0.z}, {p0.x, y1, p0.z}, {p1.x, y1, p1.z}, {p1.x, y0, p1.z},
                {std::cos(am), 0.0f, std::sin(am)});
    }
    const auto base = static_cast<std::uint32_t>(v.size());
    for (int i = 0; i < seg; ++i) {
        const float a = 2.0f * kPi * i / seg;
        Vertex x{};
        x.position = {r * std::cos(a), y1, r * std::sin(a)};
        x.normal   = {0, 1, 0};
        v.push_back(x);
    }
    for (int i = 1; i + 1 < seg; ++i)
        ix.insert(ix.end(), {base, base + static_cast<std::uint32_t>(i + 1),
                             base + static_cast<std::uint32_t>(i)});
}

void sphere(std::vector<Vertex>& v, std::vector<std::uint32_t>& ix, glm::vec3 c, float r) {
    const int stacks = 5, slices = 8;
    const auto base = static_cast<std::uint32_t>(v.size());
    for (int i = 0; i <= stacks; ++i) {
        const float ph = kPi * i / stacks;
        for (int j = 0; j <= slices; ++j) {
            const float th = 2.0f * kPi * j / slices;
            const glm::vec3 n(std::sin(ph) * std::cos(th), std::cos(ph), std::sin(ph) * std::sin(th));
            Vertex x{};
            x.position = c + r * n;
            x.normal   = n;
            v.push_back(x);
        }
    }
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j) {
            const auto a = base + static_cast<std::uint32_t>(i * (slices + 1) + j);
            const auto b = a + static_cast<std::uint32_t>(slices + 1);
            ix.insert(ix.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
}

AssetId ensure(std::vector<MaterialDef>& mats, const char* part, glm::vec3 albedo, float refl,
               float rough) {
    const std::string name = std::string("City Traffic ") + part;
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = albedo; m.reflectivity = refl; m.roughness = rough;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = albedo;
    md.reflectivity = refl;
    md.roughness    = rough;
    mats.push_back(md);
    return md.assetId;
}

} // namespace

bool TownTraffic::init() {
    m_motion = fitzel::Shader::fromFiles("assets/shaders/trafficmotion.vert",
                                         "assets/shaders/trafficmotion.frag");

    // --- The placeholders, in their own frame: x forward, y up, on the ground ---
    auto part = [](int slot, bool varies) { Part p; p.slot = slot; p.varies = varies; return p; };
    {   // Car: a body, a glass house with a painted roof, four wheels.
        std::vector<Part>& t = m_templates[0];
        Part body = part(Paint0, true), glass = part(Glass, false), tyre = part(Tyre, false);
        box(body.verts, body.idx, {-2.15f, 0.30f, -0.90f}, {2.15f, 0.95f, 0.90f});
        box(body.verts, body.idx, {-1.10f, 1.45f, -0.78f}, {0.80f, 1.52f, 0.78f});
        box(glass.verts, glass.idx, {-1.20f, 0.95f, -0.80f}, {0.90f, 1.45f, 0.80f});
        for (float x : {-1.35f, 1.35f})
            for (float z : {-0.82f, 0.82f}) wheel(tyre.verts, tyre.idx, {x, 0.33f, z}, 0.33f, 0.24f);
        t = {body, glass, tyre};
    }
    {   // Bus: a long body, a band of windows, two axles.
        std::vector<Part>& t = m_templates[1];
        Part body = part(BusBody, false), glass = part(Glass, false), tyre = part(Tyre, false);
        box(body.verts, body.idx, {-6.0f, 0.35f, -1.25f}, {6.0f, 3.10f, 1.25f});
        box(glass.verts, glass.idx, {-5.7f, 1.35f, -1.27f}, {6.02f, 2.60f, 1.27f});
        for (float x : {-4.0f, 3.9f})
            for (float z : {-1.05f, 1.05f}) wheel(tyre.verts, tyre.idx, {x, 0.50f, z}, 0.50f, 0.30f);
        t = {body, glass, tyre};
    }
    {   // Lorry: a cab with its windscreen, a box behind it, a chassis, three axles.
        std::vector<Part>& t = m_templates[2];
        Part cab = part(TruckCab, false), load = part(TruckBox, false), glass = part(Glass, false),
             tyre = part(Tyre, false);
        box(cab.verts, cab.idx, {2.55f, 0.55f, -1.20f}, {4.75f, 3.00f, 1.20f});
        box(glass.verts, glass.idx, {4.73f, 1.90f, -1.10f}, {4.79f, 2.75f, 1.10f});
        box(load.verts, load.idx, {-4.75f, 0.95f, -1.25f}, {2.40f, 3.60f, 1.25f});
        box(tyre.verts, tyre.idx, {-4.60f, 0.55f, -0.90f}, {4.50f, 0.95f, 0.90f});
        for (float x : {-3.9f, -2.6f, 3.6f})
            for (float z : {-1.02f, 1.02f}) wheel(tyre.verts, tyre.idx, {x, 0.50f, z}, 0.50f, 0.30f);
        t = {cab, load, glass, tyre};
    }
    {   // A person: a coat on a column, a head on top.
        std::vector<Part>& t = m_templates[3];
        Part coat = part(Coat0, true), skin = part(Skin, false);
        column(coat.verts, coat.idx, 0.20f, 0.02f, 1.46f);
        sphere(skin.verts, skin.idx, {0.0f, 1.62f, 0.0f}, 0.13f);
        t = {coat, skin};
    }
    return m_motion.isValid();
}

void TownTraffic::ensurePalette(std::vector<MaterialDef>& mats) {
    m_mats[Paint0]   = ensure(mats, "Paint Red",    {0.55f, 0.06f, 0.05f}, 0.30f, 0.30f);
    m_mats[Paint1]   = ensure(mats, "Paint Blue",   {0.06f, 0.16f, 0.42f}, 0.30f, 0.30f);
    m_mats[Paint2]   = ensure(mats, "Paint Silver", {0.55f, 0.56f, 0.58f}, 0.45f, 0.30f);
    m_mats[Paint3]   = ensure(mats, "Paint Black",  {0.04f, 0.04f, 0.05f}, 0.30f, 0.25f);
    m_mats[Paint4]   = ensure(mats, "Paint White",  {0.85f, 0.85f, 0.83f}, 0.20f, 0.35f);
    m_mats[Glass]    = ensure(mats, "Glass",        {0.05f, 0.06f, 0.08f}, 0.30f, 0.10f);
    m_mats[Tyre]     = ensure(mats, "Tyre",         {0.05f, 0.05f, 0.05f}, 0.00f, 0.80f);
    m_mats[BusBody]  = ensure(mats, "Bus",          {0.80f, 0.72f, 0.12f}, 0.15f, 0.40f);
    m_mats[TruckCab] = ensure(mats, "Lorry Cab",    {0.10f, 0.30f, 0.55f}, 0.20f, 0.35f);
    m_mats[TruckBox] = ensure(mats, "Lorry Box",    {0.60f, 0.60f, 0.58f}, 0.05f, 0.60f);
    m_mats[Coat0]    = ensure(mats, "Coat Navy",    {0.08f, 0.10f, 0.20f}, 0.00f, 0.85f);
    m_mats[Coat1]    = ensure(mats, "Coat Red",     {0.45f, 0.08f, 0.07f}, 0.00f, 0.85f);
    m_mats[Coat2]    = ensure(mats, "Coat Beige",   {0.55f, 0.47f, 0.35f}, 0.00f, 0.85f);
    m_mats[Coat3]    = ensure(mats, "Coat Green",   {0.12f, 0.25f, 0.14f}, 0.00f, 0.85f);
    m_mats[Skin]     = ensure(mats, "Skin",         {0.70f, 0.52f, 0.42f}, 0.00f, 0.70f);
}

int TownTraffic::slotOf(const Part& p, int look) const {
    if (!p.varies) return p.slot;
    const int n = p.slot == Paint0 ? 5 : 4;
    return p.slot + ((look % n) + n) % n;
}

void TownTraffic::rebuild(const CitySystem& towns) {
    std::vector<const cityplan::Town*> built;
    for (const CitySystem::Built& b : towns.built()) built.push_back(&b.town);
    m_sim.surfaceAt = surfaceAt;

    // Every town's vehicle prefabs, flattened once; a vehicle of a kind is then
    // dressed in one of its town's prefabs of that kind, or left a placeholder,
    // by weight.
    m_looks.clear();
    m_townLooks.assign(towns.towns.size(), {});
    for (std::size_t t = 0; t < towns.towns.size(); ++t)
        for (const cityplan::VehiclePrefab& vp : towns.towns[t].vehiclePrefabs) {
            if (vp.prefab.empty() || vp.weight <= 0.0f || !findPrefab) continue;
            const prefab::Prefab* p = findPrefab(vp.prefab);
            PrefabLook look;
            if (!p || !flatten(*p, vp.forward, look)) continue;
            look.name   = vp.prefab;
            look.kind   = vp.kind;
            look.weight = vp.weight;
            m_townLooks[t].push_back(static_cast<int>(m_looks.size()));
            m_looks.push_back(std::move(look));
        }
    auto dress = [&](Vehicle& v) {
        if (v.town < 0 || v.town >= static_cast<int>(m_townLooks.size())) return;
        float total = std::max(towns.towns[static_cast<std::size_t>(v.town)].placeholderWeight, 0.0f);
        for (int li : m_townLooks[static_cast<std::size_t>(v.town)])
            if (m_looks[static_cast<std::size_t>(li)].kind == static_cast<int>(v.kind))
                total += m_looks[static_cast<std::size_t>(li)].weight;
        if (total <= 0.0f) return;
        v.rng ^= v.rng << 13; v.rng ^= v.rng >> 17; v.rng ^= v.rng << 5;
        float pick = static_cast<float>(v.rng & 0xffffffU) / 16777215.0f * total;
        for (int li : m_townLooks[static_cast<std::size_t>(v.town)]) {
            const PrefabLook& L = m_looks[static_cast<std::size_t>(li)];
            if (L.kind != static_cast<int>(v.kind)) continue;
            pick -= L.weight;
            if (pick < 0.0f) { v.prefab = li; v.length = L.length; return; }
        }
    };
    m_sim.build(towns.towns, built, 1, dress);
    // Scene objects driving in the traffic come back after a rebuild, where
    // they last were.
    for (Driver& d : m_drivers) d.placed = false;
    placeDrivers();

    // One instance per placeholder-drawn vehicle and per person; which of them
    // are skinned is decided every frame, by what the camera sees.
    m_instances.clear();
    for (const Vehicle& v : m_sim.vehicles())
        if (v.entity < 0)   // a prefab vehicle too: far off it is drawn as a placeholder
            m_instances.push_back({&m_templates[static_cast<std::size_t>(v.kind)], v.look, false});
    for (const Walker& w : m_sim.walkers()) m_instances.push_back({&m_templates[3], w.look, true});
}

void TownTraffic::setView(const glm::vec3& eye, const glm::mat4& viewProj, bool cull) {
    m_eye      = eye;
    m_cull     = cull;
    m_haveView = true;
    // Gribb/Hartmann: the planes are sums and differences of the matrix rows.
    const glm::mat4 t = glm::transpose(viewProj);
    m_planes = {t[3] + t[0], t[3] - t[0], t[3] + t[1], t[3] - t[1], t[3] + t[2], t[3] - t[2]};
    for (glm::vec4& p : m_planes) p /= glm::length(glm::vec3(p));
}

namespace {
// How near a prefab-dressed vehicle is drawn as its prefab, and how near that
// one also casts a shadow and shows in reflections.
constexpr float kPrefabReach = 200.0f, kPrefabDetail = 60.0f;
} // namespace

glm::mat4 TownTraffic::wheelTurn(const Rig& rig, int i, float spin, float steer) {
    const RigWheel& w = rig.wheels[static_cast<std::size_t>(i)];
    if (!w.valid) return glm::mat4(1.0f);
    glm::vec3 rot = w.localRotation;
    rot.x += glm::degrees(spin) * rig.spinSign;
    if (i < 2) rot.y += glm::degrees(steer);   // the fronts steer
    return w.parentWorld * scenegraph::compose(w.localCenter, rot, glm::vec3(1.0f)) * w.restInv;
}

float TownTraffic::steerOf(const Vehicle& v, float wheelbase, float maxSteer) {
    // The race sim turns a car at v / wheelbase * tan(steer); read backwards.
    const float st = std::atan(v.yawRate * wheelbase / std::max(v.v, 1.0f));
    return glm::clamp(st, -maxSteer, maxSteer);
}

bool TownTraffic::asPrefab(const Vehicle& v, const glm::vec3& at) const {
    if (v.prefab < 0 || v.prefab >= static_cast<int>(m_looks.size())) return false;
    const glm::vec3 d = at - m_eye;
    return !m_haveView || glm::dot(d, d) < kPrefabReach * kPrefabReach;
}

bool TownTraffic::inView(const glm::vec3& p, float reach, float radius) const {
    if (!m_haveView) return true;
    const glm::vec3 d = p - m_eye;
    if (glm::dot(d, d) > reach * reach) return false;
    if (!m_cull) return true;
    for (const glm::vec4& pl : m_planes)
        if (glm::dot(glm::vec3(pl), p) + pl.w < -radius) return false;
    return true;
}

// Appends one instance, posed by m, to this frame's slot meshes and to the
// motion mesh.
void TownTraffic::skin(const Instance& in, const glm::mat4& m) {
    const glm::mat3 nm(m);
    for (const Part& p : *in.parts) {
        const std::size_t s = static_cast<std::size_t>(slotOf(p, in.look));
        std::vector<Vertex>& dst = m_verts[s];
        const auto base  = static_cast<std::uint32_t>(dst.size());
        const auto mbase = static_cast<std::uint32_t>(m_motionVerts.size());
        for (std::uint32_t i : p.idx) {
            m_idx[s].push_back(base + i);
            m_motionIdx.push_back(mbase + i);
        }
        for (const Vertex& src : p.verts) {
            Vertex d = src;
            d.position = glm::vec3(m * glm::vec4(src.position, 1.0f));
            d.normal   = glm::normalize(nm * src.normal);
            dst.push_back(d);
            // The motion copy: now in position, a frame ago in the paint slot
            // (trafficmotion.vert reads it there).
            Vertex mv;
            mv.position = d.position;
            mv.paint    = glm::vec4(glm::vec3(in.prev * glm::vec4(src.position, 1.0f)), 1.0f);
            m_motionVerts.push_back(mv);
        }
    }
}

void TownTraffic::update(const CitySystem& towns, float dt, double clock,
                         std::vector<MaterialDef>& materials) {
    // The streets may not be built yet when a town first derives (a project
    // still loading): then there is no surface to drive on, and the network
    // comes out empty. Try again every couple of seconds while that lasts.
    bool wantsCars = false, wantsLife = false;
    for (const cityplan::Rule& r : towns.towns) {
        wantsCars |= r.enabled && r.traffic > 0.0f;
        wantsLife |= r.enabled && (r.traffic > 0.0f || r.people > 0.0f);
    }
    m_retry -= dt;
    const bool starved = m_retry <= 0.0f && ((wantsCars && m_sim.lanes().empty()) ||
                                             (wantsLife && m_instances.empty()));
    if (towns.revision() != m_revision || starved) {
        m_revision = towns.revision();
        m_retry    = 2.0f;
        ensurePalette(materials);
        rebuild(towns);
        m_live.fill(false);
        m_motionLive = false;
        // First frame after a rebuild: no past to move from.
        std::size_t i = 0;
        auto matOf = [](const Pose& p) {
            glm::mat4 m = glm::translate(glm::mat4(1.0f), p.pos);
            m = glm::rotate(m, std::atan2(-p.heading.y, p.heading.x), glm::vec3(0, 1, 0));
            return glm::rotate(m, p.pitch, glm::vec3(0, 0, 1));
        };
        for (const Vehicle& v : m_sim.vehicles())
            if (v.entity < 0) m_instances[i++].prev = matOf(m_sim.pose(v));
        for (const Walker& w : m_sim.walkers()) m_instances[i++].prev = matOf(m_sim.pose(w));
    }
    if (m_playing) placeDrivers();
    if (m_sim.vehicles().empty() && m_sim.walkers().empty()) return;
    m_sim.step(dt, clock);

    // Every pose is worked out (the motion vectors need last frame's for
    // whatever comes into view), only what is in view is skinned. Past these
    // reaches a car is a few pixels long and a person a speck.
    constexpr float kCarReach = 600.0f, kPersonReach = 250.0f;
    for (auto& v : m_verts) v.clear();
    for (auto& ix : m_idx) ix.clear();
    m_motionVerts.clear();
    m_motionIdx.clear();
    std::size_t i = 0;
    auto pose = [&](const Pose& p, float lift, bool tilt, bool skip) {
        Instance& in = m_instances[i++];
        glm::mat4 m = glm::translate(glm::mat4(1.0f), p.pos + glm::vec3(0.0f, lift, 0.0f));
        m = glm::rotate(m, std::atan2(-p.heading.y, p.heading.x), glm::vec3(0, 1, 0));
        if (tilt) m = glm::rotate(m, p.pitch, glm::vec3(0, 0, 1));
        if (!skip &&
            (in.person ? inView(p.pos, kPersonReach, 1.5f) : inView(p.pos, kCarReach, 9.0f)))
            skin(in, m);
        in.prev = m;
    };
    for (const Vehicle& v : m_sim.vehicles())
        if (v.entity < 0) {
            const Pose p = m_sim.pose(v);
            pose(p, 0.0f, true, asPrefab(v, p.pos));
        }
    for (const Walker& w : m_sim.walkers()) {
        const Pose p = m_sim.pose(w);
        pose(p, p.bob, false, false);
    }
    for (std::size_t s = 0; s < SlotCount; ++s) {
        m_live[s] = !m_idx[s].empty();
        if (m_live[s]) m_meshes[s].update(m_verts[s], m_idx[s]);
    }
    m_motionLive = !m_motionIdx.empty();
    if (m_motionLive) m_motionMesh.update(m_motionVerts, m_motionIdx);
}

void TownTraffic::forEachDraw(
    const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&)>& fn) const {
    if (m_instances.empty()) return;
    for (int s = 0; s < SlotCount; ++s)
        if (m_live[static_cast<std::size_t>(s)])
            fn(m_meshes[static_cast<std::size_t>(s)], m_mats[static_cast<std::size_t>(s)]);
}

void TownTraffic::drawMotion(const glm::mat4& viewProj, const glm::mat4& curVP,
                             const glm::mat4& prevVP) {
    if (!m_motionLive || m_instances.empty() || !m_motion.isValid()) return;
    // Depth-tested against the finished opaque scene, like Renderer::renderMotion.
    GLint prevFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &prevFunc);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    m_motion.bind();
    m_motion.setMat4("uViewProj", viewProj);
    m_motion.setMat4("uCurVP", curVP);
    m_motion.setMat4("uPrevVP", prevVP);
    m_motionMesh.draw();
    glDepthMask(GL_TRUE);
    glDepthFunc(static_cast<GLenum>(prevFunc));
}


// --- Vehicle prefabs ----------------------------------------------------------------

bool TownTraffic::flatten(const prefab::Prefab& p, int forward, PrefabLook& out) {
    // The prefab's entities in its own frame, their hierarchy resolved.
    std::vector<Entity> es = p.entities;
    scenegraph::resolve(es);
    // Its nose onto the vehicle frame's +x.
    const float turn = forward == 1 ? 180.0f : forward == 2 ? 90.0f : forward == 3 ? -90.0f : 0.0f;
    const glm::mat4 toNose = glm::rotate(glm::mat4(1.0f), glm::radians(turn - 90.0f), glm::vec3(0, 1, 0));
    glm::vec3 lo(1e30f), hi(-1e30f);
    auto grow = [&](const glm::mat4& m, const glm::vec3& a, const glm::vec3& b) {
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 c((k & 1) ? b.x : a.x, (k & 2) ? b.y : a.y, (k & 4) ? b.z : a.z);
            const glm::vec3 w = glm::vec3(toNose * m * glm::vec4(c, 1.0f));
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    };
    // The vehicle rig, if the prefab has one: which entities are its wheels
    // (their ids as saved, else found again as "Make drivable" finds them).
    Rig rig;
    int wheelOf[4] = {-1, -1, -1, -1};
    auto byId = [&](int id) -> const Entity* {
        for (const Entity& x : es) if (x.id == id) return &x;
        return nullptr;
    };
    for (const Entity& e : es) {
        const auto* vc = e.components.get<VehicleComponent>();
        if (!vc) continue;
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            const Entity* w = byId(vc->wheelId[i]);
            ok = ok && w && w->parent >= 0;
            wheelOf[i] = w ? w->id : -1;
        }
        if (!ok) vehiclerig::guessWheels(es, e.id, vc->forward, wheelOf);
        rig.radius    = std::max(vc->wheelRadius, 0.05f);
        rig.wheelbase = std::max(vc->frontZ - vc->rearZ, 0.5f);
        rig.maxSteer  = glm::radians(vc->maxSteerDeg);
        rig.spinSign  = vc->forward == 1 ? -1.0f : 1.0f;
        for (int i = 0; i < 4; ++i) {
            const Entity* w = byId(wheelOf[i]);
            if (!w) continue;
            RigWheel& rw = rig.wheels[static_cast<std::size_t>(i)];
            const Entity* par = byId(w->parent);
            rw.parentWorld   = par ? scenegraph::compose(par->center, par->rotation, glm::vec3(1.0f))
                                   : glm::mat4(1.0f);
            rw.localCenter   = w->localCenter;
            rw.localRotation = w->localRotation;
            rw.restInv       = glm::inverse(scenegraph::compose(w->center, w->rotation, glm::vec3(1.0f)));
            rw.valid         = true;
            rig.any          = true;
        }
        break;
    }
    // The wheel an entity turns with: itself or its nearest ancestor that is one.
    auto wheelIndex = [&](const Entity& e) {
        for (const Entity* x = &e; x; x = x->parent >= 0 ? byId(x->parent) : nullptr)
            for (int i = 0; i < 4; ++i)
                if (wheelOf[i] >= 0 && x->id == wheelOf[i]) return i;
        return -1;
    };

    std::vector<PrefabPart> parts;
    for (const Entity& e : es) {
        if (!e.activeInHierarchy) continue;
        const int wi = rig.any ? wheelIndex(e) : -1;
        if (const auto* mc = e.components.get<ModelComponent>(); mc && models) {
            LoadedModel* lm = models->byId(mc->modelId);
            if (!lm) continue;
            // As SceneSubmit draws a model: filling centre +/- half.
            const glm::vec3 sz = glm::max(lm->size(), glm::vec3(1e-4f));
            const glm::mat4 m = scenegraph::compose(e.center, e.rotation, (e.half * 2.0f) / sz) *
                                glm::translate(glm::mat4(1.0f), -lm->center());
            for (std::size_t i = 0; i < lm->meshes.size(); ++i)
                parts.push_back({&lm->meshes[i], lm->primMaterialId[i], toNose * m, wi, m});
            grow(m, lm->boundsMin, lm->boundsMax);
        } else if (const auto* meshC = e.components.get<MeshComponent>(); meshC && meshCache) {
            // Modelled in the editor: uploaded by the shared cache under an id
            // of its own (one per prefab and entity), dressed as SceneSubmit does.
            const int cacheId = 0x70000000 + static_cast<int>(m_looks.size()) * 4096 + e.id;
            const glm::mat4 m = scenegraph::compose(e.center, e.rotation,
                                                    editmesh::fitScale(meshC->mesh, e.half));
            const auto* matC = e.components.get<MaterialComponent>();
            const fitzel::AssetId own = matC ? matC->material : fitzel::AssetId{};
            for (const EditMeshCache::Sub& sub : meshCache->submeshes(cacheId, meshC->revision, meshC->mesh))
                parts.push_back({&sub.mesh, sub.material.valid() ? sub.material : own, toNose * m, wi, m});
            glm::vec3 mlo, mhi;
            meshC->mesh.bounds(mlo, mhi);
            grow(m, mlo, mhi);
        }
    }
    if (parts.empty() || lo.x > hi.x) return false;
    // Stand it on y = 0, centred in x and z.
    const glm::mat4 centre = glm::translate(
        glm::mat4(1.0f), glm::vec3(-0.5f * (lo.x + hi.x), -lo.y, -0.5f * (lo.z + hi.z)));
    for (PrefabPart& part : parts) part.local = centre * part.local;
    out.frame  = centre * toNose;
    out.rig    = rig;
    out.parts  = std::move(parts);
    out.length = std::max(hi.x - lo.x, 1.0f);
    return true;
}

void TownTraffic::forEachPrefabDraw(
    const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&, const glm::mat4&, bool)>& fn) {
    for (const Vehicle& v : m_sim.vehicles()) {
        if (v.entity >= 0) continue;
        const Pose p = m_sim.pose(v);
        if (!asPrefab(v, p.pos) || !inView(p.pos, kPrefabReach, 0.6f * v.length + 2.0f)) continue;
        const bool detail = !m_haveView || glm::length(p.pos - m_eye) < kPrefabDetail;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), p.pos);
        m = glm::rotate(m, std::atan2(-p.heading.y, p.heading.x), glm::vec3(0, 1, 0));
        m = glm::rotate(m, p.pitch, glm::vec3(0, 0, 1));
        const PrefabLook& look = m_looks[static_cast<std::size_t>(v.prefab)];
        std::array<glm::mat4, 4> turned;
        if (look.rig.any) {
            const float spin  = v.odo / look.rig.radius;
            const float steer = steerOf(v, look.rig.wheelbase, look.rig.maxSteer);
            for (int i = 0; i < 4; ++i)
                turned[static_cast<std::size_t>(i)] = look.frame * wheelTurn(look.rig, i, spin, steer);
        }
        for (const PrefabPart& part : look.parts)
            fn(*part.mesh, part.material,
               part.wheel >= 0 && look.rig.any
                   ? m * turned[static_cast<std::size_t>(part.wheel)] * part.rest
                   : m * part.local,
               detail);
    }
}

// --- Scene objects driving in the traffic ------------------------------------------

namespace {
// The heading an entity's nose points along (XZ), for a nose on `forward`.
glm::vec2 noseOf(const Entity& e, int forward) {
    const glm::mat4 r = scenegraph::compose(glm::vec3(0.0f), e.rotation, glm::vec3(1.0f));
    const glm::vec3 axis = forward == 1 ? glm::vec3(0, 0, -1) : forward == 2 ? glm::vec3(1, 0, 0)
                         : forward == 3 ? glm::vec3(-1, 0, 0) : glm::vec3(0, 0, 1);
    const glm::vec3 n = glm::vec3(r * glm::vec4(axis, 0.0f));
    const glm::vec2 h(n.x, n.z);
    return glm::length(h) > 1e-4f ? glm::normalize(h) : glm::vec2(0.0f, 1.0f);
}
} // namespace

void TownTraffic::beginPlay(std::vector<Entity>& entities, fitzel::PhysicsWorld* physics) {
    m_drivers.clear();
    m_playing = true;
    for (const Entity& e : entities) {
        const auto* dc = e.components.get<TrafficDriverComponent>();
        if (!dc || !e.activeInHierarchy) continue;
        Driver d;
        d.entity  = e.id;
        d.kind    = static_cast<traffic::Kind>(std::clamp(dc->kind, 0, 2));
        d.forward = std::clamp(dc->forward, 0, 3);
        d.vmax    = std::max(dc->topSpeed, 1.0f) / 3.6f;
        // Its length along the nose, from the box the entity fills.
        d.length  = 2.0f * ((d.forward >= 2) ? e.half.x : e.half.z);
        if (e.type == EntityType::Empty || d.length < 1.0f) d.length = traffic::kindLength(d.kind);
        d.ride    = dc->ride >= 0.0f ? dc->ride : (e.type == EntityType::Empty ? 0.0f : e.half.y);
        d.pos     = {e.center.x, e.center.z};
        d.heading = noseOf(e, d.forward);
        d.name    = e.name;
        // A vehicle rig: its wheels turn and steer, and it stands on them.
        if (const auto* vc = e.components.get<VehicleComponent>()) {
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
                const Entity* w = nullptr;
                for (const Entity& x : entities) if (x.id == vc->wheelId[i]) { w = &x; break; }
                ok = ok && w;
                d.wheel[i] = w ? w->id : -1;
            }
            if (!ok) vehiclerig::guessWheels(entities, e.id, vc->forward, d.wheel);
            for (int i = 0; i < 4; ++i)
                for (const Entity& x : entities)
                    if (x.id == d.wheel[i]) { d.wheelRest[i] = x.localRotation; break; }
            d.wheelR    = std::max(vc->wheelRadius, 0.05f);
            d.wheelbase = std::max(vc->frontZ - vc->rearZ, 0.5f);
            d.maxSteer  = glm::radians(vc->maxSteerDeg);
            d.spinSign  = vc->forward == 1 ? -1.0f : 1.0f;
            if (dc->ride < 0.0f) d.ride = d.wheelR - vc->wheelY;   // as the race sim seats it
        }
        // Placed now if the towns' streets are there already, else as soon as
        // they are (a game started straight into Play derives them after this).
        if (physics && dc->collider)
            d.body = physics->addKinematicBox(glm::max(e.half, glm::vec3(0.2f)), e.center,
                                              glm::quat(glm::radians(e.rotation)));
        m_drivers.push_back(d);
    }
    placeDrivers();
}

void TownTraffic::placeDrivers() {
    if (m_sim.lanes().empty()) return;
    for (Driver& d : m_drivers) {
        if (d.placed) continue;
        d.placed = m_sim.addDriver(d.entity, d.pos, d.heading, d.kind, d.length, d.vmax);
        if (!d.placed) {
            std::fprintf(stderr, "[Fitzel] traffic driver '%s': no lane within 60 m\n", d.name.c_str());
            d.placed = true;   // said once; it stays where it was authored
        }
    }
}

void TownTraffic::playTick(std::vector<Entity>& entities, fitzel::PhysicsWorld* physics, float dt,
                           const std::function<void(Entity&, const glm::vec3&, const glm::vec3&)>& place) {
    if (!m_playing || m_drivers.empty()) return;
    for (const Vehicle& v : m_sim.vehicles()) {
        if (v.entity < 0) continue;
        Driver* d = nullptr;
        for (Driver& x : m_drivers) if (x.entity == v.entity) { d = &x; break; }
        Entity* e = nullptr;
        for (Entity& x : entities) if (x.id == v.entity) { e = &x; break; }
        if (!d || !e) continue;
        const Pose p = m_sim.pose(v);
        d->pos     = {p.pos.x, p.pos.z};
        d->heading = p.heading;
        // Heading and nose-up in the scene's attitude convention (see
        // attitudeEuler): nose-up is negative pitch for a +Z nose, and a nose
        // on x pitches about z instead.
        const float H = glm::degrees(std::atan2(p.heading.x, p.heading.y));
        const float up = glm::degrees(p.pitch);
        glm::vec3 rot;
        switch (d->forward) {
            case 1:  rot = attitudeEuler(H - 180.0f, up, 0.0f); break;
            case 2:  rot = attitudeEuler(H - 90.0f, 0.0f, up); break;
            case 3:  rot = attitudeEuler(H + 90.0f, 0.0f, -up); break;
            default: rot = attitudeEuler(H, -up, 0.0f); break;
        }
        const glm::vec3 pos = p.pos + glm::vec3(0.0f, d->ride, 0.0f);
        place(*e, pos, rot);
        const float spin  = v.odo / d->wheelR;
        const float steer = steerOf(v, d->wheelbase, d->maxSteer);
        for (int i = 0; i < 4; ++i) {
            if (d->wheel[i] < 0) continue;
            for (Entity& w : entities)
                if (w.id == d->wheel[i]) {
                    glm::vec3 r = d->wheelRest[i];
                    r.x += glm::degrees(spin) * d->spinSign;
                    if (i < 2) r.y += glm::degrees(steer);
                    w.localRotation = r;
                    break;
                }
        }
        if (physics && d->body)
            physics->setKinematicTarget(d->body, pos, glm::quat(glm::radians(rot)), std::max(dt, 1e-3f));
    }
}

void TownTraffic::endPlay() {
    m_sim.removeDrivers();
    m_drivers.clear();
    m_playing = false;
}

} // namespace traffic

// --- The component (global, like every other) -------------------------------------

const std::vector<Property>& TrafficDriverComponent::props() const {
    static const std::vector<Property> p = [] {
        using T = TrafficDriverComponent;
        std::vector<Property> v;
        auto add = [&](const char* label, const char* key, PropKind kind,
                       std::function<void*(void*)> field) -> Property& {
            Property q;
            q.label = label; q.key = key; q.kind = kind; q.field = std::move(field);
            v.push_back(std::move(q));
            return v.back();
        };
        add("Kind", "kind", PropKind::EnumInt,
            [](void* o) -> void* { return &static_cast<T*>(o)->kind; })
            .enumLabels = {"Car", "Bus (calls at stops)", "Lorry"};
        {
            Property& q = add("Top speed", "topSpeed", PropKind::Float,
                              [](void* o) -> void* { return &static_cast<T*>(o)->topSpeed; });
            q.min = 5.0f; q.max = 130.0f; q.speed = 1.0f; q.fmt = "%.0f km/h";
        }
        add("Nose points", "forward", PropKind::EnumInt,
            [](void* o) -> void* { return &static_cast<T*>(o)->forward; })
            .enumLabels = {"+Z", "-Z", "+X", "-X"};
        {
            Property& q = add("Ride height", "ride", PropKind::Float,
                              [](void* o) -> void* { return &static_cast<T*>(o)->ride; });
            q.min = -1.0f; q.max = 5.0f; q.speed = 0.02f; q.fmt = "%.2f m (<0 = auto)";
        }
        add("Collider", "collider", PropKind::Bool,
            [](void* o) -> void* { return &static_cast<T*>(o)->collider; });
        return v;
    }();
    return p;
}

namespace {
struct RegisterDriver {
    RegisterDriver() {
        components::registerType({"trafficDriver", "Traffic driver",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<TrafficDriverComponent>()); }});
    }
} g_registerDriver;
} // namespace
