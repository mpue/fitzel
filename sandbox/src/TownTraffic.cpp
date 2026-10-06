#include "TownTraffic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include "CitySystem.hpp"
#include "EditMesh.hpp"
#include "Modifiers.hpp"
#include "ModelLibrary.hpp"
#include "PrefabSystem.hpp"
#include "SandboxMath.hpp"
#include "SceneGraph.hpp"
#include "VehicleRig.hpp"
#include "WalkPace.hpp"

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
               float rough, glm::vec3 emission = glm::vec3(0.0f), float strength = 0.0f) {
    const std::string name = std::string("City Traffic ") + part;
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = albedo; m.reflectivity = refl; m.roughness = rough;
        m.emission = emission; m.emissionStrength = strength;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = albedo;
    md.reflectivity = refl;
    md.roughness    = rough;
    md.emission         = emission;
    md.emissionStrength = strength;
    mats.push_back(md);
    return md.assetId;
}

} // namespace

bool TownTraffic::init() {
    m_motion = fitzel::Shader::fromFiles("assets/shaders/trafficmotion.vert",
                                         "assets/shaders/trafficmotion.frag");

    // --- The placeholders, in their own frame: x forward, y up, on the ground ---
    auto part = [](int slot, bool varies) { Part p; p.slot = slot; p.varies = varies; return p; };
    // A vehicle's lamps, both sides: headlights and indicators on the front face
    // at x = `front`, tail lights and indicators on the back at x = `rear` (x
    // forward, +z the vehicle's right). Each a flat box a hair proud of the body;
    // `hy`/`ty` the heights of head and tail lamps, `zi`..`zo` how far out they
    // reach -- the indicators beyond them at the front, above them at the back.
    auto addLamps = [](std::vector<Part>& t, float front, float rear, float hy, float ty,
                       float zi, float zo) {
        auto lamp = [&](int kind, int slot, glm::vec3 lo, glm::vec3 hi) {
            Part p;
            p.slot = slot;
            p.lamp = kind;
            box(p.verts, p.idx, glm::min(lo, hi), glm::max(lo, hi));
            t.push_back(std::move(p));
        };
        for (float s : {-1.0f, 1.0f}) {
            const int blink = s < 0.0f ? BlinkLeft : BlinkRight;
            lamp(HeadLamp, LampHead, {front - 0.012f, hy, s * zi}, {front + 0.022f, hy + 0.12f, s * zo});
            lamp(blink, LampOff, {front - 0.012f, hy, s * zo}, {front + 0.022f, hy + 0.12f, s * (zo + 0.09f)});
            lamp(TailLamp, LampTail, {rear + 0.012f, ty, s * zi}, {rear - 0.022f, ty + 0.12f, s * zo});
            lamp(blink, LampOff, {rear + 0.012f, ty + 0.14f, s * zi}, {rear - 0.022f, ty + 0.21f, s * zo});
        }
    };
    {   // Car: a body, a glass house with a painted roof, four wheels.
        std::vector<Part>& t = m_templates[0];
        Part body = part(Paint0, true), glass = part(Glass, false), tyre = part(Tyre, false);
        box(body.verts, body.idx, {-2.15f, 0.30f, -0.90f}, {2.15f, 0.95f, 0.90f});
        box(body.verts, body.idx, {-1.10f, 1.45f, -0.78f}, {0.80f, 1.52f, 0.78f});
        box(glass.verts, glass.idx, {-1.20f, 0.95f, -0.80f}, {0.90f, 1.45f, 0.80f});
        for (float x : {-1.35f, 1.35f})
            for (float z : {-0.82f, 0.82f}) wheel(tyre.verts, tyre.idx, {x, 0.33f, z}, 0.33f, 0.24f);
        t = {body, glass, tyre};
        addLamps(t, 2.15f, -2.15f, 0.62f, 0.66f, 0.50f, 0.78f);
    }
    {   // Bus: a long body, a band of windows, two axles.
        std::vector<Part>& t = m_templates[1];
        Part body = part(BusBody, false), glass = part(Glass, false), tyre = part(Tyre, false);
        box(body.verts, body.idx, {-6.0f, 0.35f, -1.25f}, {6.0f, 3.10f, 1.25f});
        box(glass.verts, glass.idx, {-5.7f, 1.35f, -1.27f}, {6.02f, 2.60f, 1.27f});
        for (float x : {-4.0f, 3.9f})
            for (float z : {-1.05f, 1.05f}) wheel(tyre.verts, tyre.idx, {x, 0.50f, z}, 0.50f, 0.30f);
        t = {body, glass, tyre};
        addLamps(t, 6.0f, -6.0f, 0.58f, 0.75f, 0.72f, 1.08f);
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
        addLamps(t, 4.75f, -4.75f, 0.78f, 1.05f, 0.70f, 1.06f);
    }
    {   // A prefab vehicle's lamp: one unit box, placed and sized per lamp.
        std::vector<Vertex> v;
        std::vector<std::uint32_t> ix;
        box(v, ix, glm::vec3(-0.5f), glm::vec3(0.5f));
        m_lampCube = fitzel::Mesh::create(v, ix);
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
    // The lamps. Headlights and tail lights glow at night only (forEachGlow);
    // brake lights and indicators at their full strength whenever they are on.
    m_mats[LampHead]  = ensure(mats, "Lamp Head",  {0.85f, 0.86f, 0.84f}, 0.40f, 0.10f,
                               {1.00f, 0.95f, 0.85f}, 0.0f);
    m_mats[LampTail]  = ensure(mats, "Lamp Tail",  {0.38f, 0.03f, 0.03f}, 0.30f, 0.15f,
                               {1.00f, 0.06f, 0.04f}, 0.0f);
    m_mats[LampBrake] = ensure(mats, "Lamp Brake", {0.55f, 0.05f, 0.04f}, 0.30f, 0.15f,
                               {1.00f, 0.06f, 0.04f}, 7.0f);
    m_mats[LampBlink] = ensure(mats, "Lamp Indicator", {0.70f, 0.40f, 0.05f}, 0.30f, 0.15f,
                               {1.00f, 0.50f, 0.04f}, 7.0f);
    m_mats[LampOff]   = ensure(mats, "Lamp Off",   {0.45f, 0.30f, 0.12f}, 0.30f, 0.15f);
}

void TownTraffic::forEachGlow(const std::function<void(const fitzel::AssetId&, float, float)>& fn) const {
    if (m_mats[LampHead].valid()) fn(m_mats[LampHead], 6.0f, 0.0f);
    if (m_mats[LampTail].valid()) fn(m_mats[LampTail], 2.5f, 0.0f);
}

std::uint8_t TownTraffic::lampsOf(const Vehicle& v, float dt) {
    std::uint8_t out = 0;
    // Braking: slowing harder than coasting, or standing (the foot on the brake
    // at a light). Held on a little, so a car easing off does not flicker it.
    glm::vec2& b = m_brake[v.uid];   // x: speed last frame, y: seconds left on
    const float acc = dt > 1e-4f ? (v.v - b.x) / dt : 0.0f;
    b.x = v.v;
    if (acc < -0.7f || v.v < 0.5f) b.y = 0.6f;
    else b.y -= dt;
    if (b.y > 0.0f) out |= Braking;
    // Indicating: the next lane bends away from this one, and the turn is
    // close (or under way). Right turns the heading clockwise seen from above.
    const std::vector<Lane>& lanes = m_sim.lanes();
    if (v.next >= 0 && v.lane >= 0 && v.lane < static_cast<int>(lanes.size()) &&
        v.next < static_cast<int>(lanes.size())) {
        const Lane& a = lanes[static_cast<std::size_t>(v.lane)];
        const Lane& n = lanes[static_cast<std::size_t>(v.next)];
        const float c = a.dir.x * n.dir.y - a.dir.y * n.dir.x;
        const bool soon = v.turning || a.len - v.s < 30.0f;
        if (soon && std::abs(c) > 0.3f && std::fmod(m_clock * 1.5, 1.0) < 0.5)
            out |= c > 0.0f ? RightOn : LeftOn;
    }
    return out;
}

int TownTraffic::slotOf(const Part& p, int look, std::uint8_t lamps) const {
    switch (p.lamp) {
        case HeadLamp:   return LampHead;
        case TailLamp:   return (lamps & Braking) ? LampBrake : LampTail;
        case BlinkLeft:  return (lamps & LeftOn) ? LampBlink : LampOff;
        case BlinkRight: return (lamps & RightOn) ? LampBlink : LampOff;
        default: break;
    }
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
    // Every town's person prefabs, flattened once (the walks skinned for an
    // earlier build are reused), and each person dressed in one of their town's,
    // or left a figure, by weight.
    m_personLooks.clear();
    m_townPeople.assign(towns.towns.size(), {});
    {
        std::map<std::string, std::shared_ptr<Walk>> used;
        std::map<std::string, int>                   byKey;
        for (std::size_t t = 0; t < towns.towns.size(); ++t)
            for (const cityplan::PersonPrefab& pp : towns.towns[t].personPrefabs) {
                if (pp.prefab.empty() || pp.weight <= 0.0f || !findPrefab) continue;
                const std::string key = pp.prefab + "#" + std::to_string(pp.forward);
                int li = -1;
                if (const auto it = byKey.find(key); it != byKey.end()) {
                    li = it->second;
                } else {
                    const prefab::Prefab* p = findPrefab(pp.prefab);
                    PersonLook look;
                    if (!p || !flattenPerson(*p, pp.forward, look, used)) continue;
                    li = static_cast<int>(m_personLooks.size());
                    m_personLooks.push_back(std::move(look));
                    byKey[key] = li;
                }
                m_townPeople[t].push_back({li, pp.weight});
            }
        m_walks = std::move(used);   // the walks no town wears any more go
    }
    auto dressWalker = [&](Walker& w) {
        if (w.town < 0 || w.town >= static_cast<int>(m_townPeople.size())) return;
        const std::vector<PersonChoice>& choices = m_townPeople[static_cast<std::size_t>(w.town)];
        if (choices.empty()) return;
        float total = std::max(towns.towns[static_cast<std::size_t>(w.town)].personPlaceholderWeight, 0.0f);
        for (const PersonChoice& c : choices) total += c.weight;
        if (total <= 0.0f) return;
        w.rng ^= w.rng << 13; w.rng ^= w.rng >> 17; w.rng ^= w.rng << 5;
        float pick = static_cast<float>(w.rng & 0xffffffU) / 16777215.0f * total;
        for (const PersonChoice& c : choices) {
            pick -= c.weight;
            if (pick < 0.0f) { w.prefab = c.look; return; }
        }
    };
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
    m_sim.build(towns.towns, built, 1, dress, dressWalker);
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
// The same for people -- smaller, and many more of them: at most this many of
// the nearest are drawn as their prefab, the rest as the figures.
constexpr float kPersonPrefabReach = 60.0f, kPersonDetail = 25.0f;
constexpr int   kPersonPrefabCap   = 120;
// How densely a walk is skinned: poses per second of clip (one stride pair of
// f_casual_walk1 is 1.26 s: 63 poses, 0.76 MB each welded).
constexpr float kWalkPosesPerSecond = 50.0f;
constexpr int   kWalkPosesMin = 16, kWalkPosesMax = 72;
constexpr float kTwoPi = 6.2831853f;

// A vehicle's frame for its pose: x along its nose, y up, standing on the road.
glm::mat4 frameOf(const Pose& p) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), p.pos);
    m = glm::rotate(m, std::atan2(-p.heading.y, p.heading.x), glm::vec3(0, 1, 0));
    return glm::rotate(m, p.pitch, glm::vec3(0, 0, 1));
}
} // namespace

