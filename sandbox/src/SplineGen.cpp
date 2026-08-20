#include "SplineGen.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>

#include "Primitives.hpp"   // makeCubeVerts

using fitzel::AssetId;

namespace splinegen {
namespace {

// --- Small shared helpers ----------------------------------------------------

// A deterministic 0..1 from an integer, so the per-post jitter is a function of
// WHICH post it is rather than of the order they were emitted in. Nudging a
// control point must not reshuffle a fence the author has been looking at.
float unitHash(std::uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352dU;
    h ^= h >> 15; h *= 0x846ca68bU;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 16777215.0f;
}

// The unit cube with shared corners per face: 24 vertices and 36 indices rather
// than 36 standalone triangle corners. A post is by far the commonest piece
// here, so a third off it is a third off the whole run's memory. (Same trade
// CityGen's indexedCube makes -- and for the same reason it is a copy: this is
// six lines, and sharing it would mean a header that drags the city in.)
const fitzel::MeshData& unitCube() {
    static const fitzel::MeshData cube = [] {
        fitzel::MeshData md;
        const std::vector<fitzel::Vertex> tris = makeCubeVerts(); // 6 faces x 6 corners
        md.vertices.reserve(24);
        md.indices.reserve(36);
        for (std::uint32_t face = 0; face < 6; ++face) {
            // makeCubeVerts lays each face out as 0,1,2, 2,3,0 -- so corners
            // 0,1,2 and 4 of the six are the quad's four distinct vertices.
            const std::size_t   o    = face * 6;
            const std::uint32_t base = static_cast<std::uint32_t>(md.vertices.size());
            md.vertices.push_back(tris[o + 0]);
            md.vertices.push_back(tris[o + 1]);
            md.vertices.push_back(tris[o + 2]);
            md.vertices.push_back(tris[o + 4]);
            for (std::uint32_t i : {0u, 1u, 2u, 2u, 3u, 0u}) md.indices.push_back(base + i);
        }
        return md;
    }();
    return cube;
}

// One drawable slot: geometry plus the AABB that grows with it.
struct Slot {
    fitzel::MeshData data;
    glm::vec3        lo{1e30f}, hi{-1e30f};
    bool empty() const { return data.vertices.empty(); }
};

// A box, rotated about +Y and merged in world space. `yaw` in radians, in the
// engine's Euler-Y convention (0 = the box's local +Z points at world +Z), the
// same one the entity renderer and CityGen use.
//
// `tile` is metres per texture repeat. The cube's own 0..1 face UVs are thrown
// away and replaced by a planar mapping in the box's own space, because a shared
// texture has to read at ONE scale: with face UVs a 3 m merlon and a 0.02 m
// picket wear the same image stretched over wildly different areas, and a brick
// map turns into a smear on one and a single brick on the other.
void appendBox(Slot& sl, const glm::vec3& center, const glm::vec3& half, float yaw,
               float tile) {
    const fitzel::MeshData& src = unitCube();
    const float c = std::cos(yaw), s = std::sin(yaw);
    auto rot = [&](glm::vec3 v) {
        return glm::vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
    };
    const glm::vec3 scale = half * 2.0f;
    // Normals transform by R * S^-1, not by the model matrix: a non-uniform scale
    // would otherwise tilt them and light a fence post as if it were a cube.
    const glm::vec3 invS = 1.0f / glm::max(glm::abs(scale), glm::vec3(1e-4f));

    const float t = std::max(tile, 0.05f);
    const auto base = static_cast<std::uint32_t>(sl.data.vertices.size());
    for (const fitzel::Vertex& sv : src.vertices) {
        fitzel::Vertex v = sv;
        const glm::vec3 local = sv.position * scale;   // metres from the box centre
        // Project on the two axes the face does NOT face along.
        const glm::vec3 an = glm::abs(sv.normal);
        v.uv = (an.y > an.x && an.y > an.z) ? glm::vec2(local.x, local.z) / t
             : (an.x > an.z)                ? glm::vec2(local.z, local.y) / t
                                            : glm::vec2(local.x, local.y) / t;
        v.position = rot(local) + center;
        v.normal   = glm::normalize(rot(sv.normal * invS));
        sl.lo = glm::min(sl.lo, v.position);
        sl.hi = glm::max(sl.hi, v.position);
        sl.data.vertices.push_back(v);
    }
    for (std::uint32_t i : src.indices) sl.data.indices.push_back(base + i);
}

// --- Path frames -------------------------------------------------------------

// One sample of the path with the axes a cross-section is expressed in.
//
// The frame is deliberately UPRIGHT: the tangent is taken in plan only and `up`
// is always world up. A wall on a hillside stands plumb and a ballast bed stays
// level across the track, which is what both actually do -- rolling the section
// with the gradient would lean every post downhill.
struct Frame {
    glm::vec3 p;        // world, ground + lift
    glm::vec3 t;        // unit tangent in plan (y = 0)
    glm::vec3 r;        // unit right = cross(up, t)
    float     station;  // metres along the path (plan length)
    float     yaw;      // radians about +Y, for boxes placed on this frame
};

std::vector<Frame> makeFrames(const std::vector<glm::vec3>& path, bool closed) {
    std::vector<Frame> out;
    const std::size_t n = path.size();
    if (n < 2) return out;
    out.resize(n);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    float station = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        // Central difference, so a frame reflects the curve through it rather
        // than the segment after it -- otherwise every post on a bend is visibly
        // toed out. The ends fall back to the one segment they have (or wrap,
        // on a closed run).
        glm::vec3 a = path[i > 0 ? i - 1 : (closed ? n - 2 : 0)];
        glm::vec3 b = path[i + 1 < n ? i + 1 : (closed ? 1 : n - 1)];
        glm::vec3 d(b.x - a.x, 0.0f, b.z - a.z);
        if (glm::dot(d, d) < 1e-10f) {
            const glm::vec3 f = path[i + 1 < n ? i + 1 : i] - path[i > 0 ? i - 1 : i];
            d = glm::vec3(f.x, 0.0f, f.z);
        }
        if (glm::dot(d, d) < 1e-10f) d = glm::vec3(0.0f, 0.0f, 1.0f);
        d = glm::normalize(d);
        if (i > 0) {
            const glm::vec3 step = path[i] - path[i - 1];
            station += std::sqrt(step.x * step.x + step.z * step.z);
        }
        Frame& f = out[i];
        f.p       = path[i];
        f.t       = d;
        f.r       = glm::cross(up, d);   // +Z tangent -> +X right
        f.station = station;
        // atan2(x, z): the convention every placed object here uses (yaw 0 faces
        // +Z), so a box built on this frame lines up with roadside instances.
        f.yaw     = std::atan2(d.x, d.z);
    }
    return out;
}

// --- Sweeping a cross-section along the path ---------------------------------

// A closed cross-section in (lateral, up) metres relative to the frame origin,
// wound COUNTER-CLOCKWISE in that plane so the outward normal of edge a->b is
// (dy, -dx). Everything below builds one of these and hands it to sweep().
using Profile = std::vector<glm::vec2>;

// Triangulate a simple CCW polygon by ear clipping, as index triples into it.
//
// A triangle fan would be shorter and is what this had first, but it is only
// correct for a CONVEX section -- and the rail's is not: a fan from the foot's
// corner cuts straight across the notch beside the web, so every rail end came
// out with a dozen inverted, overlapping triangles. Sections are a dozen points
// at most, so the quadratic clip costs nothing worth measuring.
std::vector<glm::ivec3> triangulate(const Profile& prof) {
    std::vector<glm::ivec3> out;
    const int n = static_cast<int>(prof.size());
    if (n < 3) return out;
    auto cross2 = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };
    std::vector<int> poly(n);
    for (int i = 0; i < n; ++i) poly[i] = i;
    int guard = n * n + 8;   // a self-intersecting section must not hang the build
    while (static_cast<int>(poly.size()) > 3 && guard-- > 0) {
        bool clipped = false;
        const int m = static_cast<int>(poly.size());
        for (int i = 0; i < m; ++i) {
            const int ia = poly[(i + m - 1) % m], ib = poly[i], ic = poly[(i + 1) % m];
            const glm::vec2 a = prof[ia], b = prof[ib], c = prof[ic];
            if (cross2(b - a, c - b) <= 0.0f) continue;   // reflex: not an ear
            // ...and no other vertex may be inside it.
            bool empty = true;
            for (int j = 0; j < m && empty; ++j) {
                const int id = poly[j];
                if (id == ia || id == ib || id == ic) continue;
                const glm::vec2 p = prof[id];
                empty = !(cross2(b - a, p - a) >= 0.0f && cross2(c - b, p - b) >= 0.0f &&
                          cross2(a - c, p - c) >= 0.0f);
            }
            if (!empty) continue;
            out.push_back({ia, ib, ic});
            poly.erase(poly.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) break;   // degenerate section: keep what we have
    }
    if (poly.size() == 3) out.push_back({poly[0], poly[1], poly[2]});
    return out;
}

Profile rectProfile(float halfWidth, float bottom, float top) {
    return {{-halfWidth, bottom}, {halfWidth, bottom}, {halfWidth, top}, {-halfWidth, top}};
}

// A trapezoid: `halfBottom` at `bottom` narrowing to `halfTop` at `top`. Walls
// and ballast beds are both this shape.
Profile taperProfile(float halfBottom, float halfTop, float bottom, float top) {
    return {{-halfBottom, bottom}, {halfBottom, bottom}, {halfTop, top}, {-halfTop, top}};
}

// The same with a base course: the section holds `halfBase + over` at the very
// bottom, comes in to `halfBase` at `toe` metres up, and tapers to `halfTop` at
// the top. With no splay (over = 0) that is a vertical foot under a battered
// face, which is exactly a Jersey barrier; with splay it is a plinth under a
// wall. Falls back to the plain trapezoid when there is no base course to draw.
Profile toeProfile(float halfBase, float halfTop, float over, float bottom,
                   float toe, float top) {
    if (toe <= 1e-4f || toe >= top - bottom)
        return taperProfile(halfBase + over, halfTop, bottom, top);
    const float y = bottom + toe;
    return {{-(halfBase + over), bottom}, {halfBase + over, bottom},
            {halfBase, y},                {halfTop, top},
            {-halfTop, top},              {-halfBase, y}};
}

// Vignoles rail in section: wide foot, thin web, flat head. Twelve points is
// more than a rail needs at any sane viewing distance, but it is what makes the
// track read as track rather than as two grey stripes -- the head catches the
// sun and the web stays in shadow.
Profile railProfile(float width, float height) {
    const float foot = width * 1.7f, web = width * 0.34f;
    const float footH = height * 0.16f, shoulder = height * 0.30f;
    const float headB = height * 0.72f;
    return {{-foot * 0.5f, 0.0f},     {foot * 0.5f, 0.0f},
            {foot * 0.5f, footH},     {web * 0.5f, shoulder},
            {web * 0.5f, headB},      {width * 0.5f, headB},
            {width * 0.5f, height},   {-width * 0.5f, height},
            {-width * 0.5f, headB},   {-web * 0.5f, headB},
            {-web * 0.5f, shoulder},  {-foot * 0.5f, footH}};
}

// Sweep `prof` along frames [i0, i1] at a lateral offset, one quad strip per
// profile edge (so every edge keeps its own hard normal), plus end caps.
//
// `uvTile` is world metres per texture repeat: u runs along the path by arc
// length, v around the section by perimeter, so a wall's face tiles evenly
// however the samples happen to fall.
void sweep(Slot& sl, const std::vector<Frame>& f, std::size_t i0, std::size_t i1,
           const Profile& prof, float lateral, float uvTile, bool capStart, bool capEnd) {
    if (i1 <= i0 || prof.size() < 3) return;
    const std::size_t rows = i1 - i0 + 1;
    const std::size_t edges = prof.size();
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const float tile = std::max(uvTile, 0.05f);

    auto world = [&](std::size_t i, const glm::vec2& q) {
        return f[i].p + f[i].r * (q.x + lateral) + up * q.y;
    };

    // Perimeter distance to each profile point, for v.
    std::vector<float> perim(edges + 1, 0.0f);
    for (std::size_t e = 0; e < edges; ++e)
        perim[e + 1] = perim[e] + glm::length(prof[(e + 1) % edges] - prof[e]);

    sl.data.vertices.reserve(sl.data.vertices.size() + rows * edges * 2);
    sl.data.indices.reserve(sl.data.indices.size() + (rows - 1) * edges * 6);

    for (std::size_t e = 0; e < edges; ++e) {
        const glm::vec2 a = prof[e], b = prof[(e + 1) % edges];
        const glm::vec2 ed = b - a;
        if (glm::dot(ed, ed) < 1e-10f) continue;
        const glm::vec2 n2 = glm::normalize(glm::vec2(ed.y, -ed.x)); // outward, CCW
        const auto base = static_cast<std::uint32_t>(sl.data.vertices.size());
        for (std::size_t i = i0; i <= i1; ++i) {
            const glm::vec3 nrm = glm::normalize(f[i].r * n2.x + up * n2.y);
            const float     u   = f[i].station / tile;
            for (int k = 0; k < 2; ++k) {
                fitzel::Vertex v;
                v.position = world(i, k == 0 ? a : b);
                v.normal   = nrm;
                v.uv       = glm::vec2(u, perim[e + k] / tile);
                sl.lo = glm::min(sl.lo, v.position);
                sl.hi = glm::max(sl.hi, v.position);
                sl.data.vertices.push_back(v);
            }
        }
        for (std::size_t k = 0; k + 1 < rows; ++k) {
            const auto q = base + static_cast<std::uint32_t>(k * 2);
            for (std::uint32_t idx : {0u, 1u, 3u, 0u, 3u, 2u}) sl.data.indices.push_back(q + idx);
        }
    }

    // End caps. Profile order faces +tangent (cross(r, up) == t), so the start cap
    // takes each triangle backwards. Without these an open wall is hollow when you
    // stand at its end, which is exactly where the author stands after laying the
    // last point.
    const std::vector<glm::ivec3> fan = (capStart || capEnd) ? triangulate(prof)
                                                             : std::vector<glm::ivec3>{};
    auto cap = [&](std::size_t i, bool forward) {
        const glm::vec3 nrm = forward ? f[i].t : -f[i].t;
        const auto base = static_cast<std::uint32_t>(sl.data.vertices.size());
        for (std::size_t e = 0; e < edges; ++e) {
            fitzel::Vertex v;
            v.position = world(i, prof[e]);
            v.normal   = nrm;
            v.uv       = (prof[e] + glm::vec2(0.5f)) / tile;
            sl.lo = glm::min(sl.lo, v.position);
            sl.hi = glm::max(sl.hi, v.position);
            sl.data.vertices.push_back(v);
        }
        for (const glm::ivec3& t : fan) {
            sl.data.indices.push_back(base + static_cast<std::uint32_t>(t.x));
            sl.data.indices.push_back(base + static_cast<std::uint32_t>(
                                                 forward ? t.y : t.z));
            sl.data.indices.push_back(base + static_cast<std::uint32_t>(
                                                 forward ? t.z : t.y));
        }
    };
    if (capStart) cap(i0, false);
    if (capEnd)   cap(i1, true);
}

// --- Walking the path at a spacing -------------------------------------------

// Where a repeated piece stands. Interpolated between frames, so the spacing is
// a true metre spacing rather than "every n-th sample".
struct Stop {
    glm::vec3 p;
    float     yaw;
    int       index;  // which piece this is along the WHOLE path (jitter seed)
};

// Stations in [fromStation, toStation) at `spacing`, phased from the path's
// start so a chunk boundary doesn't shift the pattern.
std::vector<Stop> stopsIn(const std::vector<Frame>& f, std::size_t i0, std::size_t i1,
                          float spacing, int& budget) {
    std::vector<Stop> out;
    const float step = std::max(spacing, 0.05f);
    const float from = f[i0].station, to = f[i1].station;
    int   idx  = static_cast<int>(std::ceil(from / step - 1e-4f));
    float st   = idx * step;
    std::size_t seg = i0;
    // A closed run's last sample repeats the first; stopping strictly before `to`
    // there keeps a post from being placed twice on the seam.
    while (st < to - 1e-4f) {
        if (budget <= 0) break;
        while (seg + 1 < i1 && f[seg + 1].station <= st) ++seg;
        const float span = std::max(f[seg + 1].station - f[seg].station, 1e-5f);
        const float u    = glm::clamp((st - f[seg].station) / span, 0.0f, 1.0f);
        Stop s;
        s.p = glm::mix(f[seg].p, f[seg + 1].p, u);
        // Shortest-arc lerp of the heading: the naive one swings a post right
        // round the compass wherever the path crosses the atan2 branch cut.
        float d = f[seg + 1].yaw - f[seg].yaw;
        while (d >  glm::pi<float>()) d -= glm::two_pi<float>();
        while (d < -glm::pi<float>()) d += glm::two_pi<float>();
        s.yaw   = f[seg].yaw + d * u;
        s.index = idx;
        out.push_back(s);
        --budget;
        ++idx;
        st = idx * step;
    }
    return out;
}

// Find-or-create one material by name, re-applying its look either way -- so
// re-colouring a slot re-skins every path already using it, which is the point
// of sharing them. (Same helper BuildingGen has, kept local for the same reason
// the cube is.)
AssetId ensureMaterial(std::vector<MaterialDef>& mats, const std::string& name,
                       glm::vec3 albedo, float refl, float rough) {
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo       = albedo;
        m.reflectivity = refl;
        m.roughness    = rough;
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

// Clamp a style into a range that can actually be built. Applied inside
// generate() rather than at the panel, so a scene hand-edited to a 0 m post
// spacing loads instead of hanging.
Style sane(const Style& in) {
    Style s = in;
    s.palette        = glm::clamp(s.palette, 0, 3);
    s.sink           = glm::clamp(s.sink, 0.0f, 5.0f);
    s.postSpacing    = std::max(s.postSpacing, 0.15f);
    s.postWidth      = glm::clamp(s.postWidth, 0.01f, 5.0f);
    s.postHeight     = glm::clamp(s.postHeight, 0.05f, 40.0f);
    s.rails          = glm::clamp(s.rails, 0, 12);
    s.railThick      = glm::clamp(s.railThick, 0.005f, 2.0f);
    s.infill         = glm::clamp(s.infill, 0.0f, 2.0f);
    s.postCap        = glm::clamp(s.postCap, 0.0f, 3.0f);
    s.postCapOver    = glm::clamp(s.postCapOver, 0.0f, 2.0f);
    s.picketEvery    = std::max(s.picketEvery, 0.0f);
    s.picketWidth    = glm::clamp(s.picketWidth, 0.005f, 3.0f);
    s.picketDepth    = glm::clamp(s.picketDepth, 0.005f, 3.0f);
    s.wallHeight     = glm::clamp(s.wallHeight, 0.05f, 60.0f);
    s.wallThick      = glm::clamp(s.wallThick, 0.02f, 20.0f);
    s.wallTaper      = glm::clamp(s.wallTaper, 0.05f, 3.0f);
    s.copingHeight   = glm::clamp(s.copingHeight, 0.0f, 5.0f);
    s.copingOver     = glm::clamp(s.copingOver, 0.0f, 3.0f);
    s.pillarEvery    = std::max(s.pillarEvery, 0.0f);
    s.pillarWidth    = glm::clamp(s.pillarWidth, 0.05f, 10.0f);
    s.toeHeight      = glm::clamp(s.toeHeight, 0.0f, 20.0f);
    s.toeOver        = glm::clamp(s.toeOver, 0.0f, 10.0f);
    s.merlonEvery    = std::max(s.merlonEvery, 0.0f);
    s.merlonWidth    = glm::clamp(s.merlonWidth, 0.05f, 20.0f);
    s.merlonRise     = glm::clamp(s.merlonRise, 0.0f, 10.0f);
    s.merlonInset    = glm::clamp(s.merlonInset, 0.0f, 5.0f);
    s.gauge          = glm::clamp(s.gauge, 0.2f, 20.0f);
    s.ballastWidth   = glm::clamp(s.ballastWidth, 0.0f, 40.0f);
    s.ballastHeight  = glm::clamp(s.ballastHeight, 0.0f, 10.0f);
    s.ballastSlope   = glm::clamp(s.ballastSlope, 0.0f, 6.0f);
    s.sleeperSpacing = std::max(s.sleeperSpacing, 0.1f);
    s.sleeperLength  = glm::clamp(s.sleeperLength, 0.1f, 20.0f);
    s.sleeperWidth   = glm::clamp(s.sleeperWidth, 0.02f, 5.0f);
    s.sleeperHeight  = glm::clamp(s.sleeperHeight, 0.01f, 3.0f);
    s.railHeight     = glm::clamp(s.railHeight, 0.01f, 3.0f);
    s.railWidth      = glm::clamp(s.railWidth, 0.005f, 2.0f);
    s.texTile        = std::max(s.texTile, 0.05f);
    return s;
}

// Metres of path one merged chunk covers. Long enough that a garden fence is one
// draw, short enough that a two-kilometre wall still has something for the
// frustum cull to bite on.
constexpr float kChunkLength = 140.0f;

} // namespace

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Fence: return "Fence";
        case Kind::Wall:  return "Wall";
        case Kind::Rail:  return "Track";
        case Kind::Count: break;
    }
    return "Spline";
}

