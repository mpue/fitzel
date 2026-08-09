#include "CityGen.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>

#include <map>

#include "Component.hpp"
#include "Primitives.hpp"

namespace city {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// --- Deterministic noise ----------------------------------------------------
// Everything random here is hashed from a position (a lot index, a station),
// never drawn from a running generator. That is the difference between a city
// that stays put when the road is nudged and one that reshuffles itself every
// rebuild -- the same lesson the vegetation's per-cell hash learned.

std::uint32_t hashU(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
std::uint32_t hash2(std::uint32_t a, std::uint32_t b) { return hashU(a ^ hashU(b)); }
std::uint32_t hash3(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    return hashU(a ^ hashU(b ^ hashU(c)));
}
// 0..1 from a hash.
float unit(std::uint32_t h) { return static_cast<float>(h & 0xffffffU) / 16777215.0f; }
// -1..1
float sym(std::uint32_t h) { return unit(h) * 2.0f - 1.0f; }

// Smooth value noise along one axis -- the skyline swell. Two octaves is enough
// to break the sine-wave regularity a single one has.
float noise1(float x, std::uint32_t seed) {
    auto oct = [&](float p, std::uint32_t s) {
        const float fi = std::floor(p);
        const float f  = p - fi;
        const float w  = f * f * (3.0f - 2.0f * f);
        const auto  i  = static_cast<std::int32_t>(fi);
        const float a  = unit(hash2(static_cast<std::uint32_t>(i), s));
        const float b  = unit(hash2(static_cast<std::uint32_t>(i + 1), s));
        return a + (b - a) * w;
    };
    return oct(x, seed) * 0.68f + oct(x * 2.37f + 11.3f, seed ^ 0x9e3779b9U) * 0.32f;
}

// Largest component of a half-extent -- the radius of the sphere that contains a
// box however it is turned. Spelled out rather than pulled from glm's
// component_wise, which is a GTX header and needs GLM_ENABLE_EXPERIMENTAL.
float maxAxis(const glm::vec3& v) {
    return std::max(std::abs(v.x), std::max(std::abs(v.y), std::abs(v.z)));
}

// A yaw about +Y applied to a point in the XZ plane, matching the engine's
// Euler-Y convention (the same helper BuildingGen uses, for the same reason:
// the generator's parts are laid out in the building's own frame and have to be
// lifted into the world by hand -- there is no parent transform to do it).
glm::vec2 yawXZ(glm::vec2 v, float deg) {
    const float r = glm::radians(deg), c = std::cos(r), s = std::sin(r);
    return {v.x * c + v.y * s, -v.x * s + v.y * c};
}

// --- The centreline, as something you can ask questions of ------------------
// The road hands us a dense polyline. Parcelling needs arc-length lookup and
// curvature, so build that once instead of re-walking the polyline per lot.
struct Curve {
    std::vector<glm::vec2> p;
    std::vector<float>     y;      // carriageway height per point
    std::vector<float>     s;      // cumulative arc length
    float                  total = 0.0f;

    // Position, unit direction and road height at arc length `q` (clamped).
    void at(float q, glm::vec2& pos, glm::vec2& dir, float& roadY) const {
        q = glm::clamp(q, 0.0f, total);
        // s is monotonic, so a binary search finds the segment.
        std::size_t i = static_cast<std::size_t>(
            std::lower_bound(s.begin(), s.end(), q) - s.begin());
        if (i == 0) i = 1;
        if (i >= p.size()) i = p.size() - 1;
        const float seg = std::max(s[i] - s[i - 1], 1e-5f);
        const float t   = glm::clamp((q - s[i - 1]) / seg, 0.0f, 1.0f);
        pos   = p[i - 1] + (p[i] - p[i - 1]) * t;
        roadY = y[i - 1] + (y[i] - y[i - 1]) * t;
        const glm::vec2 d = p[i] - p[i - 1];
        const float     l = glm::length(d);
        dir = (l > 1e-5f) ? d / l : glm::vec2(0.0f, 1.0f);
    }

    // Signed curvature (1/m) at `q`, measured over a baseline long enough not to
    // pick up the polyline's own faceting. Positive turns AWAY from `perp`
    // below, which is what makes the offset-scale formula in placeSide read the
    // right way round.
    float curvature(float q) const {
        constexpr float kH = 9.0f;
        glm::vec2 pa, da, pb, db; float ya, yb;
        at(q - kH, pa, da, ya);
        at(q + kH, pb, db, yb);
        const float cross = da.x * db.y - da.y * db.x;
        const float dot   = da.x * db.x + da.y * db.y;
        return std::atan2(cross, dot) / (2.0f * kH);
    }
};

Curve makeCurve(const std::vector<glm::vec2>& center, const std::vector<float>& centerY) {
    Curve c;
    c.p = center;
    c.y = centerY;
    // A road saved before carriageway heights existed (or a preview) can hand us
    // a short/empty height array; fall back to flat rather than reading past it.
    c.y.resize(c.p.size(), c.y.empty() ? 0.0f : c.y.back());
    c.s.resize(c.p.size(), 0.0f);
    for (std::size_t i = 1; i < c.p.size(); ++i)
        c.s[i] = c.s[i - 1] + glm::length(c.p[i] - c.p[i - 1]);
    c.total = c.s.empty() ? 0.0f : c.s.back();
    return c;
}

// Left of travel in XZ. Same convention as roadside::generate, so "Left" means
// the same edge in both panels: the LEFT side is at -perp, the RIGHT at +perp.
glm::vec2 perpOf(glm::vec2 dir) { return {dir.y, -dir.x}; }

// The biome that owns arc-length station `q`: the first ENABLED one whose range
// contains it. -1 for bare road. Order decides overlaps, deliberately -- see the
// note on Biome.
int biomeAt(const std::vector<Biome>& bs, float q, float total) {
    for (std::size_t i = 0; i < bs.size(); ++i) {
        const Biome& b = bs[i];
        if (!b.enabled) continue;
        const float to = (b.to > b.from) ? b.to : total;
        if (q >= b.from && q < to) return static_cast<int>(i);
    }
    return -1;
}

// 0 at the biome's boundary, 1 once `blend` metres inside it. Thins the frontage
// and lowers the towers towards a district's edge, so downtown peters out
// instead of ending in a cliff.
//
// Only an end that meets BARE ROAD fades. Where two districts abut, fading both
// of them would open a hole between them -- and the character change from one
// ruleset to the next is the transition; it doesn't need a gap to announce it.
float edgeFade(const std::vector<Biome>& bs, int self, float q, float total) {
    const Biome& b = bs[self];
    if (b.blend <= 0.1f) return 1.0f;
    const float to = (b.to > b.from) ? b.to : total;
    float f = 1.0f;
    if (biomeAt(bs, b.from - 1.0f, total) < 0)
        f = std::min(f, (q - b.from) / b.blend);
    if (to < total && biomeAt(bs, to + 1.0f, total) < 0)
        f = std::min(f, (to - q) / b.blend);
    return glm::clamp(f, 0.0f, 1.0f);
}

} // namespace

const char* presetName(Preset p) {
    switch (p) {
        case Preset::Downtown:   return "Downtown";
        case Preset::Canyon:     return "Street canyon";
        case Preset::Industrial: return "Industrial";
        case Preset::Slums:      return "Slums";
        case Preset::Megablock:  return "Megablock";
        case Preset::Outskirts:  return "Outskirts";
        case Preset::Count:      break;
    }
    return "Biome";
}

bool Biome::operator==(const Biome& o) const {
    for (int i = 0; i < kStyleCount; ++i)
        if (styleMix[i] != o.styleMix[i]) return false;
    return enabled == o.enabled && name == o.name && from == o.from && to == o.to &&
           blend == o.blend && side == o.side && setback == o.setback &&
           setbackVar == o.setbackVar && frontage == o.frontage &&
           frontageVar == o.frontageVar && gap == o.gap && depth == o.depth &&
           depthVar == o.depthVar && fill == o.fill && blockLength == o.blockLength &&
           blockGap == o.blockGap && maxSlope == o.maxSlope && maxDrop == o.maxDrop &&
           floorsMin == o.floorsMin && floorsMax == o.floorsMax &&
           floorHeight == o.floorHeight && skylineScale == o.skylineScale &&
           skylineBias == o.skylineBias && twist == o.twist &&
           skywayEvery == o.skywayEvery && skywayHeight == o.skywayHeight &&
           skywayWidth == o.skywayWidth && signChance == o.signChance &&
           palette == o.palette && glassTint == o.glassTint &&
           frameTint == o.frameTint && accentColor == o.accentColor &&
           baseTint == o.baseTint && accentStrength == o.accentStrength &&
           neon == o.neon && collider == o.collider && seed == o.seed &&
           weathering == o.weathering && grime == o.grime &&
           clutter == o.clutter && clutterVar == o.clutterVar &&
           deadNeon == o.deadNeon;
}

Biome preset(Preset p) {
    Biome b;
    auto mix = [&b](std::initializer_list<float> w) {
        int i = 0;
        for (float v : w) { if (i < kStyleCount) b.styleMix[i++] = v; }
        for (; i < kStyleCount; ++i) b.styleMix[i] = 0.0f;
    };
    // Style order is buildings::Style: Skyscraper, NeoTokyo, Arcology, Needle,
    // Slab, Habitat.
    switch (p) {
        case Preset::Downtown:
            b.name = "Downtown";
            b.setback = 10.0f; b.setbackVar = 4.0f;
            b.frontage = 30.0f; b.frontageVar = 12.0f; b.gap = 7.0f;
            b.depth = 28.0f; b.depthVar = 10.0f; b.fill = 0.88f;
            b.blockLength = 220.0f; b.blockGap = 18.0f;
            b.floorsMin = 12; b.floorsMax = 44; b.skylineScale = 240.0f;
            mix({3, 1, 0, 1, 2, 1});
            b.signChance = 0.30f;
            b.weathering = 0.42f; b.grime = 0.20f;
            b.clutter = 4; b.deadNeon = 0.18f;
            break;
        case Preset::Canyon:
            // Tall, deep, and hard against both kerbs with almost no daylight
            // between neighbours: a continuous wall on each side, which is what
            // makes the road below read as a canyon floor rather than a street
            // with buildings near it.
            b.name = "Street canyon";
            b.setback = 3.0f; b.setbackVar = 1.0f;
            b.frontage = 24.0f; b.frontageVar = 7.0f; b.gap = 1.0f;
            b.depth = 34.0f; b.depthVar = 8.0f; b.fill = 0.99f;
            b.blockLength = 0.0f;
            b.floorsMin = 26; b.floorsMax = 68; b.skylineScale = 170.0f;
            b.skylineBias = 0.68f; b.twist = 25.0f;
            mix({2, 4, 0, 2, 1, 1});
            b.skywayEvery = 140.0f; b.skywayHeight = 46.0f; b.skywayWidth = 7.0f;
            b.signChance = 0.65f;
            b.accentColor = {0.95f, 0.25f, 0.55f};
            b.glassTint = {0.05f, 0.07f, 0.11f};
            // Lived-in rather than derelict: a working street, just one nobody
            // has cleaned in a decade. Enough dead signage to make the lit ones
            // count.
            b.weathering = 0.66f; b.grime = 0.38f;
            b.clutter = 8; b.deadNeon = 0.28f;
            break;
        case Preset::Industrial:
            b.name = "Industrial";
            b.setback = 14.0f; b.setbackVar = 6.0f;
            b.frontage = 46.0f; b.frontageVar = 16.0f; b.gap = 12.0f;
            b.depth = 40.0f; b.depthVar = 12.0f; b.fill = 0.7f;
            b.floorsMin = 3; b.floorsMax = 10; b.skylineScale = 300.0f;
            b.skylineBias = 0.3f;
            mix({0, 0, 1, 0, 5, 0});
            b.signChance = 0.12f;
            b.accentColor = {1.00f, 0.62f, 0.12f};
            b.baseTint = {0.26f, 0.25f, 0.23f};
            b.frameTint = {0.22f, 0.20f, 0.18f};
            // Almost nothing glazed, everything rusted, roofs buried in plant.
            b.weathering = 0.92f; b.grime = 0.80f;
            b.clutter = 14; b.clutterVar = 0.4f; b.deadNeon = 0.6f;
            break;
        case Preset::Slums:
            b.name = "Slums";
            b.setback = 2.5f; b.setbackVar = 2.0f;
            b.frontage = 12.0f; b.frontageVar = 6.0f; b.gap = 1.5f;
            b.depth = 14.0f; b.depthVar = 6.0f; b.fill = 0.95f;
            b.blockLength = 70.0f; b.blockGap = 9.0f;
            b.floorsMin = 2; b.floorsMax = 12; b.skylineScale = 90.0f;
            b.skylineBias = 0.35f;
            mix({1, 2, 0, 0, 3, 1});
            b.signChance = 0.55f;
            b.accentColor = {0.30f, 1.00f, 0.55f};
            b.glassTint = {0.13f, 0.12f, 0.10f};
            b.baseTint = {0.24f, 0.22f, 0.20f};
            // The bottom of the scale: raw block, no maintenance, roofs used as
            // storage. The signage is the one thing still cared about, so it is
            // the LEAST dead here -- that contrast is the whole look.
            b.weathering = 1.0f; b.grime = 0.85f;
            b.clutter = 12; b.clutterVar = 0.85f; b.deadNeon = 0.30f;
            break;
        case Preset::Megablock:
            b.name = "Megablock";
            b.setback = 6.0f; b.setbackVar = 1.0f;
            b.frontage = 78.0f; b.frontageVar = 14.0f; b.gap = 2.0f;
            b.depth = 70.0f; b.depthVar = 10.0f; b.fill = 1.0f;
            b.blockLength = 320.0f; b.blockGap = 26.0f;
            b.floorsMin = 22; b.floorsMax = 40; b.skylineScale = 400.0f;
            mix({1, 0, 5, 0, 2, 1});
            b.skywayEvery = 190.0f; b.skywayHeight = 62.0f; b.skywayWidth = 11.0f;
            b.signChance = 0.25f;
            b.accentColor = {0.55f, 0.35f, 1.00f};
            b.weathering = 0.55f; b.grime = 0.45f;
            b.clutter = 10; b.deadNeon = 0.22f;
            break;
        case Preset::Outskirts:
            b.name = "Outskirts";
            b.setback = 26.0f; b.setbackVar = 14.0f;
            b.frontage = 40.0f; b.frontageVar = 18.0f; b.gap = 40.0f;
            b.depth = 24.0f; b.depthVar = 10.0f; b.fill = 0.45f;
            b.floorsMin = 8; b.floorsMax = 55; b.skylineScale = 500.0f;
            b.skylineBias = 0.4f;
            mix({1, 1, 0, 4, 1, 2});
            b.signChance = 0.1f;
            b.weathering = 0.5f; b.grime = 0.3f;
            b.clutter = 3; b.deadNeon = 0.4f;
            break;
        case Preset::Count:
            break;
    }
    return b;
}

std::vector<buildings::Palette> ensurePalettes(std::vector<MaterialDef>& materials,
                                               const std::vector<Biome>& biomes) {
    std::vector<buildings::Palette> out;
    out.reserve(biomes.size());
    for (const Biome& b : biomes) {
        // ensurePalette reads only the look block, so a throwaway Params carrying
        // it is all it needs -- and keeps Biome from having to *be* a Params.
        buildings::Params p;
        p.palette        = b.palette;
        p.glassTint      = b.glassTint;
        p.frameTint      = b.frameTint;
        p.accentColor    = b.accentColor;
        p.baseTint       = b.baseTint;
        p.accentStrength = b.accentStrength;
        p.weathering     = b.weathering;   // drives the reflectivity/roughness pair
        out.push_back(buildings::ensurePalette(materials, p));
    }
    return out;
}

namespace {

// --- Merging ----------------------------------------------------------------
// Metres of road per batch chunk. Short enough that culling a chunk still means
// something, long enough that the whole city is a few hundred draws.
constexpr float kChunkLength = 160.0f;

// The unit primitive each piece type is an instance of, as CPU data. Built once.
//
// Deliberately coarser than the meshes the entity renderer uses for the same
// shapes, because a merged district pays for these in FULL -- an instance costs
// one draw and shares one buffer, a merged copy costs its vertices every time.
// The box is indexed (24 vertices, not 36 triangle corners), the cylinder drops
// from 20 segments to 12, and the sphere from 24x32 to 8x12: the only spheres in
// a city are crown domes and beacons, and nobody reads the smoothness of one
// from the street.
// The unit cube with shared corners per face: 24 vertices and 36 indices rather
// than 36 standalone triangle corners. A box is by far the commonest piece in a
// city, so a third off it is a third off the whole district's memory.
fitzel::MeshData indexedCube() {
    fitzel::MeshData md;
    const std::vector<fitzel::Vertex> tris = makeCubeVerts(); // 6 faces x 6 corners
    md.vertices.reserve(24);
    md.indices.reserve(36);
    for (std::uint32_t f = 0; f < 6; ++f) {
        // makeCubeVerts lays each face out as 0,1,2, 2,3,0 -- so corners 0,1,2
        // and 4 of the six are the quad's four distinct vertices.
        const std::size_t o = f * 6;
        const std::uint32_t base = static_cast<std::uint32_t>(md.vertices.size());
        md.vertices.push_back(tris[o + 0]);
        md.vertices.push_back(tris[o + 1]);
        md.vertices.push_back(tris[o + 2]);
        md.vertices.push_back(tris[o + 4]);
        for (std::uint32_t i : {0u, 1u, 2u, 2u, 3u, 0u}) md.indices.push_back(base + i);
    }
    return md;
}

const fitzel::MeshData& unitMesh(EntityType t) {
    // Non-indexed sources get sequential indices so every primitive here has the
    // same shape and appendPiece needs only one code path.
    auto seq = [](std::vector<fitzel::Vertex> v) {
        fitzel::MeshData md;
        md.vertices = std::move(v);
        md.indices.resize(md.vertices.size());
        for (std::size_t i = 0; i < md.indices.size(); ++i)
            md.indices[i] = static_cast<std::uint32_t>(i);
        return md;
    };
    static const fitzel::MeshData box    = indexedCube();
    static const fitzel::MeshData cyl    = seq(makeCylinderYVerts(12));
    static const fitzel::MeshData sphere = seq(makeSphereVerts(8, 12));
    static const fitzel::MeshData ramp   = seq(makeRampVerts());
    switch (t) {
        case EntityType::Cylinder: return cyl;
        case EntityType::Sphere:   return sphere;
        case EntityType::Ramp:     return ramp;
        default:                   return box;
    }
}

// Append one piece's geometry, transformed into world space, to `md`.
//
// No reserve() here on purpose. Reserving exactly what this one piece needs
// leaves capacity == size, so the NEXT piece reallocates and copies the whole
// buffer -- which turned a 4 ms district into a 2.3 second one. The caller sizes
// each batch once, up front (see the merge in generate()).
void appendPiece(fitzel::MeshData& md, const Piece& pc, glm::vec3& lo, glm::vec3& hi) {
    const fitzel::MeshData& src = unitMesh(pc.type);
    const float r = glm::radians(pc.yaw), c = std::cos(r), s = std::sin(r);
    // The engine's Euler-Y convention, the same one yawXZ above uses.
    auto rot = [&](glm::vec3 v) {
        return glm::vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
    };
    const glm::vec3 scale = pc.half * 2.0f;
    // Normals transform by R * S^-1, not by the model matrix: a non-uniform
    // scale would otherwise tilt them and light a squashed box as if it were a
    // cube. R is orthogonal, so it needs no inverse.
    const glm::vec3 invS = 1.0f / glm::max(glm::abs(scale), glm::vec3(1e-4f));

    const auto base = static_cast<std::uint32_t>(md.vertices.size());
    for (const fitzel::Vertex& sv : src.vertices) {
        fitzel::Vertex v = sv;
        v.position = rot(v.position * scale) + pc.center;
        v.normal   = glm::normalize(rot(v.normal * invS));
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
        md.vertices.push_back(v);
    }
    for (std::uint32_t idx : src.indices) md.indices.push_back(base + idx);
}

// The style a lot draws from the biome's mix.
buildings::Style pickStyle(const Biome& b, std::uint32_t h) {
    float sum = 0.0f;
    for (int i = 0; i < kStyleCount; ++i) sum += std::max(b.styleMix[i], 0.0f);
    if (sum <= 1e-4f) return buildings::Style::Skyscraper;
    float r = unit(h) * sum;
    for (int i = 0; i < kStyleCount; ++i) {
        r -= std::max(b.styleMix[i], 0.0f);
        if (r <= 0.0f) return static_cast<buildings::Style>(i);
    }
    return buildings::Style::Skyscraper;
}

// Flatten one generated tower into world-space pieces. BuildingGen lays its parts
// out in the building's own frame with a Y-only rotation each, so lifting them
// into the world is one yaw and one translate -- no matrices, and nothing that
// can disagree with what a baked copy would look like (bake() runs the same
// generator with the same params).
void flatten(const std::vector<Entity>& es, const glm::vec3& ground, float yawDeg,
             std::vector<Piece>& out, glm::vec3& lo, glm::vec3& hi) {
    for (std::size_t i = 1; i < es.size(); ++i) { // [0] is the Empty root
        const Entity& e = es[i];
        if (e.type == EntityType::Empty) continue;
        // Point lights are dropped: one per tower would blow the renderer's
        // shadowed-light budget several hundred times over. The crown's neon is
        // emissive material, which costs nothing and reads at night anyway.
        if (e.type == EntityType::Light || e.type == EntityType::Sun) continue;

        Piece pc;
        const glm::vec2 r = yawXZ({e.localCenter.x, e.localCenter.z}, yawDeg);
        pc.center = ground + glm::vec3(r.x, e.localCenter.y, r.y);
        pc.half   = e.half;
        pc.yaw    = e.localRotation.y + yawDeg;
        pc.type   = e.type;
        if (const auto* mc = e.components.get<MaterialComponent>()) pc.material = mc->material;
        pc.collide = e.components.get<PhysicsComponent>() != nullptr;
        // Conservative bounds: the half-extents unrotated by the largest axis, so
        // a twisted mass still sits inside the sphere the culler tests.
        const float m = maxAxis(pc.half);
        lo = glm::min(lo, pc.center - m);
        hi = glm::max(hi, pc.center + m);
        out.push_back(pc);
    }
}

} // namespace

District generate(const std::vector<glm::vec2>& center,
                  const std::vector<float>& centerY, float halfWidth,
                  const std::vector<Biome>& biomes,
                  const std::vector<buildings::Palette>& palettes,
                  const std::function<float(float, float)>& groundAt,
                  int maxBuildings) {
    District out;
    if (center.size() < 2 || biomes.empty() || palettes.size() != biomes.size())
        return out;

    const Curve cv = makeCurve(center, centerY);
    if (cv.total < 20.0f) return out;

    // Every piece the walk produces, tagged with the chunk it belongs to. This
    // list is transient: it is merged into `out.batches` at the end and thrown
    // away, so the district never holds a per-piece copy of a city.
    std::vector<Piece> pcs;
    std::vector<int>   pcChunk;
    auto tag = [&](float station) {
        pcChunk.resize(pcs.size(), static_cast<int>(station / kChunkLength));
    };

    auto ground = [&](float x, float z) { return groundAt ? groundAt(x, z) : 0.0f; };

    // Both sides are walked independently: a canyon is two frontages that happen
    // to face each other, not one alternating run. Doing it this way also means
    // "Left only" costs exactly half, with no special case.
    for (int sideIdx = 0; sideIdx < 2; ++sideIdx) {
        const bool  right = (sideIdx == 1);
        const float sgn   = right ? 1.0f : -1.0f;   // along perp
        // Buildings sit with their local X along the road (so `width` is the
        // frontage) and their local -Z facing the carriageway. That is a quarter
        // turn off the road's heading, flipped for the far side.
        const float faceTurn = right ? 90.0f : -90.0f;

        float q = 0.0f;                 // station of the next parcel's near edge
        int   lotIndex = 0;
        while (q < cv.total) {
            if (static_cast<int>(out.buildings.size()) >= maxBuildings) {
                out.budgetHit = true;
                break;
            }
            const int bi = biomeAt(biomes, q, cv.total);
            if (bi < 0) { q += 12.0f; continue; }   // bare road: skip ahead cheaply
            const Biome& B = biomes[bi];
            if (B.side != Side::Both && B.side != (right ? Side::Right : Side::Left)) {
                q += 12.0f;
                continue;
            }

            const std::uint32_t h0 =
                hash3(B.seed, static_cast<std::uint32_t>(sideIdx),
                      static_cast<std::uint32_t>(lotIndex) * 2654435761U);
            ++lotIndex;

            // --- Parcel size ------------------------------------------------
            const float fade = edgeFade(biomes, bi, q, cv.total);
            float frontage = std::max(4.0f, B.frontage + sym(hashU(h0)) * B.frontageVar);
            float lotDepth = std::max(5.0f, B.depth + sym(hashU(h0 ^ 0x51U)) * B.depthVar);
            const float setback =
                std::max(0.0f, B.setback + sym(hashU(h0 ^ 0xa7U)) * B.setbackVar);

            // The style is picked here, before the plot is placed, because it
            // decides how DEEP the building actually is: a slab fills a fraction
            // of its plot, a cross-plan arcology all of it. The facade has to sit
            // at `setback` whatever that works out to, so the building is centred
            // on its own depth -- centring it on the plot's would push a shallow
            // style back off the street and open a canyon wall into a boulevard.
            buildings::Params bp;
            const buildings::Style style = pickStyle(B, hashU(h0 ^ 0x77U));
            buildings::applyStyle(bp, style);
            const float aspect     = bp.depth / std::max(bp.width, 1e-3f);
            const int   styleFloor = bp.floors;
            const float bodyDepth  = glm::clamp(frontage * aspect, 5.0f, lotDepth);

            // --- Cross streets ----------------------------------------------
            // A block runs `blockLength` metres and is then cut by a `blockGap`
            // wide slot. A parcel that would straddle the slot is pushed past it
            // rather than shortened, so the corner building keeps its proportions.
            if (B.blockLength > 1.0f) {
                const float period = B.blockLength + std::max(B.blockGap, 1.0f);
                const float phase  = std::fmod(q - B.from, period);
                if (phase >= B.blockLength) { q += period - phase; continue; }
                if (frontage > B.blockLength - phase) {
                    q += (B.blockLength - phase) + std::max(B.blockGap, 1.0f);
                    continue;
                }
            }

            // --- Curvature: how much centreline a parcel eats at this offset --
            // The offset curve is longer on the outside of a bend and shorter on
            // the inside, by exactly (1 + offset*curvature). Advancing by the
            // scaled amount is what keeps a canyon's wall continuous round a bend
            // instead of gapping on the outside and interpenetrating on the
            // inside -- no per-pair overlap test needed.
            const float kappa   = cv.curvature(q + frontage * 0.5f);
            const float offNear = sgn * (halfWidth + setback);
            const float offFar  = sgn * (halfWidth + setback + bodyDepth);
            const float scaleNear = 1.0f + offNear * kappa;
            const float scaleFar  = 1.0f + offFar * kappa;
            if (scaleNear < 0.2f || scaleFar < 0.2f) {
                // Inside of a bend tighter than the parcel is deep: the plot folds
                // through itself. Step on rather than place a knot of geometry.
                ++out.skippedTight;
                q += std::max(frontage * 0.4f, 4.0f);
                continue;
            }
            const float advance = (frontage + B.gap) / scaleNear;

            // --- Empty plots -------------------------------------------------
            if (unit(hashU(h0 ^ 0x1dU)) > B.fill * (0.25f + 0.75f * fade)) {
                q += advance;
                continue;
            }

            // --- Where it stands ---------------------------------------------
            glm::vec2 c, dir; float roadY;
            cv.at(q + frontage * 0.5f, c, dir, roadY);
            const glm::vec2 perp = perpOf(dir);
            const glm::vec2 pos  = c + perp * (sgn * (halfWidth + setback + bodyDepth * 0.5f));
            const float yawDeg   = glm::degrees(std::atan2(dir.x, dir.y)) + faceTurn;

            // Ground fit: the four footprint corners decide whether the plot is
            // buildable at all, and the lowest one is where the building sits --
            // buried a little on the high side (BuildingGen's `sink` hides that
            // seam) rather than floating on the low one.
            float gLo = 1e9f, gHi = -1e9f;
            for (int k = 0; k < 4; ++k) {
                const glm::vec2 corner =
                    pos + perp * (sgn * ((k & 1) ? bodyDepth : -bodyDepth) * 0.5f) +
                    dir * ((k & 2) ? frontage : -frontage) * 0.5f;
                const float g = ground(corner.x, corner.y);
                gLo = std::min(gLo, g);
                gHi = std::max(gHi, g);
            }
            // Measured across the footprint's DIAGONAL, which is the distance the
            // two extreme corners are actually apart -- using the short side
            // instead reads a gentle cross-fall as a cliff and empties a hillside
            // that would have built fine.
            const float run = std::max(std::sqrt(frontage * frontage +
                                                 bodyDepth * bodyDepth), 1.0f);
            if ((gHi - gLo) / run > std::max(B.maxSlope, 0.01f)) {
                ++out.skippedSlope;
                q += advance;
                continue;
            }
            // Keep the ground floors addressing the street: a plot that has fallen
            // away below the carriageway is propped back up to within maxDrop, so
            // a canyon crossing a dip still has walls rather than a pit.
            const float baseY = std::max(gLo, roadY - std::max(B.maxDrop, 0.0f));

            // --- Massing ------------------------------------------------------
            // Height: the smooth swell along the road, broken up per lot, biased,
            // faded towards the district edge, and finally nudged by how tall the
            // chosen style wants to be (a needle out-tops a slab on the same plot).
            float t = noise1(q / std::max(B.skylineScale, 8.0f), B.seed ^ 0x2f6a1bU);
            t = glm::clamp(t * 0.75f + unit(hashU(h0 ^ 0x93U)) * 0.25f, 0.0f, 1.0f);
            t = std::pow(t, std::exp((0.5f - glm::clamp(B.skylineBias, 0.0f, 1.0f)) * 2.4f));
            // How tall the chosen style wants to be biases WHERE IN THE RANGE the
            // lot lands, rather than scaling the result: a needle out-tops a slab
            // on the same plot, but Floors min/max stay the bounds the user set --
            // multiplying afterwards produced 91-storey towers in a 68-storey
            // district, which is not what a maximum means.
            const float styleK = glm::clamp(styleFloor / 40.0f, 0.55f, 1.6f);
            t = glm::clamp(t * styleK, 0.0f, 1.0f);
            const int   fMin  = std::max(1, std::min(B.floorsMin, B.floorsMax));
            const int   fMax  = std::max(fMin, std::max(B.floorsMin, B.floorsMax));
            float floors = fMin + (fMax - fMin) * t;
            floors *= 0.35f + 0.65f * fade;   // districts taper off at their edges
            bp.floors      = glm::clamp(static_cast<int>(std::lround(floors)), 1, 250);
            bp.floorHeight = B.floorHeight;
            // The parcel: the frontage is given, the depth is the style's aspect
            // capped at the plot (or an Arcology would swallow its neighbours).
            bp.width = frontage;
            bp.depth = bodyDepth;
            bp.twist = B.twist * sym(hashU(h0 ^ 0xc1U));
            bp.seed  = h0 | 1U;
            bp.sink  = std::max(0.4f, (gHi - gLo) * 0.6f); // bury the high corner
            // Look: the biome's, not the style's -- one district, one palette.
            bp.palette        = B.palette;
            bp.glassTint      = B.glassTint;
            bp.frameTint      = B.frameTint;
            bp.accentColor    = B.accentColor;
            bp.baseTint       = B.baseTint;
            bp.accentStrength = B.accentStrength;
            bp.collider       = B.collider;
            bp.beaconLight    = false;  // see flatten(): no point light per tower

            // --- Wear ---------------------------------------------------------
            // Signage that has failed is per building, not per district: a run of
            // towers where a third of the signs are dark is what makes the lit
            // ones read as lit. `neon` also gates the projecting sign below, so a
            // dead building gets no glowing blade over the street either.
            const bool lit = B.neon && unit(hashU(h0 ^ 0xe1U)) >= B.deadNeon;
            bp.neon       = lit;
            bp.weathering = B.weathering;
            bp.grime      = B.grime;
            // Clutter varies per roof so the district isn't uniformly littered --
            // a couple of swept roofs make the buried ones read as buried.
            bp.clutter = static_cast<int>(std::lround(
                B.clutter * glm::mix(1.0f, unit(hashU(h0 ^ 0x2aU)) * 2.0f,
                                     glm::clamp(B.clutterVar, 0.0f, 1.0f))));
            // A neglected stack is an irregular one, and a good few have lost
            // whatever was on the roof.
            bp.jitter = glm::clamp(bp.jitter + B.weathering * 0.35f, 0.0f, 1.0f);
            if (unit(hashU(h0 ^ 0x6fU)) < B.weathering * 0.35f)
                bp.crown = buildings::Crown::None;

            // --- Build it -----------------------------------------------------
            District::Building rec;
            const std::size_t pieces0 = pcs.size();
            rec.ground  = glm::vec3(pos.x, baseY, pos.y);
            rec.yaw     = yawDeg;
            rec.biome   = bi;
            rec.station = q;
            rec.params  = bp;

            int counter = 0;
            const std::vector<Entity> es =
                buildings::generate(bp, palettes[bi], counter, glm::vec3(0.0f));
            glm::vec3 lo(1e9f), hi(-1e9f);
            flatten(es, rec.ground, yawDeg, pcs, lo, hi);

            // --- A projecting sign ---------------------------------------------
            // Two emissive slabs on the street-facing face (local -Z): a blade
            // hanging over the carriageway and a vertical strip up the facade.
            // Cheap, and the single most legible "this is a sci-fi street" cue
            // there is -- at night the canyon lights itself.
            if (lit && unit(hashU(h0 ^ 0x3bU)) < B.signChance) {
                const float bodyH = bp.floors * bp.floorHeight;
                const float sy    = glm::clamp(bodyH * (0.18f + 0.5f * unit(hashU(h0 ^ 0x4dU))),
                                               5.0f, std::max(6.0f, bodyH - 4.0f));
                const float bladeW = std::min(frontage * 0.45f, 7.0f);
                const float reach  = 1.2f + 1.8f * unit(hashU(h0 ^ 0x5eU));
                auto place = [&](glm::vec3 local, glm::vec3 half) {
                    Piece pc;
                    const glm::vec2 r = yawXZ({local.x, local.z}, yawDeg);
                    pc.center   = rec.ground + glm::vec3(r.x, local.y, r.y);
                    pc.half     = half;
                    pc.yaw      = yawDeg;
                    pc.type     = EntityType::Box;
                    pc.material = palettes[bi].accent;
                    pcs.push_back(pc);
                    const float m = maxAxis(half);
                    lo = glm::min(lo, pc.center - m);
                    hi = glm::max(hi, pc.center + m);
                };
                const float faceZ = -bp.depth * 0.5f;
                place({0.0f, sy, faceZ - reach * 0.5f},
                      {bladeW * 0.5f, bladeW * 0.9f, reach * 0.5f});
                place({0.0f, sy * 0.55f, faceZ - 0.2f},
                      {0.35f, sy * 0.5f, 0.2f});
            }

            if (pcs.size() == pieces0) { q += advance; continue; }
            tag(q);
            rec.center = (lo + hi) * 0.5f;
            rec.radius = glm::length(hi - lo) * 0.5f;
            out.buildings.push_back(rec);
            ++out.placed;

            q += advance;
        }
    }

    // --- Skyways -----------------------------------------------------------
    // Walked over the road itself, not per side: a skyway is a piece of the
    // street. One deck plus a neon underside, spanning facade to facade.
    for (std::size_t i = 0; i < biomes.size(); ++i) {
        const Biome& B = biomes[i];
        if (!B.enabled || B.skywayEvery < 5.0f || B.side != Side::Both) continue;
        const float to = (B.to > B.from) ? B.to : cv.total;
        for (float q = B.from + B.skywayEvery * 0.5f; q < std::min(to, cv.total);
             q += B.skywayEvery) {
            glm::vec2 c, dir; float roadY;
            cv.at(q, c, dir, roadY);
            const std::uint32_t h = hash3(B.seed ^ 0xbeefU, static_cast<std::uint32_t>(i),
                                          static_cast<std::uint32_t>(q * 0.5f));
            const float span = 2.0f * (halfWidth + B.setback + B.depth * 0.35f);
            const float hgt  = roadY + B.skywayHeight * (0.75f + 0.5f * unit(h));
            const float yaw  = glm::degrees(std::atan2(dir.x, dir.y));
            const float wid  = std::max(B.skywayWidth, 1.5f);

            Piece deck;
            deck.center   = glm::vec3(c.x, hgt, c.y);
            deck.half     = {span * 0.5f, 1.1f, wid * 0.5f};
            deck.yaw      = yaw;
            deck.material = palettes[i].frame;
            pcs.push_back(deck);

            if (B.neon) {
                Piece strip = deck;
                strip.center.y -= 1.25f;
                strip.half      = {span * 0.49f, 0.18f, wid * 0.22f};
                strip.material  = palettes[i].accent;
                pcs.push_back(strip);
            }
            tag(q);
            // Its own record, so "bake nearest" can tell it apart from a tower.
            District::Building rec;
            rec.center = deck.center;
            rec.radius = span * 0.55f;
            rec.ground = deck.center;
            rec.yaw    = yaw;
            rec.biome  = static_cast<int>(i);
            rec.station = q;
            rec.params.floors = 0;   // marks it as street furniture, not a tower
            out.buildings.push_back(rec);
        }
    }

    // --- Merge ---------------------------------------------------------------
    // Every piece in a chunk that shares a material becomes one mesh. This is the
    // step that makes a district drawable: see the note on Batch for why the cost
    // is per draw rather than per triangle. The loose pieces are dropped here --
    // only the SOLID ones are kept, and only because Play needs colliders.
    out.parts = static_cast<int>(pcs.size());
    pcChunk.resize(pcs.size(), 0);   // a tag() missed by an early `continue`
    {
        // An ORDERED map, so the grouping is reproducible run to run: an unordered
        // one would leave the batch list (and therefore the draw order) at the
        // mercy of hash iteration, which makes a rebuild needlessly hard to
        // compare against the one before it. AssetId has operator<.
        std::map<std::pair<int, fitzel::AssetId>, std::size_t> index;
        std::vector<int> batchOf(pcs.size(), -1);
        std::vector<std::size_t> nVerts, nIndices;

        // Pass one: assign every piece to a batch and total up what that batch
        // will hold, so pass two can allocate each buffer exactly once.
        for (std::size_t i = 0; i < pcs.size(); ++i) {
            const Piece& pc = pcs[i];
            if (pc.collide) out.colliders.push_back(pc);
            if (!pc.material.valid()) continue;  // no material -> nothing to draw
            const auto key = std::make_pair(pcChunk[i], pc.material);
            auto it = index.find(key);
            if (it == index.end()) {
                it = index.emplace(key, out.batches.size()).first;
                Batch b;
                b.material = pc.material;
                b.lo = glm::vec3(1e9f);
                b.hi = glm::vec3(-1e9f);
                out.batches.push_back(std::move(b));
                nVerts.push_back(0);
                nIndices.push_back(0);
            }
            const std::size_t bi2 = it->second;
            batchOf[i] = static_cast<int>(bi2);
            const fitzel::MeshData& src = unitMesh(pc.type);
            nVerts[bi2]   += src.vertices.size();
            nIndices[bi2] += src.indices.size();
        }
        for (std::size_t b = 0; b < out.batches.size(); ++b) {
            out.batches[b].data.vertices.reserve(nVerts[b]);
            out.batches[b].data.indices.reserve(nIndices[b]);
        }
        // Pass two: fill them.
        for (std::size_t i = 0; i < pcs.size(); ++i) {
            if (batchOf[i] < 0) continue;
            Batch& b = out.batches[static_cast<std::size_t>(batchOf[i])];
            appendPiece(b.data, pcs[i], b.lo, b.hi);
        }
    }
    for (const Batch& b : out.batches)
        out.verts += static_cast<int>(b.data.vertices.size());

    return out;
}

std::vector<Entity> bake(const District& d, int buildingIndex,
                         const buildings::Palette& pal, int& entityCounter) {
    std::vector<Entity> out;
    if (buildingIndex < 0 || buildingIndex >= static_cast<int>(d.buildings.size()))
        return out;
    const District::Building& b = d.buildings[buildingIndex];
    if (b.params.floors <= 0) return out;   // a skyway, not a tower

    // The same generator with the same params: a baked tower is the derived one,
    // not a lookalike. The yaw goes on the ROOT -- the scene graph carries it down
    // to the parts, which is exactly what flatten() does by hand for the derived
    // copy.
    out = buildings::generate(b.params, pal, entityCounter, b.ground);
    if (out.empty()) return out;
    out.front().localRotation = out.front().rotation = glm::vec3(0.0f, b.yaw, 0.0f);
    out.front().name = "Building " + std::to_string(static_cast<int>(b.station)) + "m";
    return out;
}

} // namespace city