glm::vec3 TownTraffic::halfOf(const Vehicle& v) const {
    if (v.prefab >= 0 && v.prefab < static_cast<int>(m_looks.size()))
        return 0.5f * m_looks[static_cast<std::size_t>(v.prefab)].size;
    // The placeholders' boxes (see init).
    switch (v.kind) {
        case Kind::Bus:   return {6.0f, 1.55f, 1.25f};
        case Kind::Truck: return {4.75f, 1.8f, 1.25f};
        default:          return {2.15f, 0.76f, 0.9f};
    }
}

glm::mat4 TownTraffic::wheelTurn(const Rig& rig, int i, float spin, float steer) {
    const RigWheel& w = rig.wheels[static_cast<std::size_t>(i)];
    if (!w.valid) return glm::mat4(1.0f);
    const glm::vec3 rot = vehiclerig::wheelLocalRotation(w.localRotation, w.turn,
                                                         spin * rig.spinSign,
                                                         i < 2 ? steer : 0.0f);   // fronts steer
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
        const std::size_t s = static_cast<std::size_t>(slotOf(p, in.look, in.lamps));
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

void TownTraffic::advance(float dt, double clock) {
    m_clock = clock;
    if (m_playing) placeDrivers();
    // Out of Play nothing else is in the street but the trams (in Play,
    // playTick hands them over with the wrecks and the player's car).
    else m_sim.setObstacles(m_trams);
    m_sim.step(dt, clock);
}

void TownTraffic::update(const CitySystem& towns, float dt, std::vector<MaterialDef>& materials) {
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
        m_brake.clear();
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
    if (m_sim.vehicles().empty() && m_sim.walkers().empty()) return;

    // Every pose is worked out (the motion vectors need last frame's for
    // whatever comes into view), only what is in view is skinned. Past these
    // reaches a car is a few pixels long and a person a speck.
    constexpr float kCarReach = 600.0f, kPersonReach = 250.0f;
    for (auto& v : m_verts) v.clear();
    for (auto& ix : m_idx) ix.clear();
    m_motionVerts.clear();
    m_motionIdx.clear();
    std::size_t i = 0;
    auto pose = [&](const Pose& p, float lift, bool tilt, bool skip, std::uint8_t lamps = 0) {
        Instance& in = m_instances[i++];
        in.lamps = lamps;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), p.pos + glm::vec3(0.0f, lift, 0.0f));
        m = glm::rotate(m, std::atan2(-p.heading.y, p.heading.x), glm::vec3(0, 1, 0));
        if (tilt) m = glm::rotate(m, p.pitch, glm::vec3(0, 0, 1));
        if (!skip &&
            (in.person ? inView(p.pos, kPersonReach, 1.5f) : inView(p.pos, kCarReach, 9.0f)))
            skin(in, m);
        in.prev = m;
    };
    m_vehLamps.assign(m_sim.vehicles().size(), 0);
    for (std::size_t k = 0; k < m_sim.vehicles().size(); ++k) {
        const Vehicle& v = m_sim.vehicles()[k];
        if (v.entity >= 0) continue;
        const Pose p = m_sim.pose(v);
        m_vehLamps[k] = lampsOf(v, dt);
        pose(p, 0.0f, true, asPrefab(v, p.pos), m_vehLamps[k]);
    }
    // Which people are drawn as their prefab this frame: the nearest in view.
    m_personNear.assign(m_sim.walkers().size(), 0);
    {
        std::vector<std::pair<float, std::size_t>> near;
        const std::vector<Walker>& ws = m_sim.walkers();
        for (std::size_t k = 0; k < ws.size(); ++k) {
            const Walker& w = ws[k];
            if (w.prefab < 0 || w.prefab >= static_cast<int>(m_personLooks.size())) continue;
            const glm::vec3 at = m_sim.pose(w).pos;
            if (!inView(at, kPersonPrefabReach, 1.5f)) continue;
            const glm::vec3 d = at - m_eye;
            near.push_back({m_haveView ? glm::dot(d, d) : 0.0f, k});
        }
        if (static_cast<int>(near.size()) > kPersonPrefabCap) {
            std::nth_element(near.begin(), near.begin() + kPersonPrefabCap, near.end());
            near.resize(static_cast<std::size_t>(kPersonPrefabCap));
        }
        for (const auto& n : near) m_personNear[n.second] = 1;
    }
    for (std::size_t k = 0; k < m_sim.walkers().size(); ++k) {
        const Pose p = m_sim.pose(m_sim.walkers()[k]);
        pose(p, p.bob, false, m_personNear[k] != 0);
    }
    // Wrecks, where playTick last found their bodies; a prefab one near
    // enough is drawn by forEachPrefabDraw instead.
    for (Wreck& w : m_wrecks) {
        const glm::vec3 at(w.now[3]);
        const bool asLook = w.prefab >= 0 && w.prefab < static_cast<int>(m_looks.size()) &&
                            (!m_haveView || glm::distance(at, m_eye) < kPrefabReach);
        if (!asLook && inView(at, kCarReach, 9.0f)) skin(w.inst, w.now);
        w.inst.prev = w.now;
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
    // Restored to the frame's convention (GL_LESS) rather than read back: a glGet
    // waits for everything queued so far under the driver's threaded
    // optimisation, and one in the middle of a frame kept the CPU from ever
    // getting ahead of the GPU.
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
    glDepthFunc(GL_LESS);
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
            rw.turn          = vc->wheelTurn[i];
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
    std::vector<std::pair<const LoadedModel*, glm::mat4>> bodies;   // for the lamps (below)
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
            if (wi < 0) bodies.emplace_back(lm, toNose * m);
        } else if (const auto* meshC = e.components.get<MeshComponent>(); meshC && meshCache) {
            // Modelled in the editor: uploaded by the shared cache under an id
            // of its own (one per prefab and entity), dressed as SceneSubmit does.
            const int cacheId = 0x70000000 + static_cast<int>(m_looks.size()) * 4096 + e.id;
            // What it shows: its modifier stack's result, if it has one.
            const modifiers::Shown sh =
                modifiers::shown(cacheId, *meshC, e.components.get<ModifierStackComponent>());
            const glm::mat4 m = scenegraph::compose(e.center, e.rotation,
                                                    editmesh::fitScale(*sh.mesh, e.half));
            const auto* matC = e.components.get<MaterialComponent>();
            const fitzel::AssetId own = matC ? matC->material : fitzel::AssetId{};
            for (const EditMeshCache::Sub& sub : meshCache->submeshes(cacheId, sh.revision, *sh.mesh))
                parts.push_back({&sub.mesh, sub.material.valid() ? sub.material : own, toNose * m, wi, m});
            glm::vec3 mlo, mhi;
            sh.mesh->bounds(mlo, mhi);
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
    out.size   = glm::max(hi - lo, glm::vec3(0.4f));

    // Its lamps: at a lamp's height, a third of its width out from the middle,
    // ON the body -- the frontmost and backmost of its own points there, so a
    // rounded nose gets its headlights on its curve, not floating in front of it.
    {
        const glm::vec3 sz = out.size;
        const float y = glm::clamp(0.40f * sz.y, 0.45f, 1.1f);
        const float z = 0.34f * sz.z;
        float fx = -1e30f, rx = 1e30f;
        for (const auto& [lm, m] : bodies) {
            const glm::mat4 toFrame = centre * m;
            for (const glm::vec3& q : lm->hullPoints) {
                const glm::vec3 w = glm::vec3(toFrame * glm::vec4(q, 1.0f));
                if (std::abs(w.y - y) > 0.18f || std::abs(std::abs(w.z) - z) > 0.25f) continue;
                fx = std::max(fx, w.x);
                rx = std::min(rx, w.x);
            }
        }
        out.lampFront = {fx > -1e29f ? fx : 0.5f * sz.x, y, z};
        out.lampRear  = {rx < 1e29f ? rx : -0.5f * sz.x, y + 0.05f, z};
    }
    return true;
}