bool Style::operator==(const Style& o) const {
    return sink == o.sink && lift == o.lift && collide == o.collide &&
           palette == o.palette && seed == o.seed &&
           postSpacing == o.postSpacing && postWidth == o.postWidth &&
           postHeight == o.postHeight && rails == o.rails &&
           railThick == o.railThick && railTop == o.railTop &&
           railBottom == o.railBottom && infill == o.infill &&
           infillTop == o.infillTop && infillBottom == o.infillBottom &&
           postJitter == o.postJitter && postCap == o.postCap &&
           postCapOver == o.postCapOver && picketEvery == o.picketEvery &&
           picketWidth == o.picketWidth && picketDepth == o.picketDepth &&
           picketTop == o.picketTop && picketBottom == o.picketBottom &&
           toeHeight == o.toeHeight && toeOver == o.toeOver &&
           merlonEvery == o.merlonEvery && merlonWidth == o.merlonWidth &&
           merlonRise == o.merlonRise && merlonInset == o.merlonInset &&
           matA == o.matA && matB == o.matB && matC == o.matC &&
           wallHeight == o.wallHeight &&
           wallThick == o.wallThick && wallTaper == o.wallTaper &&
           copingHeight == o.copingHeight && copingOver == o.copingOver &&
           pillarEvery == o.pillarEvery && pillarWidth == o.pillarWidth &&
           pillarRise == o.pillarRise && gauge == o.gauge &&
           ballastWidth == o.ballastWidth && ballastHeight == o.ballastHeight &&
           ballastSlope == o.ballastSlope && sleeperSpacing == o.sleeperSpacing &&
           sleeperLength == o.sleeperLength && sleeperWidth == o.sleeperWidth &&
           sleeperHeight == o.sleeperHeight && railHeight == o.railHeight &&
           railWidth == o.railWidth && colorA == o.colorA && colorB == o.colorB &&
           colorC == o.colorC && texTile == o.texTile;
}

