#include "BuildingGen.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>

#include "Component.hpp"

using fitzel::AssetId;

namespace buildings {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A yaw about +Y applied to a point in the XZ plane (local -> parent frame),
// matching the engine's Euler-Y convention: x' = x cos + z sin, z' = -x sin + z cos.
glm::vec2 yawXZ(glm::vec2 v, float deg) {
    const float r = glm::radians(deg), c = std::cos(r), s = std::sin(r);
    return {v.x * c + v.y * s, -v.x * s + v.y * c};
}

// The yaw (degrees) that points an entity's local +Z along `n`.
float yawTowards(glm::vec2 n) { return glm::degrees(std::atan2(n.x, n.y)); }

// Clamp every field into a range the generator can actually build, so no slider
// (or a hand-edited project) can ask for a 10,000-storey tower or a zero-height
// floor. Everything downstream may assume these bounds.
Params sane(const Params& in) {
    Params p = in;
    p.width       = glm::clamp(p.width, 3.0f, 400.0f);
    p.depth       = glm::clamp(p.depth, 3.0f, 400.0f);
    p.floors      = glm::clamp(p.floors, 1, 250);
    p.floorHeight = glm::clamp(p.floorHeight, 2.0f, 12.0f);
    p.sections    = glm::clamp(p.sections, 1, 12);
    p.taper       = glm::clamp(p.taper, 0.1f, 1.5f);
    p.jitter      = glm::clamp(p.jitter, 0.0f, 1.0f);
    p.twist       = glm::clamp(p.twist, -720.0f, 720.0f);
    p.podiumFloors = glm::clamp(p.podiumFloors, 0, std::max(0, p.floors - 1));
    p.podiumSpread = glm::clamp(p.podiumSpread, 1.0f, 4.0f);
    p.sink        = glm::clamp(p.sink, 0.0f, 8.0f);
    p.bandEvery   = glm::clamp(p.bandEvery, 0, 50);
    p.bandOverhang = glm::clamp(p.bandOverhang, 0.0f, 4.0f);
    p.fins        = glm::clamp(p.fins, 0, 16);
    p.finDepth    = glm::clamp(p.finDepth, 0.05f, 6.0f);
    p.crownHeight = glm::clamp(p.crownHeight, 0.0f, 1.5f);
    p.accentStrength = glm::clamp(p.accentStrength, 0.0f, 12.0f);
    p.palette     = glm::clamp(p.palette, 0, 7);
    return p;
}

// One stacked mass of the tower: the slab of height [y0,y1) with its own
// footprint and yaw (the twist sampled at its mid-height).
struct Mass {
    float y0, y1, w, d, yaw;
};

// Find-or-create one material by name, re-applying the colours either way (so
// re-generating with a new tint re-skins every building already placed on that
// palette slot, which is the point of sharing them).
AssetId ensureMaterial(std::vector<MaterialDef>& mats, const std::string& name,
                       glm::vec3 albedo, float refl, float rough,
                       glm::vec3 emission, float emissionStrength) {
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo           = albedo;
        m.reflectivity     = refl;
        m.roughness        = rough;
        m.emission         = emission;
        m.emissionStrength = emissionStrength;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId          = AssetId::generate();
    md.name             = name;
    md.albedo           = albedo;
    md.reflectivity     = refl;
    md.roughness        = rough;
    md.emission         = emission;
    md.emissionStrength = emissionStrength;
    mats.push_back(md);
    return md.assetId;
}

} // namespace

const char* styleName(Style s) {
    switch (s) {
        case Style::Skyscraper: return "Skyscraper";
        case Style::NeoTokyo:   return "Neo-Tokyo";
        case Style::Arcology:   return "Arcology";
        case Style::Needle:     return "Needle";
        case Style::Slab:       return "Slab";
        case Style::Habitat:    return "Habitat ring";
        case Style::Count:      break;
    }
    return "Building";
}