void TownTraffic::forEachPrefabDraw(
    const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&, const glm::mat4&, bool)>& fn) {
    for (std::size_t k = 0; k < m_sim.vehicles().size(); ++k) {
        const Vehicle& v = m_sim.vehicles()[k];
        if (v.entity >= 0) continue;
        const Pose p = m_sim.pose(v);
        if (!asPrefab(v, p.pos) || !inView(p.pos, kPrefabReach, 0.6f * v.length + 2.0f)) continue;
        const bool detail = !m_haveView || glm::length(p.pos - m_eye) < kPrefabDetail;
        const PrefabLook& look = m_looks[static_cast<std::size_t>(v.prefab)];
        const glm::mat4 frame = frameOf(p);
        drawLook(look, frame, v.odo / look.rig.radius,
                 steerOf(v, look.rig.wheelbase, look.rig.maxSteer), detail, fn);
        // Its lamps, where flatten() found its front and back.
        const std::uint8_t lamps = k < m_vehLamps.size() ? m_vehLamps[k] : 0;
        const glm::vec3 F = look.lampFront, R = look.lampRear;
        auto lamp = [&](glm::vec3 c, glm::vec3 size, int slot) {
            fn(m_lampCube, m_mats[static_cast<std::size_t>(slot)],
               frame * glm::scale(glm::translate(glm::mat4(1.0f), c), size), detail);
        };
        for (float s : {-1.0f, 1.0f}) {
            const bool on = (lamps & (s < 0.0f ? LeftOn : RightOn)) != 0;
            lamp({F.x - 0.01f, F.y, s * F.z}, {0.05f, 0.11f, 0.24f}, LampHead);
            lamp({F.x - 0.01f, F.y, s * (F.z + 0.19f)}, {0.05f, 0.07f, 0.09f}, on ? LampBlink : LampOff);
            lamp({R.x + 0.01f, R.y, s * R.z}, {0.05f, 0.10f, 0.24f},
                 (lamps & Braking) ? LampBrake : LampTail);
            lamp({R.x + 0.01f, R.y + 0.10f, s * R.z}, {0.05f, 0.05f, 0.16f}, on ? LampBlink : LampOff);
        }
    }
    for (const Wreck& w : m_wrecks) {
        if (w.prefab < 0 || w.prefab >= static_cast<int>(m_looks.size())) continue;
        const glm::vec3 at(w.now[3]);
        const PrefabLook& look = m_looks[static_cast<std::size_t>(w.prefab)];
        if (!inView(at, kPrefabReach, 0.6f * look.length + 2.0f)) continue;
        const bool detail = !m_haveView || glm::length(at - m_eye) < kPrefabDetail;
        drawLook(look, w.now, w.odo / look.rig.radius, 0.0f, detail, fn);
    }
    // The people update() picked, each in the pose its steps have reached: the
    // walk runs with the ground, a cycle per `cycle` metres, so the feet stay
    // where they are put.
    const std::vector<Walker>& ws = m_sim.walkers();
    for (std::size_t k = 0; k < ws.size() && k < m_personNear.size(); ++k) {
        if (!m_personNear[k]) continue;
        const Walker& w = ws[k];
        if (w.prefab < 0 || w.prefab >= static_cast<int>(m_personLooks.size())) continue;
        const PersonLook& look = m_personLooks[static_cast<std::size_t>(w.prefab)];
        const Pose p = m_sim.pose(w);
        const bool detail = !m_haveView || glm::length(p.pos - m_eye) < kPersonDetail;
        const float walked = w.phase / kTwoPi * 1.4f;   // the sim's stride clock, in metres
        float u = walked / look.cycle;
        u -= std::floor(u);
        const glm::mat4 m = frameOf(p);
        for (const PersonPart& part : look.parts) {
            const std::size_t n = part.poses.size();
            const fitzel::Mesh* mesh = part.poses[std::min(static_cast<std::size_t>(u * n), n - 1)];
            fn(*mesh, part.material, m * part.local, detail);
        }
    }
}