const char* presetName(Preset p) {
    switch (p) {
        case Preset::PostRail:      return "Post & rail";
        case Preset::Picket:        return "Picket";
        case Preset::ChainLink:     return "Chain link";
        case Preset::Palisade:      return "Palisade";
        case Preset::Wire:          return "Wire";
        case Preset::IronRailing:   return "Iron railing";
        case Preset::Ranch:         return "Ranch rail";
        case Preset::Balustrade:    return "Balustrade";
        case Preset::Hoarding:      return "Site hoarding";
        case Preset::Security:      return "Security fence";
        case Preset::GardenWall:    return "Garden wall";
        case Preset::PierWall:      return "Wall with piers";
        case Preset::DryStone:      return "Dry stone wall";
        case Preset::Retaining:     return "Retaining wall";
        case Preset::Battlement:    return "Battlement";
        case Preset::Jersey:        return "Jersey barrier";
        case Preset::ConcretePanel: return "Concrete panel";
        case Preset::Parapet:       return "Parapet";
        case Preset::SeaWall:       return "Sea wall";
        case Preset::LowBoundary:   return "Low boundary";
        case Preset::StandardGauge: return "Standard gauge";
        case Preset::NarrowGauge:   return "Narrow gauge";
        case Preset::Tram:          return "Tram track";
        case Preset::Siding:        return "Yard siding";
        case Preset::Count:         break;
    }
    return "Preset";
}