const char* footprintName(Footprint f) {
    switch (f) {
        case Footprint::Rect:      return "Rectangle";
        case Footprint::Chamfered: return "Faceted";
        case Footprint::Round:     return "Round";
        case Footprint::Cross:     return "Cross";
        case Footprint::L:         return "L-shape";
        case Footprint::Count:     break;
    }
    return "Rectangle";
}

const char* crownName(Crown c) {
    switch (c) {
        case Crown::None:    return "None";
        case Crown::Cap:     return "Roof cap";
        case Crown::Spire:   return "Spire";
        case Crown::Antenna: return "Antenna mast";
        case Crown::Dome:    return "Dome";
        case Crown::Halo:    return "Halo ring";
        case Crown::Slant:   return "Slanted top";
        case Crown::Count:   break;
    }
    return "None";
}

void applyStyle(Params& p, Style s) {
    switch (s) {
        case Style::Skyscraper:
            p.shape = Footprint::Rect;
            p.width = 26.0f; p.depth = 26.0f;
            p.floors = 46; p.floorHeight = 3.6f;
            p.sections = 4; p.taper = 0.55f; p.jitter = 0.25f; p.twist = 0.0f;
            p.podiumFloors = 3; p.podiumSpread = 1.5f;
            p.bandEvery = 6; p.bandOverhang = 0.4f;
            p.fins = 4; p.finDepth = 0.6f;
            p.crown = Crown::Antenna; p.crownHeight = 0.18f;
            break;
        case Style::NeoTokyo:
            p.shape = Footprint::Chamfered;
            p.width = 20.0f; p.depth = 20.0f;
            p.floors = 60; p.floorHeight = 3.4f;
            p.sections = 9; p.taper = 0.45f; p.jitter = 0.1f; p.twist = 90.0f;
            p.podiumFloors = 2; p.podiumSpread = 1.8f;
            p.bandEvery = 4; p.bandOverhang = 0.5f;
            p.fins = 2; p.finDepth = 0.8f;
            p.crown = Crown::Spire; p.crownHeight = 0.22f;
            break;
        case Style::Arcology:
            p.shape = Footprint::Cross;
            p.width = 70.0f; p.depth = 70.0f;
            p.floors = 30; p.floorHeight = 4.2f;
            p.sections = 5; p.taper = 0.3f; p.jitter = 0.15f; p.twist = 0.0f;
            p.podiumFloors = 4; p.podiumSpread = 1.35f;
            p.bandEvery = 5; p.bandOverhang = 0.9f;
            p.fins = 8; p.finDepth = 1.2f;
            p.crown = Crown::Halo; p.crownHeight = 0.25f;
            break;
        case Style::Needle:
            p.shape = Footprint::Round;
            p.width = 13.0f; p.depth = 13.0f;
            p.floors = 70; p.floorHeight = 3.5f;
            p.sections = 3; p.taper = 0.35f; p.jitter = 0.05f; p.twist = 0.0f;
            p.podiumFloors = 2; p.podiumSpread = 2.4f;
            p.bandEvery = 8; p.bandOverhang = 0.6f;
            p.fins = 3; p.finDepth = 0.5f;
            p.crown = Crown::Antenna; p.crownHeight = 0.45f;
            break;
        case Style::Slab:
            p.shape = Footprint::Rect;
            p.width = 62.0f; p.depth = 20.0f;
            p.floors = 26; p.floorHeight = 3.4f;
            p.sections = 2; p.taper = 0.9f; p.jitter = 0.3f; p.twist = 0.0f;
            p.podiumFloors = 2; p.podiumSpread = 1.2f;
            p.bandEvery = 3; p.bandOverhang = 0.3f;
            p.fins = 6; p.finDepth = 0.7f;
            p.crown = Crown::Slant; p.crownHeight = 0.12f;
            break;
        case Style::Habitat:
            p.shape = Footprint::Round;
            p.width = 34.0f; p.depth = 34.0f;
            p.floors = 34; p.floorHeight = 3.8f;
            p.sections = 3; p.taper = 0.75f; p.jitter = 0.1f; p.twist = 0.0f;
            p.podiumFloors = 3; p.podiumSpread = 1.6f;
            p.bandEvery = 4; p.bandOverhang = 1.0f;
            p.fins = 6; p.finDepth = 1.0f;
            p.crown = Crown::Halo; p.crownHeight = 0.3f;
            break;
        case Style::Count:
            break;
    }
}