void TownTraffic::drawLook(const PrefabLook& look, const glm::mat4& m, float spin, float steer,
                           bool detail,
                           const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                                    const glm::mat4&, bool)>& fn) const {
    std::array<glm::mat4, 4> turned;
    if (look.rig.any)
        for (int i = 0; i < 4; ++i)
            turned[static_cast<std::size_t>(i)] = look.frame * wheelTurn(look.rig, i, spin, steer);
    for (const PrefabPart& part : look.parts)
        fn(*part.mesh, part.material,
           part.wheel >= 0 && look.rig.any
               ? m * turned[static_cast<std::size_t>(part.wheel)] * part.rest
               : m * part.local,
           detail);
}

// --- Person prefabs -----------------------------------------------------------------

bool TownTraffic::flattenPerson(const prefab::Prefab& p, int forward, PersonLook& out,
                                std::map<std::string, std::shared_ptr<Walk>>& used) {
    // The prefab's entities in its own frame, their hierarchy resolved.
    std::vector<Entity> es = p.entities;
    scenegraph::resolve(es);
    // The way it faces onto the person frame's +x: +Z turns a quarter left,
    // -Z a quarter right, +X stays, -X turns round.
    const float turn = forward == 1 ? -90.0f : forward == 2 ? 0.0f : forward == 3 ? 180.0f : 90.0f;
    const glm::mat4 toFront = glm::rotate(glm::mat4(1.0f), glm::radians(turn), glm::vec3(0, 1, 0));
    const glm::vec3 front = forward == 1 ? glm::vec3(0, 0, -1) : forward == 2 ? glm::vec3(1, 0, 0)
                          : forward == 3 ? glm::vec3(-1, 0, 0) : glm::vec3(0, 0, 1);
    glm::vec3 lo(1e30f), hi(-1e30f);
    auto grow = [&](const glm::mat4& m, const glm::vec3& a, const glm::vec3& b) {
        for (int k = 0; k < 8; ++k) {
            const glm::vec3 c((k & 1) ? b.x : a.x, (k & 2) ? b.y : a.y, (k & 4) ? b.z : a.z);
            const glm::vec3 w = glm::vec3(toFront * m * glm::vec4(c, 1.0f));
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    };

    std::vector<PersonPart> parts;
    float cycle = 0.0f;
    std::vector<fitzel::Vertex> scratch;
    for (const Entity& e : es) {
        if (!e.activeInHierarchy) continue;
        if (const auto* mc = e.components.get<ModelComponent>(); mc && models) {
            LoadedModel* lm = models->byId(mc->modelId);
            if (!lm) continue;
            // As SceneSubmit draws a model: filling centre +/- half.
            const glm::vec3 sz = glm::max(lm->size(), glm::vec3(1e-4f));
            const glm::mat4 m = scenegraph::compose(e.center, e.rotation, (e.half * 2.0f) / sz) *
                                glm::translate(glm::mat4(1.0f), -lm->center());
            grow(m, lm->boundsMin, lm->boundsMax);
            const fitzel::ModelData* md = lm->animData.get();
            if (!lm->animated || !md || md->animations.empty()) {
                for (std::size_t i = 0; i < lm->meshes.size(); ++i)
                    parts.push_back({{&lm->meshes[i]}, lm->primMaterialId[i], toFront * m});
                continue;
            }
            // The walk: the prefab's own Animation clip and range (the first
            // clip, whole, when it has none), skinned into its poses -- or
            // taken from an earlier build, the same model walking the same way.
            const auto* ac = e.components.get<AnimationComponent>();
            const int clip = glm::clamp(ac ? ac->clip : 0, 0, static_cast<int>(md->animations.size()) - 1);
            const float dur = md->animations[static_cast<std::size_t>(clip)].duration;
            const float start = ac ? glm::clamp(ac->start, 0.0f, dur) : 0.0f;
            float end = (ac && ac->end > ac->start) ? glm::clamp(ac->end, 0.0f, dur) : dur;
            if (end <= start) end = dur;
            const float span = std::max(end - start, 1e-3f);
            // The way the person faces, in the model's own space (the entity
            // may be turned in the prefab).
            const glm::mat3 rot(scenegraph::compose(glm::vec3(0.0f), e.rotation, glm::vec3(1.0f)));
            const glm::vec3 fwdModel = glm::normalize(glm::transpose(rot) * front);
            char key[160];
            std::snprintf(key, sizeof key, "%p#%d#%d#%.4f#%.4f#%.3f,%.3f,%.3f",
                          static_cast<const void*>(lm), mc->modelId, clip, start, end,
                          fwdModel.x, fwdModel.y, fwdModel.z);
            std::shared_ptr<Walk>& walk = used[key];
            if (!walk) {
                if (const auto it = m_walks.find(key); it != m_walks.end()) walk = it->second;
            }
            if (!walk) {
                // One stride pair of it (a clip may hold several), densely:
                // the poses are all a person has, and too few read as a stutter.
                walk = std::make_shared<Walk>();
                walk->prims  = std::min(lm->meshes.size(), md->primitives.size());
                walk->period = span / static_cast<float>(walkpace::cycles(*md, clip, start, end));
                walk->count  = std::clamp(static_cast<int>(std::lround(walk->period * kWalkPosesPerSecond)),
                                          kWalkPosesMin, kWalkPosesMax);
                std::vector<fitzel::ModelPrimitive>     welded(walk->prims);
                std::vector<std::vector<std::uint32_t>> indices(walk->prims);
                for (std::size_t i = 0; i < walk->prims; ++i)
                    welded[i] = walkpace::weld(md->primitives[i], indices[i]);
                for (int k = 0; k < walk->count; ++k) {
                    const std::vector<glm::mat4> palette = fitzel::sampleSkeleton(
                        *md, clip, start + walk->period * static_cast<float>(k) / static_cast<float>(walk->count));
                    for (std::size_t i = 0; i < walk->prims; ++i) {
                        fitzel::skinPrimitive(welded[i], palette, scratch);
                        walk->poses.push_back(
                            std::make_unique<fitzel::Mesh>(fitzel::Mesh::create(scratch, indices[i])));
                    }
                }
                walk->pace = std::abs(walkpace::stanceSpeed(*md, clip, fwdModel));
            }
            const std::size_t np = std::min({walk->prims, lm->meshes.size(), lm->primMaterialId.size()});
            for (std::size_t i = 0; i < np; ++i) {
                PersonPart part{{}, lm->primMaterialId[i], toFront * m};
                for (int k = 0; k < walk->count; ++k)
                    part.poses.push_back(walk->poses[static_cast<std::size_t>(k) * walk->prims + i].get());
                parts.push_back(std::move(part));
            }
            out.walks.push_back(walk);
            // How far one run of the walk carries them: the clip's own pace
            // scaled into metres.
            if (cycle <= 0.0f) {
                const float scale = glm::length(glm::mat3(m) * fwdModel);
                if (walk->pace * scale > 0.2f) cycle = walk->pace * scale * walk->period;
            }
        } else if (const auto* meshC = e.components.get<MeshComponent>(); meshC && meshCache) {
            // Modelled in the editor: uploaded by the shared cache under an id
            // of its own (one per person look and entity), dressed as SceneSubmit does.
            const int cacheId = 0x78000000 + static_cast<int>(m_personLooks.size()) * 4096 + e.id;
            const modifiers::Shown sh =
                modifiers::shown(cacheId, *meshC, e.components.get<ModifierStackComponent>());
            const glm::mat4 m = scenegraph::compose(e.center, e.rotation,
                                                    editmesh::fitScale(*sh.mesh, e.half));
            const auto* matC = e.components.get<MaterialComponent>();
            const fitzel::AssetId own = matC ? matC->material : fitzel::AssetId{};
            for (const EditMeshCache::Sub& sub : meshCache->submeshes(cacheId, sh.revision, *sh.mesh))
                parts.push_back({{&sub.mesh}, sub.material.valid() ? sub.material : own, toFront * m});
            glm::vec3 mlo, mhi;
            sh.mesh->bounds(mlo, mhi);
            grow(m, mlo, mhi);
        }
    }
    if (parts.empty() || lo.x > hi.x) return false;
    // Feet on y = 0, centred in x and z.
    const glm::mat4 centre = glm::translate(
        glm::mat4(1.0f), glm::vec3(-0.5f * (lo.x + hi.x), -lo.y, -0.5f * (lo.z + hi.z)));
    for (PersonPart& part : parts) part.local = centre * part.local;
    out.parts = std::move(parts);
    out.cycle = cycle > 0.2f ? cycle : 1.4f;   // unmeasured: a stride pair
    return true;
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

// What a driven body weighs when its object does not say (a vehicle rig does).
float kindMass(traffic::Kind k) {
    switch (k) {
        case traffic::Kind::Bus:   return 12000.0f;
        case traffic::Kind::Truck: return 9000.0f;
        default:                   return 1300.0f;
    }
}

// A physics box as the traffic's obstacle. False if the body is gone.
bool obstacleOf(fitzel::PhysicsWorld& physics, std::uint32_t body, const glm::vec3& half,
                Obstacle& o) {
    glm::quat q;
    glm::vec3 vel(0.0f);
    if (!body || !physics.getTransform(body, o.center, q)) return false;
    physics.getLinearVelocity(body, vel);
    const glm::mat3 m = glm::mat3_cast(q);
    for (int i = 0; i < 3; ++i) o.axes[i] = m[i] * half[i];
    o.vel = {vel.x, vel.z};
    return true;
}

// Steer a driven body onto (pos, q) over the coming step -- or jump it there
// when that is a leap (put on a lane, or on another after a rebuild) or a sharp
// turn: flown, it would go through whatever is in between, and the physics caps
// how fast a body spins, so the spin it did not get would read as a crash next
// tick. Then read back what it was asked, to tell a knock from the driving.
void steerBody(fitzel::PhysicsWorld& ph, std::uint32_t body, const glm::vec3& pos,
               const glm::quat& q, float vmax, float dt, glm::vec3& askedVel, glm::vec3& askedSpin) {
    glm::vec3 now(0.0f);
    glm::quat nowQ(1.0f, 0.0f, 0.0f, 0.0f);
    ph.getTransform(body, now, nowQ);
    const float turn = 2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(nowQ, q))));
    if (glm::distance(now, pos) > 1.0f + 2.0f * vmax * std::max(dt, 1.0f / 60.0f) ||
        turn > glm::radians(30.0f))
        ph.setTransform(body, pos, q);
    else
        ph.setKinematicTarget(body, pos, q, std::max(dt, 1e-3f));
    ph.getLinearVelocity(body, askedVel);
    ph.getAngularVelocity(body, askedSpin);
}