Kind presetKind(Preset p) {
    if (p >= Preset::StandardGauge) return Kind::Rail;
    if (p >= Preset::GardenWall)    return Kind::Wall;
    return Kind::Fence;
}

Style preset(Preset p) {
    Style s;
    // Shared starting points per kind; each case below only says what makes it
    // that structure rather than the generic one.
    switch (presetKind(p)) {
        case Kind::Fence:
            s.colorA = {0.40f, 0.31f, 0.22f};
            s.colorB = {0.46f, 0.36f, 0.26f};
            s.colorC = {0.55f, 0.56f, 0.58f};
            s.texTile = 1.5f;
            break;
        case Kind::Wall:
            s.colorA = {0.56f, 0.54f, 0.50f};
            s.colorB = {0.62f, 0.60f, 0.56f};
            s.colorC = {0.50f, 0.48f, 0.45f};
            s.sink   = 0.30f;
            s.texTile = 2.5f;
            break;
        case Kind::Rail:
        case Kind::Count:
            s.colorA = {0.42f, 0.39f, 0.37f};
            s.colorB = {0.24f, 0.20f, 0.17f};
            s.colorC = {0.44f, 0.42f, 0.39f};
            s.sink   = 0.15f;
            s.texTile = 1.0f;
            break;
    }

    switch (p) {
        // --- Fences ----------------------------------------------------------
        case Preset::PostRail:   // a timber field fence: posts, three rails, air
            break;               // (the shared fence defaults already are one)
        case Preset::Picket:
            s.postSpacing = 2.4f; s.postWidth = 0.09f; s.postHeight = 1.05f;
            s.rails = 2; s.railThick = 0.045f; s.railTop = 0.80f; s.railBottom = 0.25f;
            s.picketEvery = 0.14f; s.picketWidth = 0.085f; s.picketDepth = 0.022f;
            s.picketTop = 1.0f; s.picketBottom = 0.12f;
            s.postCap = 0.05f; s.postCapOver = 0.02f;
            s.colorA = s.colorB = {0.86f, 0.85f, 0.82f};   // painted white
            s.colorC = {0.84f, 0.83f, 0.80f};
            s.texTile = 0.8f;
            break;
        case Preset::ChainLink:
            s.postSpacing = 3.0f; s.postWidth = 0.06f; s.postHeight = 2.0f;
            s.rails = 1; s.railThick = 0.045f; s.railTop = 0.98f;
            s.infill = 0.012f; s.infillTop = 0.97f; s.infillBottom = 0.04f;
            s.colorA = s.colorB = {0.58f, 0.60f, 0.62f};   // galvanised
            s.colorC = {0.62f, 0.64f, 0.66f};
            break;
        case Preset::Palisade:
            s.postSpacing = 2.6f; s.postWidth = 0.11f; s.postHeight = 1.95f;
            s.rails = 2; s.railThick = 0.06f; s.railTop = 0.82f; s.railBottom = 0.28f;
            s.picketEvery = 0.17f; s.picketWidth = 0.13f; s.picketDepth = 0.035f;
            s.picketTop = 1.0f; s.picketBottom = 0.04f;
            s.postJitter = 0.05f;
            s.colorA = {0.31f, 0.24f, 0.17f};
            s.colorB = {0.34f, 0.27f, 0.19f};
            s.colorC = {0.37f, 0.29f, 0.20f};
            break;
        case Preset::Wire:
            s.postSpacing = 4.0f; s.postWidth = 0.075f; s.postHeight = 1.25f;
            s.rails = 4; s.railThick = 0.014f; s.railTop = 0.96f; s.railBottom = 0.22f;
            s.postJitter = 0.08f;
            s.colorA = {0.33f, 0.27f, 0.20f};              // weathered stakes
            s.colorB = {0.46f, 0.47f, 0.48f};              // wire
            break;
        case Preset::IronRailing:
            s.postSpacing = 3.0f; s.postWidth = 0.075f; s.postHeight = 1.35f;
            s.rails = 2; s.railThick = 0.045f; s.railTop = 0.97f; s.railBottom = 0.11f;
            s.picketEvery = 0.13f; s.picketWidth = 0.024f; s.picketDepth = 0.024f;
            s.picketTop = 0.94f; s.picketBottom = 0.10f;
            s.postCap = 0.08f; s.postCapOver = 0.022f;
            s.colorA = s.colorB = s.colorC = {0.075f, 0.078f, 0.085f}; // near black
            s.texTile = 0.6f;
            break;
        case Preset::Ranch:
            s.postSpacing = 3.2f; s.postWidth = 0.17f; s.postHeight = 1.35f;
            s.rails = 2; s.railThick = 0.10f; s.railTop = 0.95f; s.railBottom = 0.45f;
            s.colorA = s.colorB = {0.88f, 0.87f, 0.84f};
            s.texTile = 1.2f;
            break;
        case Preset::Balustrade:
            s.postSpacing = 2.6f; s.postWidth = 0.20f; s.postHeight = 1.05f;
            s.rails = 2; s.railThick = 0.11f; s.railTop = 0.95f; s.railBottom = 0.10f;
            s.picketEvery = 0.19f; s.picketWidth = 0.085f; s.picketDepth = 0.085f;
            s.picketTop = 0.90f; s.picketBottom = 0.14f;
            s.postCap = 0.09f; s.postCapOver = 0.035f;
            s.sink = 0.05f;
            s.colorA = s.colorB = s.colorC = {0.72f, 0.70f, 0.65f};   // stone
            s.texTile = 1.0f;
            break;
        case Preset::Hoarding:
            s.postSpacing = 2.4f; s.postWidth = 0.10f; s.postHeight = 2.4f;
            s.rails = 2; s.railThick = 0.07f; s.railTop = 0.92f; s.railBottom = 0.25f;
            s.infill = 0.022f; s.infillTop = 1.0f; s.infillBottom = 0.02f;
            s.colorA = s.colorB = {0.36f, 0.30f, 0.24f};
            s.colorC = {0.24f, 0.36f, 0.52f};              // painted ply
            s.texTile = 2.0f;
            break;
        case Preset::Security:
            s.postSpacing = 3.0f; s.postWidth = 0.085f; s.postHeight = 3.0f;
            s.rails = 2; s.railThick = 0.05f; s.railTop = 0.99f; s.railBottom = 0.03f;
            s.infill = 0.01f; s.infillTop = 0.98f; s.infillBottom = 0.02f;
            s.colorA = s.colorB = {0.34f, 0.36f, 0.36f};
            s.colorC = {0.52f, 0.54f, 0.55f};
            break;

        // --- Walls -----------------------------------------------------------
        case Preset::GardenWall:  // the shared wall defaults are already one
            break;
        case Preset::PierWall:
            s.wallHeight = 2.0f; s.wallThick = 0.35f; s.wallTaper = 1.0f;
            s.copingHeight = 0.09f; s.copingOver = 0.06f;
            s.pillarEvery = 4.0f; s.pillarWidth = 0.55f; s.pillarRise = 0.25f;
            s.colorA = {0.47f, 0.29f, 0.23f};              // brick
            s.colorB = {0.60f, 0.58f, 0.54f};
            s.colorC = {0.44f, 0.27f, 0.21f};
            s.texTile = 1.6f;
            break;
        case Preset::DryStone:
            s.wallHeight = 1.15f; s.wallThick = 0.75f; s.wallTaper = 0.58f;
            s.copingHeight = 0.10f; s.copingOver = 0.0f;
            s.sink = 0.35f;
            s.colorA = {0.52f, 0.51f, 0.47f};
            s.colorB = {0.56f, 0.55f, 0.51f};
            s.texTile = 1.2f;
            break;
        case Preset::Retaining:
            s.wallHeight = 3.0f; s.wallThick = 1.0f; s.wallTaper = 0.55f;
            s.copingHeight = 0.12f; s.copingOver = 0.0f;
            s.sink = 0.5f;
            s.colorA = s.colorB = {0.55f, 0.55f, 0.54f};   // shuttered concrete
            s.texTile = 3.0f;
            break;
        case Preset::Battlement:
            s.wallHeight = 3.6f; s.wallThick = 0.95f; s.wallTaper = 0.92f;
            s.copingHeight = 0.10f; s.copingOver = 0.04f;
            s.merlonEvery = 2.0f; s.merlonWidth = 1.10f; s.merlonRise = 0.85f;
            s.merlonInset = 0.10f;
            s.sink = 0.4f;
            s.colorA = {0.58f, 0.56f, 0.50f};
            s.colorB = {0.62f, 0.60f, 0.54f};
            s.colorC = {0.58f, 0.56f, 0.50f};
            s.texTile = 1.8f;
            break;
        case Preset::Jersey:
            // The motorway barrier: a vertical foot, then the hard batter that
            // rides a wheel back down instead of catching it.
            s.wallHeight = 0.81f; s.wallThick = 0.60f; s.wallTaper = 0.31f;
            s.toeHeight = 0.09f; s.toeOver = 0.0f;
            s.copingHeight = 0.0f; s.sink = 0.02f;
            s.colorA = s.colorB = {0.70f, 0.69f, 0.66f};
            s.texTile = 2.0f;
            break;
        case Preset::ConcretePanel:
            s.wallHeight = 2.6f; s.wallThick = 0.20f; s.wallTaper = 1.0f;
            s.copingHeight = 0.06f; s.copingOver = 0.02f;
            s.pillarEvery = 3.0f; s.pillarWidth = 0.30f; s.pillarRise = 0.08f;
            s.sink = 0.25f;
            s.colorA = {0.60f, 0.60f, 0.58f};
            s.colorB = {0.56f, 0.56f, 0.54f};
            s.colorC = {0.52f, 0.52f, 0.50f};
            s.texTile = 2.6f;
            break;
        case Preset::Parapet:
            s.wallHeight = 1.05f; s.wallThick = 0.30f; s.wallTaper = 1.0f;
            s.copingHeight = 0.09f; s.copingOver = 0.06f;
            s.sink = 0.1f;
            s.colorA = {0.68f, 0.66f, 0.62f};
            s.colorB = {0.72f, 0.70f, 0.66f};
            break;
        case Preset::SeaWall:
            s.wallHeight = 4.0f; s.wallThick = 2.2f; s.wallTaper = 0.42f;
            s.copingHeight = 0.20f; s.copingOver = 0.10f;
            s.toeHeight = 0.5f; s.toeOver = 0.25f;
            s.sink = 0.6f;
            s.colorA = s.colorB = {0.50f, 0.50f, 0.49f};
            s.texTile = 3.5f;
            break;
        case Preset::LowBoundary:
            s.wallHeight = 0.60f; s.wallThick = 0.30f; s.wallTaper = 0.95f;
            s.copingHeight = 0.07f; s.copingOver = 0.04f;
            s.sink = 0.2f;
            break;

        // --- Track -----------------------------------------------------------
        case Preset::StandardGauge:  // the shared rail defaults are already one
            break;
        case Preset::NarrowGauge:
            s.gauge = 0.76f; s.sleeperLength = 1.55f; s.sleeperSpacing = 0.55f;
            s.sleeperWidth = 0.20f; s.sleeperHeight = 0.13f;
            s.ballastWidth = 2.20f; s.ballastHeight = 0.32f;
            s.railHeight = 0.11f; s.railWidth = 0.05f;
            break;
        case Preset::Tram:
            // Rails set into a street: no bed, sleepers reduced to the ties that
            // hold the gauge, everything nearly flush with the ground.
            s.ballastWidth = 0.0f; s.ballastHeight = 0.0f;
            s.sleeperSpacing = 1.20f; s.sleeperLength = 1.90f;
            s.sleeperWidth = 0.16f; s.sleeperHeight = 0.06f;
            s.railHeight = 0.14f; s.railWidth = 0.075f;
            s.sink = 0.05f;
            s.colorB = {0.40f, 0.40f, 0.40f};
            break;
        case Preset::Siding:
            s.sleeperSpacing = 0.75f; s.ballastWidth = 3.1f; s.ballastHeight = 0.28f;
            s.ballastSlope = 1.6f;
            s.colorA = {0.38f, 0.31f, 0.26f};              // rusted, little traffic
            s.colorB = {0.21f, 0.18f, 0.15f};
            s.colorC = {0.40f, 0.38f, 0.34f};
            break;
        case Preset::Count: break;
    }
    return s;
}