Palette ensurePalette(std::vector<MaterialDef>& materials, const Params& pIn) {
    const Params p = sane(pIn);
    const std::string slot =
        std::string("Building ") + static_cast<char>('A' + p.palette) + " ";
    Palette pal;
    pal.glass = ensureMaterial(materials, slot + "Glass", p.glassTint,
                               0.70f, 0.08f, glm::vec3(0.0f), 1.0f);
    pal.frame = ensureMaterial(materials, slot + "Frame", p.frameTint,
                               0.25f, 0.45f, glm::vec3(0.0f), 1.0f);
    // The neon: nearly black under light, all of its brightness from emission --
    // so it reads as a lit strip at night instead of a pale grey band by day.
    pal.accent = ensureMaterial(materials, slot + "Neon", p.accentColor * 0.15f,
                                0.0f, 0.5f, p.accentColor, p.accentStrength);
    pal.base  = ensureMaterial(materials, slot + "Base", p.baseTint,
                               0.08f, 0.70f, glm::vec3(0.0f), 1.0f);
    return pal;
}

std::vector<Entity> generate(const Params& pIn, const Palette& pal,
                             int& counter, const glm::vec3& groundPos) {
    const Params p = sane(pIn);
    std::mt19937 rng(p.seed);
    auto jit = [&](float amount) { // symmetric jitter in [-amount, amount]
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        return u(rng) * amount;
    };

    std::vector<Entity> out;
    out.reserve(64);

    Entity root;
    root.type        = EntityType::Empty;
    root.name        = "Building";
    root.id          = counter++;
    root.parent      = -1;
    root.half        = glm::vec3(0.5f);
    root.localCenter = root.center = groundPos;
    out.push_back(std::move(root));
    const int rootId = out.front().id;

    // Every part is a child of the root, positioned in the root's frame (y = 0
    // is the ground the building stands on).
    auto add = [&](EntityType t, std::string name, glm::vec3 lc, glm::vec3 half,
                   float yawDeg, AssetId mat, bool collide) -> int {
        Entity e;
        e.type          = t;
        e.name          = std::move(name);
        e.id            = counter++;
        e.parent        = rootId;
        e.half          = glm::max(half, glm::vec3(0.02f));
        e.localCenter   = lc;
        e.center        = groundPos + lc;
        e.localRotation = glm::vec3(0.0f, yawDeg, 0.0f);
        e.rotation      = e.localRotation;
        if (mat.valid()) {
            auto mc = std::make_unique<MaterialComponent>();
            mc->material = mat;
            e.components.items.push_back(std::move(mc));
        }
        if (collide) { // static: a wall to crash into, never a falling body
            auto pc = std::make_unique<PhysicsComponent>();
            pc->dynamic = false;
            e.components.items.push_back(std::move(pc));
        }
        const int id = e.id;
        out.push_back(std::move(e));
        return id;
    };

    // --- Massing ------------------------------------------------------------
    const float floorH = p.floorHeight;
    const float H      = p.floors * floorH;              // roof height
    const float podH   = p.podiumFloors * floorH;        // podium top
    const float bodyH  = std::max(H - podH, floorH);     // tower above the podium

    std::vector<Mass> masses;
    masses.reserve(p.sections);
    {
        // Lower sections get more of the height (a tower carries its bulk low),
        // nudged by the seed so a row of towers isn't a row of clones.
        std::vector<float> wgt(p.sections);
        float sum = 0.0f;
        for (int i = 0; i < p.sections; ++i) {
            const float t = (p.sections > 1)
                                ? static_cast<float>(i) / (p.sections - 1) : 0.0f;
            wgt[i] = std::max(0.15f, (1.0f - 0.45f * t) * (1.0f + jit(p.jitter * 0.5f)));
            sum += wgt[i];
        }
        float y = podH;
        for (int i = 0; i < p.sections; ++i) {
            const float h = bodyH * wgt[i] / sum;
            const float t = (p.sections > 1)
                                ? static_cast<float>(i) / (p.sections - 1) : 0.0f;
            const float s = glm::clamp((1.0f - (1.0f - p.taper) * t) *
                                           (1.0f + jit(p.jitter * 0.18f)),
                                       0.08f, 1.5f);
            const float yaw = p.twist * ((y + h * 0.5f) - podH) / bodyH;
            masses.push_back({y, y + h, p.width * s, p.depth * s, yaw});
            y += h;
        }
        // Without a podium the lowest mass carries the sink itself, so the
        // building never floats over a bumpy patch of terrain.
        if (podH <= 0.0f) masses.front().y0 = -p.sink;
    }

    // One mass, as the current footprint: the shape switch lives here so every
    // caller (body, podium, bands) gets the same silhouette for free.
    auto emitMass = [&](const std::string& name, float y0, float y1, float w,
                        float d, float yaw, AssetId mat, bool collide) {
        const float hy = (y1 - y0) * 0.5f;
        const glm::vec3 c(0.0f, (y0 + y1) * 0.5f, 0.0f);
        switch (p.shape) {
            case Footprint::Round:
                add(EntityType::Cylinder, name, c, {w * 0.5f, hy, d * 0.5f}, yaw,
                    mat, collide);
                break;
            case Footprint::Chamfered: // a second, 45-degrees mass -> faceted corners
                add(EntityType::Box, name, c, {w * 0.5f, hy, d * 0.5f}, yaw, mat,
                    collide);
                add(EntityType::Box, name + " facet", c,
                    {w * 0.41f, hy * 0.998f, d * 0.41f}, yaw + 45.0f, mat, false);
                break;
            case Footprint::Cross:
                add(EntityType::Box, name + " A", c, {w * 0.5f, hy, d * 0.26f}, yaw,
                    mat, collide);
                add(EntityType::Box, name + " B", c, {w * 0.26f, hy, d * 0.5f}, yaw,
                    mat, collide);
                break;
            case Footprint::L: {
                const glm::vec2 a = yawXZ({0.0f, -d * 0.25f}, yaw);
                const glm::vec2 b = yawXZ({-w * 0.25f, d * 0.25f}, yaw);
                add(EntityType::Box, name + " A", {a.x, c.y, a.y},
                    {w * 0.5f, hy, d * 0.25f}, yaw, mat, collide);
                add(EntityType::Box, name + " B", {b.x, c.y, b.y},
                    {w * 0.25f, hy, d * 0.25f}, yaw, mat, collide);
                break;
            }
            case Footprint::Rect:
            case Footprint::Count:
                add(EntityType::Box, name, c, {w * 0.5f, hy, d * 0.5f}, yaw, mat,
                    collide);
                break;
        }
    };

    if (podH > 0.0f)
        emitMass("Podium", -p.sink, podH, p.width * p.podiumSpread,
                 p.depth * p.podiumSpread, 0.0f, pal.base, p.collider);

    for (std::size_t i = 0; i < masses.size(); ++i) {
        const Mass& m = masses[i];
        emitMass("Mass " + std::to_string(i + 1), m.y0, m.y1, m.w, m.d, m.yaw,
                 pal.glass, p.collider);
    }

    // The footprint at a given height (for bands and the crown), plus which mass
    // it belongs to -- so a band never straddles a setback edge.
    auto massAt = [&](float y) -> const Mass* {
        for (const Mass& m : masses)
            if (y >= m.y0 && y < m.y1) return &m;
        return &masses.back();
    };

    // --- Mechanical bands ----------------------------------------------------
    // The single cheapest thing that makes a plain extrusion read as a BUILDING:
    // a horizontal ledge every few floors, optionally with a lit strip on it.
    if (p.bandEvery > 0) {
        int band = 0;
        for (int f = p.bandEvery; f < p.floors; f += p.bandEvery) {
            const float by = f * floorH;
            if (by <= masses.front().y0 + 1.0f) continue;
            const Mass* m = massAt(by);
            if (by - m->y0 < 0.8f || m->y1 - by < 0.8f) continue; // clear of setbacks
            const float ov = p.bandOverhang;
            const EntityType t = (p.shape == Footprint::Round) ? EntityType::Cylinder
                                                               : EntityType::Box;
            ++band;
            add(t, "Band " + std::to_string(band), {0.0f, by, 0.0f},
                {m->w * 0.5f + ov, 0.28f, m->d * 0.5f + ov}, m->yaw, pal.frame, false);
            if (p.neon)
                add(t, "Neon " + std::to_string(band), {0.0f, by, 0.0f},
                    {m->w * 0.5f + ov * 1.2f, 0.10f, m->d * 0.5f + ov * 1.2f},
                    m->yaw, pal.accent, false);
        }
    }

    // --- Vertical fins -------------------------------------------------------
    // Pilasters running each mass, spaced around its perimeter. Per mass (not
    // per building) so they follow the setbacks and the twist instead of
    // floating off the facade higher up.
    if (p.fins > 0) {
        for (std::size_t i = 0; i < masses.size(); ++i) {
            const Mass& m = masses[i];
            const float hy    = (m.y1 - m.y0) * 0.5f;
            const float yc    = (m.y0 + m.y1) * 0.5f;
            const float thick = glm::clamp(std::min(m.w, m.d) * 0.06f, 0.25f, 2.5f);
            for (int j = 0; j < p.fins; ++j) {
                const float t = (static_cast<float>(j) + 0.5f) / p.fins;
                glm::vec2 pos(0.0f), n(0.0f, 1.0f);
                if (p.shape == Footprint::Round) {
                    const float a = t * 2.0f * kPi;
                    pos = {m.w * 0.5f * std::sin(a), m.d * 0.5f * std::cos(a)};
                    n   = glm::normalize(glm::vec2(std::sin(a), std::cos(a)));
                } else { // walk the rectangle's perimeter, starting at the +Z face
                    const float per = 2.0f * (m.w + m.d);
                    float s = t * per;
                    if (s < m.w)              { pos = {-m.w * 0.5f + s, m.d * 0.5f};  n = { 0.0f,  1.0f}; }
                    else if (s < m.w + m.d)   { s -= m.w;           pos = { m.w * 0.5f, m.d * 0.5f - s};  n = { 1.0f, 0.0f}; }
                    else if (s < 2*m.w + m.d) { s -= m.w + m.d;     pos = { m.w * 0.5f - s, -m.d * 0.5f}; n = { 0.0f, -1.0f}; }
                    else                      { s -= 2*m.w + m.d;   pos = {-m.w * 0.5f, -m.d * 0.5f + s}; n = {-1.0f, 0.0f}; }
                }
                const glm::vec2 lp = pos + n * (p.finDepth * 0.5f); // stand proud
                const glm::vec2 wp = yawXZ(lp, m.yaw);
                add(EntityType::Box,
                    "Fin " + std::to_string(i + 1) + "." + std::to_string(j + 1),
                    {wp.x, yc, wp.y}, {thick, hy, p.finDepth * 0.5f},
                    m.yaw + yawTowards(n), pal.frame, false);
            }
        }
    }

    // --- Crown ---------------------------------------------------------------
    const Mass& top    = masses.back();
    const float wTop   = top.w, dTop = top.d, yawTop = top.yaw;
    const float crownH = std::max(2.0f, p.crownHeight * H);
    const AssetId neonMat = p.neon ? pal.accent : pal.frame;
    switch (p.crown) {
        case Crown::Cap:
            add(EntityType::Box, "Roof cap", {0.0f, H + 0.6f, 0.0f},
                {wTop * 0.5f + 0.8f, 0.6f, dTop * 0.5f + 0.8f}, yawTop, pal.frame,
                false);
            add(EntityType::Box, "Roof trim", {0.0f, H + 1.5f, 0.0f},
                {wTop * 0.42f, 0.25f, dTop * 0.42f}, yawTop, neonMat, false);
            break;
        case Crown::Spire: {
            const float seg = crownH / 3.0f;
            const float r[4] = {wTop * 0.32f, wTop * 0.18f, wTop * 0.07f, wTop * 0.02f};
            for (int i = 0; i < 3; ++i)
                add(EntityType::Cylinder, "Spire " + std::to_string(i + 1),
                    {0.0f, H + seg * (i + 0.5f), 0.0f},
                    {r[i], seg * 0.5f, r[i]}, yawTop, pal.frame, false);
            add(EntityType::Cylinder, "Spire tip", {0.0f, H + crownH * 1.15f, 0.0f},
                {std::max(0.15f, r[3]), crownH * 0.15f, std::max(0.15f, r[3])},
                yawTop, neonMat, false);
            break;
        }
        case Crown::Antenna: {
            const float r = std::max(0.30f, wTop * 0.02f);
            add(EntityType::Cylinder, "Mast", {0.0f, H + crownH * 0.5f, 0.0f},
                {r, crownH * 0.5f, r}, 0.0f, pal.frame, false);
            add(EntityType::Box, "Mast collar", {0.0f, H + crownH * 0.35f, 0.0f},
                {r * 4.0f, 0.4f, r * 4.0f}, yawTop, pal.frame, false);
            const float br = std::max(0.5f, wTop * 0.035f);
            add(EntityType::Sphere, "Beacon", {0.0f, H + crownH, 0.0f},
                glm::vec3(br), 0.0f, neonMat, false);
            if (p.beaconLight) { // one real light per tower -- the skyline reads
                Entity e;        // at night without lighting up the whole city
                e.type          = EntityType::Light;
                e.name          = "Beacon light";
                e.id            = counter++;
                e.parent        = rootId;
                e.half          = glm::vec3(0.3f);
                e.localCenter   = glm::vec3(0.0f, H + crownH, 0.0f);
                e.center        = groundPos + e.localCenter;
                auto lc = std::make_unique<LightComponent>();
                lc->color     = p.accentColor;
                lc->intensity = 14.0f;
                lc->range     = 45.0f;
                e.components.items.push_back(std::move(lc));
                out.push_back(std::move(e));
            }
            break;
        }
        case Crown::Dome:
            add(EntityType::Sphere, "Dome", {0.0f, H, 0.0f},
                {wTop * 0.5f, std::max(2.0f, wTop * 0.35f), dTop * 0.5f}, yawTop,
                pal.glass, false);
            break;
        case Crown::Halo: {
            const float ry = std::max(0.4f, crownH * 0.07f);
            const float hy = H + crownH * 0.55f;
            add(EntityType::Cylinder, "Halo", {0.0f, hy, 0.0f},
                {wTop * 0.85f, ry, dTop * 0.85f}, yawTop, neonMat, false);
            for (int i = 0; i < 2; ++i) { // the two struts that carry the ring
                const float sx = (i == 0 ? 1.0f : -1.0f) * wTop * 0.32f;
                const glm::vec2 wp = yawXZ({sx, 0.0f}, yawTop);
                add(EntityType::Box, "Halo strut " + std::to_string(i + 1),
                    {wp.x, H + crownH * 0.275f, wp.y},
                    {std::max(0.3f, wTop * 0.03f), crownH * 0.275f,
                     std::max(0.3f, dTop * 0.03f)},
                    yawTop, pal.frame, false);
            }
            break;
        }
        case Crown::Slant:
            add(EntityType::Ramp, "Slanted top", {0.0f, H + crownH * 0.5f, 0.0f},
                {wTop * 0.5f, crownH * 0.5f, dTop * 0.5f}, yawTop, pal.glass, false);
            break;
        case Crown::None:
        case Crown::Count:
            break;
    }

    return out;
}

int objectCount(const Params& p) {
    int counter = 0;
    Palette none; // invalid GUIDs -> no material components, same object count
    return static_cast<int>(generate(p, none, counter, glm::vec3(0.0f)).size());
}

} // namespace buildings