// How hard the physics step just taken knocked a driven body (traffic::jolt),
// and whether that crashes it.
bool knocked(fitzel::PhysicsWorld& ph, std::uint32_t body, const glm::vec3& askedVel,
             const glm::vec3& askedSpin, float reach, float limit, float& j) {
    glm::vec3 vel(0.0f), spin(0.0f);
    ph.getLinearVelocity(body, vel);
    ph.getAngularVelocity(body, spin);
    j = jolt(vel, askedVel, spin, askedSpin, reach);
    return j >= limit;
}

// The knock that crashes a town vehicle: TrafficDriverComponent's default.
constexpr float kTownCrash = 10.0f / 3.6f;
// The town vehicles lent a body: the nearest this many, this near the player.
constexpr float       kBodyReach = 70.0f;
constexpr std::size_t kBodiesAtMost = 32;

const char* kindName(Kind k) {
    return k == Kind::Bus ? "bus" : k == Kind::Truck ? "lorry" : "car";
}
} // namespace

int TownTraffic::wreckCount() const {
    return static_cast<int>(m_wrecks.size()) +
           static_cast<int>(std::count_if(m_drivers.begin(), m_drivers.end(),
                                          [](const Driver& d) { return d.wrecked; }));
}

void TownTraffic::tickTownBodies(fitzel::PhysicsWorld& physics, float dt) {
    const std::vector<Vehicle>& V = m_sim.vehicles();
    auto indexOf = [&](std::uint32_t uid) {
        for (int i = 0; i < static_cast<int>(V.size()); ++i)
            if (V[static_cast<std::size_t>(i)].uid == uid) return i;
        return -1;
    };

    // Crashes first: what the step just taken knocked harder than it takes
    // leaves the traffic -- with its placeholder instance, which runs parallel
    // to the town's vehicles -- and lies where its body goes from now on.
    for (std::size_t k = 0; k < m_proxies.size();) {
        const Proxy px = m_proxies[k];
        const int vi = indexOf(px.uid);
        float j = 0.0f;
        if (vi < 0 || !knocked(physics, px.body, px.askedVel, px.askedSpin, glm::length(px.half),
                               kTownCrash, j)) {
            ++k;
            continue;
        }
        const Vehicle& v = V[static_cast<std::size_t>(vi)];
        std::size_t inst = 0;
        for (int i = 0; i < vi; ++i) inst += V[static_cast<std::size_t>(i)].entity < 0;
        Wreck w;
        w.body   = px.body;
        w.half   = px.half;
        w.prefab = v.prefab;
        w.odo    = v.odo;
        if (inst < m_instances.size()) {
            w.inst = m_instances[inst];
            m_instances.erase(m_instances.begin() + static_cast<std::ptrdiff_t>(inst));
        } else {
            w.inst = {&m_templates[static_cast<std::size_t>(v.kind)], v.look, false};
        }
        w.now = w.inst.prev;
        std::fprintf(stderr, "[Fitzel] a town %s crashed (a %.0f km/h jolt)\n", kindName(v.kind),
                     j * 3.6f);
        physics.releaseBody(px.body);
        m_sim.removeVehicle(px.uid);   // v is gone after this
        m_wrecks.push_back(w);
        m_crashedTown = true;
        m_proxies.erase(m_proxies.begin() + static_cast<std::ptrdiff_t>(k));
    }

    // Who is near the player's car (or the eye, on foot): the nearest few get
    // a body, steered onto the pose the simulation gives them.
    glm::vec3 focus = m_eye;
    if (m_playerBody) {
        glm::vec3 p;
        glm::quat q;
        if (physics.getTransform(m_playerBody, p, q)) focus = p;
    }
    std::vector<std::pair<float, int>> near;
    for (int i = 0; i < static_cast<int>(V.size()); ++i) {
        const Vehicle& v = V[static_cast<std::size_t>(i)];
        if (v.entity >= 0) continue;
        const glm::vec3 d = m_sim.pose(v).pos - focus;
        const float d2 = glm::dot(d, d);
        if (d2 < kBodyReach * kBodyReach) near.push_back({d2, i});
    }
    if (near.size() > kBodiesAtMost) {
        std::partial_sort(near.begin(), near.begin() + kBodiesAtMost, near.end());
        near.resize(kBodiesAtMost);
    }
    for (Proxy& px : m_proxies) px.wanted = false;
    for (const auto& n : near) {
        const Vehicle& v = V[static_cast<std::size_t>(n.second)];
        const glm::mat4 f = frameOf(m_sim.pose(v));
        std::size_t k = 0;
        while (k < m_proxies.size() && m_proxies[k].uid != v.uid) ++k;
        const glm::vec3 half = k < m_proxies.size() ? m_proxies[k].half : halfOf(v);
        const glm::vec3 centre(f * glm::vec4(0.0f, half.y, 0.0f, 1.0f));
        const glm::quat q = glm::quat_cast(glm::mat3(f));
        if (k == m_proxies.size()) {
            Proxy px;
            px.uid  = v.uid;
            px.half = half;
            px.body = physics.addDrivenBox(half, centre, q, kindMass(v.kind));
            if (!px.body) continue;
            m_proxies.push_back(px);
        }
        Proxy& px = m_proxies[k];
        px.wanted = true;
        steerBody(physics, px.body, centre, q, v.vmax, dt, px.askedVel, px.askedSpin);
    }
    // Out of reach again: the body goes back.
    for (std::size_t k = 0; k < m_proxies.size();) {
        if (m_proxies[k].wanted) { ++k; continue; }
        physics.removeBody(m_proxies[k].body);
        m_proxies.erase(m_proxies.begin() + static_cast<std::ptrdiff_t>(k));
    }

    // The wrecks' poses for drawing: their bodies, down to the vehicle frame
    // (which stands on the road, the box sits on it).
    for (Wreck& w : m_wrecks) {
        glm::vec3 p;
        glm::quat q;
        if (physics.getTransform(w.body, p, q))
            w.now = glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q) *
                    glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -w.half.y, 0.0f));
    }
}