Style preset(Kind k) {
    switch (k) {
        case Kind::Wall: return preset(Preset::GardenWall);
        case Kind::Rail: return preset(Preset::StandardGauge);
        case Kind::Fence:
        case Kind::Count: break;
    }
    return preset(Preset::PostRail);
}

Palette ensurePalette(std::vector<MaterialDef>& materials, Kind k, const Style& sIn) {
    const Style s = sane(sIn);
    const std::string slot = std::string(kindName(k)) + " " +
                             static_cast<char>('A' + s.palette) + " ";
    Palette pal;
    switch (k) {
        case Kind::Wall:
            pal.primary   = ensureMaterial(materials, slot + "Face",   s.colorA, 0.0f, 0.90f);
            pal.secondary = ensureMaterial(materials, slot + "Coping", s.colorB, 0.02f, 0.80f);
            pal.tertiary  = ensureMaterial(materials, slot + "Pier",   s.colorC, 0.0f, 0.90f);
            break;
        case Kind::Rail:
            // Only the rail head is polished by traffic, and it is the one thing
            // on a track that must catch the light -- hence the reflectivity here
            // and nowhere else in this module.
            pal.primary   = ensureMaterial(materials, slot + "Steel",   s.colorA, 0.28f, 0.28f);
            pal.secondary = ensureMaterial(materials, slot + "Sleeper", s.colorB, 0.0f, 0.92f);
            pal.tertiary  = ensureMaterial(materials, slot + "Ballast", s.colorC, 0.0f, 0.98f);
            break;
        case Kind::Fence:
        case Kind::Count:
            pal.primary   = ensureMaterial(materials, slot + "Post",  s.colorA, 0.0f, 0.88f);
            pal.secondary = ensureMaterial(materials, slot + "Rail",  s.colorB, 0.0f, 0.88f);
            pal.tertiary  = ensureMaterial(materials, slot + "Panel", s.colorC, 0.08f, 0.55f);
            break;
    }
    return pal;
}

Result generate(Kind k, const Style& sIn, const std::vector<glm::vec3>& pathIn,
                bool closed, const Palette& pal, int maxPieces) {
    Result res;
    const Style s = sane(sIn);
    if (pathIn.size() < 2) return res;

    // A closed run is an open one whose last sample repeats the first. Doing that
    // here means nothing downstream needs wrap-around logic -- the sweep, the
    // chunking and the post walk all just see a longer path.
    std::vector<glm::vec3> path = pathIn;
    if (closed && glm::distance(path.front(), path.back()) > 1e-4f) path.push_back(path.front());

    const std::vector<Frame> f = makeFrames(path, closed);
    if (f.size() < 2) return res;
    res.length = f.back().station;

    int budget = std::max(maxPieces, 1);

    // Chunk boundaries: sample indices at which one merged batch ends and the next
    // begins. Neighbours SHARE the boundary sample, so the sweep is continuous
    // across it -- a seam in the mesh, not a gap in the wall.
    std::vector<std::size_t> cut{0};
    for (std::size_t i = 1; i < f.size(); ++i)
        if (f[i].station - f[cut.back()].station >= kChunkLength && i + 1 < f.size())
            cut.push_back(i);
    cut.push_back(f.size() - 1);

    for (std::size_t c = 0; c + 1 < cut.size(); ++c) {
        const std::size_t i0 = cut[c], i1 = cut[c + 1];
        if (i1 <= i0) continue;
        const bool capStart = (c == 0) && !closed;
        const bool capEnd   = (c + 2 == cut.size()) && !closed;
        Slot slot[3];   // primary, secondary, tertiary

        switch (k) {
            case Kind::Fence: {
                // Posts first: they are what a fence IS, and the rails only span
                // between them.
                const std::vector<Stop> posts = stopsIn(f, i0, i1, s.postSpacing, budget);
                for (const Stop& st : posts) {
                    const float j = s.postJitter <= 0.0f ? 1.0f
                        : 1.0f + (unitHash(static_cast<std::uint32_t>(st.index) * 2654435761u +
                                           s.seed) - 0.5f) * 2.0f * glm::clamp(s.postJitter, 0.0f, 0.9f);
                    const float h = s.postHeight * j + s.sink;
                    appendBox(slot[0], st.p + glm::vec3(0.0f, h * 0.5f - s.sink, 0.0f),
                              glm::vec3(s.postWidth * 0.5f, h * 0.5f, s.postWidth * 0.5f),
                              st.yaw, s.texTile);
                    if (s.postCap > 0.0f) {
                        const float capHalf = s.postWidth * 0.5f + s.postCapOver;
                        appendBox(slot[0],
                                  st.p + glm::vec3(0.0f, s.postHeight * j + s.postCap * 0.5f, 0.0f),
                                  glm::vec3(capHalf, s.postCap * 0.5f, capHalf),
                                  st.yaw, s.texTile);
                    }
                }
                res.pieces += static_cast<int>(posts.size());

                // Pickets: the vertical bars between the posts. Their own walk, at
                // their own spacing, so a picket fence is a post fence with the
                // gaps filled rather than a second kind of thing. They take the
                // PANEL material -- what fills the bay is what fills the bay,
                // whether it is boarding or a hundred balusters.
                if (s.picketEvery > 0.0f) {
                    const std::vector<Stop> bars =
                        stopsIn(f, i0, i1, s.picketEvery, budget);
                    const float bot = s.postHeight * s.picketBottom;
                    const float top = s.postHeight *
                                      std::max(s.picketTop, s.picketBottom + 0.02f);
                    for (const Stop& st : bars)
                        appendBox(slot[2],
                                  st.p + glm::vec3(0.0f, (bot + top) * 0.5f, 0.0f),
                                  glm::vec3(s.picketWidth * 0.5f, (top - bot) * 0.5f,
                                            s.picketDepth * 0.5f),
                                  st.yaw, s.texTile);
                    res.pieces += static_cast<int>(bars.size());
                }

                // The bars, swept whole rather than cut per bay: a rail crossing a
                // post is invisible and one continuous sweep costs a fraction of
                // the segments (and none of the mitring) that cutting would.
                for (int r = 0; r < s.rails; ++r) {
                    const float t = s.rails == 1 ? 1.0f
                                                 : static_cast<float>(r) / static_cast<float>(s.rails - 1);
                    const float hf = glm::mix(s.railBottom, s.railTop, t);
                    const float y  = s.postHeight * hf;
                    sweep(slot[1], f, i0, i1,
                          rectProfile(s.railThick * 0.5f, y - s.railThick * 0.5f,
                                      y + s.railThick * 0.5f),
                          0.0f, s.texTile, capStart, capEnd);
                }

                if (s.infill > 0.0f) {
                    const float bot = s.postHeight * s.infillBottom;
                    const float top = s.postHeight * std::max(s.infillTop, s.infillBottom + 0.01f);
                    sweep(slot[2], f, i0, i1, rectProfile(s.infill * 0.5f, bot, top),
                          0.0f, s.texTile, capStart, capEnd);
                }
                break;
            }
            case Kind::Wall: {
                const float halfB = s.wallThick * 0.5f;
                const float halfT = halfB * s.wallTaper;
                sweep(slot[0], f, i0, i1,
                      toeProfile(halfB, halfT, s.toeOver, -s.sink,
                                 s.toeHeight + s.sink, s.wallHeight),
                      0.0f, s.texTile, capStart, capEnd);
                if (s.copingHeight > 0.0f)
                    sweep(slot[1], f, i0, i1,
                          rectProfile(halfT + s.copingOver, s.wallHeight,
                                      s.wallHeight + s.copingHeight),
                          0.0f, s.texTile, capStart, capEnd);
                if (s.pillarEvery > 0.0f) {
                    const std::vector<Stop> piers = stopsIn(f, i0, i1, s.pillarEvery, budget);
                    for (const Stop& st : piers) {
                        const float h = s.wallHeight + s.copingHeight + s.pillarRise + s.sink;
                        appendBox(slot[2],
                                  st.p + glm::vec3(0.0f, h * 0.5f - s.sink, 0.0f),
                                  glm::vec3(s.pillarWidth * 0.5f, h * 0.5f, s.pillarWidth * 0.5f),
                                  st.yaw, s.texTile);
                    }
                    res.pieces += static_cast<int>(piers.size());
                }
                // Merlons: blocks on the wall top with the embrasures left between
                // them. Boxes rather than a notched sweep, because a crenellation
                // is a repeated piece at a spacing -- the same walk the piers and
                // the posts use, and it lands on curves for free.
                if (s.merlonEvery > 0.0f && s.merlonRise > 0.0f) {
                    const std::vector<Stop> merlons =
                        stopsIn(f, i0, i1, s.merlonEvery, budget);
                    const float base = s.wallHeight + s.copingHeight;
                    const float halfM = std::max(halfT - s.merlonInset, 0.02f);
                    for (const Stop& st : merlons)
                        appendBox(slot[1],
                                  st.p + glm::vec3(0.0f, base + s.merlonRise * 0.5f, 0.0f),
                                  glm::vec3(halfM, s.merlonRise * 0.5f,
                                            std::min(s.merlonWidth, s.merlonEvery) * 0.5f),
                                  st.yaw, s.texTile);
                    res.pieces += static_cast<int>(merlons.size());
                }
                break;
            }
            case Kind::Rail:
            case Kind::Count: {
                float deck = 0.0f;   // top of the bed the sleepers lie on
                if (s.ballastWidth > 0.0f && s.ballastHeight > 0.0f) {
                    const float halfT = s.ballastWidth * 0.5f;
                    const float halfB = halfT + s.ballastHeight * s.ballastSlope;
                    sweep(slot[2], f, i0, i1, taperProfile(halfB, halfT, -s.sink, s.ballastHeight),
                          0.0f, s.texTile, capStart, capEnd);
                    deck = s.ballastHeight;
                }
                const std::vector<Stop> sleepers = stopsIn(f, i0, i1, s.sleeperSpacing, budget);
                for (const Stop& st : sleepers)
                    appendBox(slot[1],
                              st.p + glm::vec3(0.0f, deck + s.sleeperHeight * 0.5f, 0.0f),
                              glm::vec3(s.sleeperLength * 0.5f, s.sleeperHeight * 0.5f,
                                        s.sleeperWidth * 0.5f),
                              st.yaw, s.texTile);
                res.pieces += static_cast<int>(sleepers.size());

                const float railBase = deck + s.sleeperHeight;
                const Profile rp = railProfile(s.railWidth, s.railHeight);
                Profile lifted = rp;
                for (glm::vec2& q : lifted) q.y += railBase;
                sweep(slot[0], f, i0, i1, lifted, -s.gauge * 0.5f, s.texTile, capStart, capEnd);
                sweep(slot[0], f, i0, i1, lifted,  s.gauge * 0.5f, s.texTile, capStart, capEnd);
                break;
            }
        }

        // The path's own override wins over the shared palette slot, per element.
        const AssetId mat[3] = {s.matA.valid() ? s.matA : pal.primary,
                                s.matB.valid() ? s.matB : pal.secondary,
                                s.matC.valid() ? s.matC : pal.tertiary};
        for (int i = 0; i < 3; ++i) {
            if (slot[i].empty()) continue;
            Batch b;
            b.material = mat[i];
            b.lo       = slot[i].lo;
            b.hi       = slot[i].hi;
            b.data     = std::move(slot[i].data);
            res.verts += static_cast<int>(b.data.vertices.size());
            res.batches.push_back(std::move(b));
        }
    }
    res.budgetHit = budget <= 0;

    // --- Collision -----------------------------------------------------------
    // One box per short run rather than one per part. A car hitting a fence needs
    // the fence to be there, not to be able to thread the gap between two rails --
    // and a box per post would put thousands of bodies in the world for a hedge.
    if (s.collide) {
        float halfW = 0.5f, height = 1.0f;
        switch (k) {
            case Kind::Fence:
                halfW  = std::max({s.postWidth, s.railThick, s.infill,
                                   s.picketEvery > 0.0f ? s.picketDepth : 0.0f}) * 0.5f;
                height = s.postHeight + s.postCap;
                break;
            case Kind::Wall:
                halfW  = std::max(s.wallThick, s.pillarEvery > 0.0f ? s.pillarWidth : 0.0f) * 0.5f
                         + std::max(s.copingOver, s.toeOver);
                height = s.wallHeight + s.copingHeight +
                         (s.merlonEvery > 0.0f ? s.merlonRise : 0.0f);
                break;
            case Kind::Rail:
            case Kind::Count:
                // The bed, not the rails: a track is something you drive over.
                halfW  = s.ballastWidth * 0.5f + s.ballastHeight * s.ballastSlope;
                height = s.ballastHeight + s.sleeperHeight;
                if (s.ballastWidth <= 0.0f) { halfW = s.sleeperLength * 0.5f; height = s.sleeperHeight; }
                break;
        }
        // ~4 m of path per box: short enough that a curve stays inside its own
        // envelope, long enough that a kilometre of wall is 250 bodies.
        const float step = 4.0f;
        std::size_t i = 0;
        while (i + 1 < f.size()) {
            std::size_t j = i + 1;
            while (j + 1 < f.size() && f[j].station - f[i].station < step) ++j;
            const glm::vec3 a = f[i].p, b = f[j].p;
            const glm::vec3 mid = (a + b) * 0.5f;
            const glm::vec3 d(b.x - a.x, 0.0f, b.z - a.z);
            const float len = glm::length(d);
            if (len > 1e-4f) {
                Collider col;
                col.center = glm::vec3(mid.x, mid.y + height * 0.5f, mid.z);
                // Half the chord plus the drop across it, so a box still covers the
                // ground under a run that climbs.
                col.half   = glm::vec3(halfW, height * 0.5f + std::abs(b.y - a.y) * 0.5f,
                                       len * 0.5f);
                col.yaw    = glm::degrees(std::atan2(d.x, d.z));
                res.colliders.push_back(col);
            }
            i = j;
        }
    }
    return res;
}

} // namespace splinegen