void TownTraffic::beginPlay(std::vector<Entity>& entities, fitzel::PhysicsWorld* physics) {
    m_drivers.clear();
    m_proxies.clear();
    m_wrecks.clear();
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
        // A body with the car's own weight, standing where it was authored
        // until the traffic takes it (asked for nothing yet: a car still
        // waiting for its lane can be crashed too).
        d.crashJolt = std::max(dc->crashJolt, 1.0f) / 3.6f;
        if (physics && dc->collider) {
            const auto* vc = e.components.get<VehicleComponent>();
            d.half = glm::max(e.half, glm::vec3(0.2f));
            d.body = physics->addDrivenBox(d.half, e.center, glm::quat(glm::radians(e.rotation)),
                                           vc ? vc->mass : kindMass(d.kind));
        }
        // Placed now if the towns' streets are there already, else as soon as
        // they are (a game started straight into Play derives them after this).
        m_drivers.push_back(d);
    }
    placeDrivers();
}

void TownTraffic::placeDrivers() {
    if (m_sim.lanes().empty()) return;
    for (Driver& d : m_drivers) {
        if (d.placed || d.wrecked) continue;
        d.placed = m_sim.addDriver(d.entity, d.pos, d.heading, d.kind, d.length, d.vmax);
        if (!d.placed) {
            std::fprintf(stderr, "[Fitzel] traffic driver '%s': no lane within 60 m\n", d.name.c_str());
            d.placed = true;   // said once; it stays where it was authored
        }
    }
}

void TownTraffic::playTick(std::vector<Entity>& entities, fitzel::PhysicsWorld* physics, float dt,
                           const std::function<void(Entity&, const glm::vec3&, const glm::vec3&)>& place) {
    if (!m_playing) return;
    auto entityOf = [&](int id) -> Entity* {
        for (Entity& x : entities) if (x.id == id) return &x;
        return nullptr;
    };

    // Crashes: the physics step just taken knocked a driven body off what it
    // was asked by more than it takes. It leaves the traffic, and from now on
    // the physics has it.
    if (physics)
        for (Driver& d : m_drivers) {
            float j = 0.0f;
            if (!d.body || d.wrecked ||
                !knocked(*physics, d.body, d.askedVel, d.askedSpin, glm::length(d.half), d.crashJolt, j))
                continue;
            d.wrecked = true;
            physics->releaseBody(d.body);
            m_sim.removeDriver(d.entity);
            std::fprintf(stderr, "[Fitzel] traffic driver '%s' crashed (a %.0f km/h jolt)\n",
                         d.name.c_str(), j * 3.6f);
        }
    // ...and the towns' own vehicles near the player.
    if (physics) tickTownBodies(*physics, dt);

    for (const Vehicle& v : m_sim.vehicles()) {
        if (v.entity < 0) continue;
        Driver* d = nullptr;
        for (Driver& x : m_drivers) if (x.entity == v.entity) { d = &x; break; }
        Entity* e = entityOf(v.entity);
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
        // The correction is read live, so turning a wheel in the inspector
        // while the car drives shows at once.
        const auto* vc = e->components.get<VehicleComponent>();
        for (int i = 0; i < 4; ++i) {
            if (d->wheel[i] < 0) continue;
            for (Entity& w : entities)
                if (w.id == d->wheel[i]) {
                    w.localRotation = vehiclerig::wheelLocalRotation(
                        d->wheelRest[i], vc ? vc->wheelTurn[i] : glm::vec3(0.0f),
                        spin * d->spinSign, i < 2 ? steer : 0.0f);
                    break;
                }
        }
        if (physics && d->body)
            steerBody(*physics, d->body, pos, glm::quat(glm::radians(rot)), d->vmax, dt,
                      d->askedVel, d->askedSpin);
    }

    // Wrecks lie where the physics has them, tumbled as they are.
    if (physics)
        for (const Driver& d : m_drivers) {
            if (!d.wrecked) continue;
            Entity* e = entityOf(d.entity);
            glm::vec3 p;
            glm::quat q;
            if (e && physics->getTransform(d.body, p, q)) place(*e, p, sceneEuler(glm::mat3_cast(q)));
        }

    // What the traffic stops for: the wrecks, and the player's car.
    std::vector<Obstacle> obstacles;
    if (physics) {
        Obstacle o;
        for (const Driver& d : m_drivers)
            if (d.wrecked && obstacleOf(*physics, d.body, d.half, o)) obstacles.push_back(o);
        for (const Wreck& w : m_wrecks)
            if (obstacleOf(*physics, w.body, w.half, o)) obstacles.push_back(o);
        if (obstacleOf(*physics, m_playerBody, m_playerHalf, o)) obstacles.push_back(o);
    }
    // A person: a box as wide as one with arms at the sides and some room to
    // spare, from the feet up, standing still as far as the traffic knows.
    for (const glm::vec3& f : m_people) {
        Obstacle o;
        o.center  = f + glm::vec3(0.0f, 0.9f, 0.0f);
        o.axes[0] = glm::vec3(0.45f, 0.0f, 0.0f);
        o.axes[1] = glm::vec3(0.0f, 0.9f, 0.0f);
        o.axes[2] = glm::vec3(0.0f, 0.0f, 0.45f);
        obstacles.push_back(o);
    }
    obstacles.insert(obstacles.end(), m_trams.begin(), m_trams.end());
    m_sim.setObstacles(obstacles);
}

void TownTraffic::endPlay() {
    m_sim.removeDrivers();
    m_sim.setObstacles({});
    m_drivers.clear();
    m_proxies.clear();   // their bodies go with Play's physics world
    m_wrecks.clear();
    m_playing    = false;
    m_playerBody = 0;
    // The crashed come back: the traffic is built again, as it was.
    if (m_crashedTown) m_revision = -1;
    m_crashedTown = false;
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
        {
            // The knock it takes before it crashes, as the change in its speed
            // (delta-v): the masses are in it, so a car barely shakes a bus.
            Property& q = add("Crashes at a jolt of", "crashJolt", PropKind::Float,
                              [](void* o) -> void* { return &static_cast<T*>(o)->crashJolt; });
            q.min = 2.0f; q.max = 60.0f; q.speed = 0.5f; q.fmt = "%.0f km/h";
            q.visible = [](const void* o) { return static_cast<const T*>(o)->collider; };
        }
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
