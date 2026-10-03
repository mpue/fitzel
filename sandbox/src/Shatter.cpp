#include "Shatter.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/render/Renderer.hpp>

#include "Component.hpp"   // ModelComponent, MeshComponent, MaterialComponent, DecalComponent
#include "Document.hpp"
#include "EditMesh.hpp"
#include "ModelLibrary.hpp"
#include "Modifiers.hpp"   // modifiers::shown
#include "SceneGraph.hpp"  // scenegraph::compose

namespace shatter {
namespace {

constexpr float kPi     = 3.14159265358979f;
constexpr float kG      = 9.81f;
constexpr float kFlat   = 0.0015f;  // corners this far off the struck plane still lie in it
constexpr float kDepth  = 0.03f;    // how far behind the face the rest of a pane may lie
constexpr float kEdge   = 0.002f;   // and how far outside its outline
constexpr float kTiny   = 2.0e-5f;  // m^2: a crumb smaller than this is not a shard

// A small repeatable random source (xorshift).
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 0x2545f491u) {}
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uni(float a, float b) { return a + (b - a) * static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint32_t>(n)); }
    glm::vec3 inBall() {
        for (;;) {
            const glm::vec3 p(uni(-1.0f, 1.0f), uni(-1.0f, 1.0f), uni(-1.0f, 1.0f));
            if (glm::dot(p, p) <= 1.0f) return p;
        }
    }
};

// The point of triangle abc nearest `p` (Ericson, Real-Time Collision Detection 5.1.5).
glm::vec3 nearestOnTri(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float den = 1.0f / (va + vb + vc);
    return a + ab * (vb * den) + ac * (vc * den);
}

float distToTri2(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    const glm::vec3 q = nearestOnTri(glm::vec3(p, 0.0f), glm::vec3(a, 0.0f), glm::vec3(b, 0.0f), glm::vec3(c, 0.0f));
    return glm::length(glm::vec2(q) - p);
}

float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

float signedArea(const std::vector<glm::vec2>& poly) {
    float a = 0.0f;
    for (std::size_t i = 0; i < poly.size(); ++i) a += cross2(poly[i], poly[(i + 1) % poly.size()]);
    return 0.5f * a;
}

// Keep the part of `poly` on the left of each edge of counter-clockwise triangle abc.
std::vector<glm::vec2> clipToTri(std::vector<glm::vec2> poly, const glm::vec2& a, const glm::vec2& b,
                                 const glm::vec2& c) {
    const glm::vec2 corner[3] = {a, b, c};
    std::vector<glm::vec2> next;
    for (int k = 0; k < 3 && poly.size() >= 3; ++k) {
        const glm::vec2 p0 = corner[k], e = corner[(k + 1) % 3] - p0;
        next.clear();
        const std::size_t n = poly.size();
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec2& s = poly[i];
            const glm::vec2& t = poly[(i + 1) % n];
            const float ds = cross2(e, s - p0), dt = cross2(e, t - p0);
            if (ds >= 0.0f) next.push_back(s);
            if ((ds >= 0.0f) != (dt >= 0.0f)) next.push_back(s + (t - s) * (ds / (ds - dt)));
        }
        poly.swap(next);
    }
    return poly.size() >= 3 ? poly : std::vector<glm::vec2>{};
}

// The turn that takes direction `from` onto `to` (both unit length).
glm::quat turnOnto(const glm::vec3& from, const glm::vec3& to) {
    const float c = glm::dot(from, to);
    if (c > 0.99999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (c < -0.99999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), from);
        if (glm::dot(axis, axis) < 1e-6f) axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), from);
        return glm::angleAxis(kPi, glm::normalize(axis));
    }
    const glm::vec3 axis = glm::cross(from, to);
    const float s = std::sqrt((1.0f + c) * 2.0f);
    return glm::normalize(glm::quat(s * 0.5f, axis.x / s, axis.y / s, axis.z / s));
}

// A corner welded to half a millimetre, as a key.
std::uint64_t weldKey(const glm::vec3& p) {
    const auto q = [](float x) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(x * 2000.0f)) & 0x1fffff); };
    return q(p.x) | (q(p.y) << 21) | (q(p.z) << 42);
}

const Entity* findEntity(const std::vector<Entity>& entities, int id) {
    for (const Entity& e : entities)
        if (e.id == id) return &e;
    return nullptr;
}

// An imported model's transform as SceneSubmit draws it.
glm::mat4 modelToWorld(const Entity& e, const LoadedModel& lm) {
    const glm::vec3 sz = glm::max(lm.size(), glm::vec3(1e-4f));
    return scenegraph::compose(e.center, e.rotation, (e.half * 2.0f) / sz) *
           glm::translate(glm::mat4(1.0f), -lm.center());
}

// A solid never made editable, as the shape it draws (like Decals' receivers).
EditMesh primitiveOf(const Entity& e) {
    switch (e.type) {
        case EntityType::Ramp:     return EditMesh::ramp(e.half);
        case EntityType::Cylinder: return EditMesh::cylinder(e.half);
        case EntityType::Sphere:   return EditMesh::sphere(e.half);
        case EntityType::Plane:    return EditMesh::plane(e.half);
        default:                   return EditMesh::box(e.half);
    }
}

void addFaces(const EditMesh& m, const glm::mat4& mm, std::vector<glm::vec3>& out) {
    std::vector<glm::vec3> w(m.verts.size());
    for (std::size_t i = 0; i < m.verts.size(); ++i) w[i] = glm::vec3(mm * glm::vec4(m.verts[i], 1.0f));
    for (const auto& f : m.faces)
        for (std::size_t k = 1; k + 1 < f.size(); ++k) {
            out.push_back(w[static_cast<std::size_t>(f[0])]);
            out.push_back(w[static_cast<std::size_t>(f[k])]);
            out.push_back(w[static_cast<std::size_t>(f[k + 1])]);
        }
}

} // namespace

bool breakable(const MaterialDef& md) {
    return md.glass || (md.opacity < 0.99f && md.alphaMode != AlphaMode::Cutout);
}

bool Pane::contains(const glm::vec3& p, float slack) const {
    if (std::abs(glm::dot(p - origin, n)) > thickness * 0.5f + slack) return false;
    const glm::vec2 q = flat(p);
    for (std::size_t i = 0; i + 2 < outline.size(); i += 3)
        if (distToTri2(q, outline[i], outline[i + 1], outline[i + 2]) <= slack) return true;
    return false;
}

float Pane::area() const {
    float a = 0.0f;
    for (std::size_t i = 0; i + 2 < outline.size(); i += 3)
        a += 0.5f * std::abs(cross2(outline[i + 1] - outline[i], outline[i + 2] - outline[i]));
    return a;
}

bool paneAt(const std::vector<glm::vec3>& tris, const std::vector<glm::vec2>& uvs,
            const std::vector<bool>& skip, const glm::vec3& hit, float reach, Pane& pane,
            std::vector<std::uint32_t>& taken) {
    taken.clear();
    const std::size_t nt = tris.size() / 3;
    const auto usable = [&](std::size_t t) { return t >= skip.size() || !skip[t]; };
    const auto corner = [&](std::size_t t, int k) -> const glm::vec3& { return tris[t * 3 + static_cast<std::size_t>(k)]; };
    const auto normalOf = [&](std::size_t t) {
        const glm::vec3 c = glm::cross(corner(t, 1) - corner(t, 0), corner(t, 2) - corner(t, 0));
        const float len = glm::length(c);
        return len > 1e-12f ? c / len : glm::vec3(0.0f);
    };

    // The triangle under the hit.
    float best = reach;
    std::size_t seed = nt;
    for (std::size_t t = 0; t < nt; ++t) {
        if (!usable(t) || normalOf(t) == glm::vec3(0.0f)) continue;
        const float d = glm::length(nearestOnTri(hit, corner(t, 0), corner(t, 1), corner(t, 2)) - hit);
        if (d < best) { best = d; seed = t; }
    }
    if (seed == nt) return false;
    const glm::vec3 n0 = normalOf(seed);
    const glm::vec3 a0 = corner(seed, 0);
    const float o0 = glm::dot(n0, a0);
    const auto depth = [&](const glm::vec3& p) { return glm::dot(n0, p) - o0; };

    // Its face: the triangles it reaches through shared corners, facing the same
    // way in the same plane.
    std::vector<char> face(nt, 0);
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> byCorner;
    for (std::size_t t = 0; t < nt; ++t) {
        if (!usable(t) || glm::dot(normalOf(t), n0) < 0.995f) continue;
        bool flat = true;
        for (int k = 0; k < 3; ++k) flat = flat && std::abs(depth(corner(t, k))) < kFlat;
        if (!flat) continue;
        face[t] = 1;   // a candidate, until reached
        for (int k = 0; k < 3; ++k) byCorner[weldKey(corner(t, k))].push_back(t);
    }
    std::vector<char> in(nt, 0);
    std::vector<std::size_t> open{seed}, faceTris;
    in[seed] = 1;
    while (!open.empty()) {
        const std::size_t t = open.back();
        open.pop_back();
        faceTris.push_back(t);
        for (int k = 0; k < 3; ++k) {
            auto it = byCorner.find(weldKey(corner(t, k)));
            if (it == byCorner.end()) continue;
            for (std::size_t o : it->second)
                if (face[o] && !in[o]) { in[o] = 1; open.push_back(o); }
        }
    }

    // The frame: across along the struck triangle's first edge.
    pane.n = n0;
    pane.u = glm::normalize(corner(seed, 1) - a0);
    pane.v = glm::cross(n0, pane.u);
    pane.origin = a0;
    pane.outline.clear();
    for (std::size_t t : faceTris)
        for (int k = 0; k < 3; ++k) pane.outline.push_back(pane.flat(corner(t, k)));

    // The rest of it: what lies on that outline just behind (or before) it.
    float lo = 0.0f, hi = 0.0f;
    for (std::size_t t = 0; t < nt; ++t) {
        if (in[t]) { taken.push_back(static_cast<std::uint32_t>(t)); continue; }
        if (!usable(t)) continue;
        bool on = true;
        for (int k = 0; k < 3 && on; ++k) {
            const glm::vec3& p = corner(t, k);
            if (std::abs(depth(p)) > kDepth) { on = false; break; }
            const glm::vec2 q = pane.flat(p);
            bool inside = false;
            for (std::size_t i = 0; i + 2 < pane.outline.size() && !inside; i += 3)
                inside = distToTri2(q, pane.outline[i], pane.outline[i + 1], pane.outline[i + 2]) <= kEdge;
            on = inside;
        }
        if (!on) continue;
        taken.push_back(static_cast<std::uint32_t>(t));
        for (int k = 0; k < 3; ++k) {
            lo = std::min(lo, depth(corner(t, k)));
            hi = std::max(hi, depth(corner(t, k)));
        }
    }
    // Its thickness from the back it has; a single sheet gets a pane's 4 mm.
    pane.thickness = hi - lo > 0.001f ? hi - lo : 0.004f;
    pane.origin = a0 + n0 * (0.5f * (lo + hi));

    // Texture coordinates: the struck triangle's own mapping, carried across.
    pane.uv0 = glm::vec2(0.0f);
    pane.uvU = glm::vec2(1.0f, 0.0f);
    pane.uvV = glm::vec2(0.0f, 1.0f);
    if (uvs.size() >= tris.size()) {
        const glm::vec2 pa = pane.flat(corner(seed, 0)), pb = pane.flat(corner(seed, 1)), pc = pane.flat(corner(seed, 2));
        const glm::vec2 ta = uvs[seed * 3], tb = uvs[seed * 3 + 1], tc = uvs[seed * 3 + 2];
        const glm::mat2 P(pb - pa, pc - pa);
        if (std::abs(glm::determinant(P)) > 1e-12f) {
            const glm::mat2 M = glm::mat2(tb - ta, tc - ta) * glm::inverse(P);
            pane.uvU = M[0];
            pane.uvV = M[1];
            pane.uv0 = ta - M * pa;
        }
    }
    return true;
}

std::vector<Piece> crack(const Pane& pane, const glm::vec2& at, std::uint32_t seed) {
    std::vector<Piece> out;
    float reachOut = 0.0f;
    for (const glm::vec2& p : pane.outline) reachOut = std::max(reachOut, glm::length(p - at));
    if (reachOut < 1e-4f || pane.outline.size() < 3) return out;
    Rng rng(seed);

    // Cracks running out from the hole, unevenly spaced...
    const int spokes = 7 + rng.below(5);
    const float a0 = rng.uni(0.0f, 2.0f * kPi);
    std::vector<float> angle(static_cast<std::size_t>(spokes));
    for (int s = 0; s < spokes; ++s)
        angle[static_cast<std::size_t>(s)] = a0 + (static_cast<float>(s) + rng.uni(-0.3f, 0.3f)) * 2.0f * kPi / static_cast<float>(spokes);
    // ...and rings round it, further apart the further out: crumbs at the hole,
    // large pieces at the frame. The last ring lies outside the whole pane.
    std::vector<float> radius;
    float r = std::min(0.045f, reachOut * 0.3f) * rng.uni(0.8f, 1.2f);
    while (r < reachOut && radius.size() < 5) {
        radius.push_back(r);
        r *= rng.uni(1.6f, 2.1f);
    }
    radius.push_back(reachOut * 1.6f + 0.01f);
    const std::size_t rings = radius.size();
    std::vector<std::vector<glm::vec2>> ring(rings, std::vector<glm::vec2>(static_cast<std::size_t>(spokes)));
    for (std::size_t k = 0; k < rings; ++k)
        for (int s = 0; s < spokes; ++s) {
            const bool last = k + 1 == rings;
            const float rr = radius[k] * (last ? 1.0f : rng.uni(0.85f, 1.15f));
            const float aa = angle[static_cast<std::size_t>(s)] + (last ? 0.0f : rng.uni(-0.1f, 0.1f));
            ring[k][static_cast<std::size_t>(s)] = at + rr * glm::vec2(std::cos(aa), std::sin(aa));
        }

    // Every cell between two cracks and two rings, cut to the pane.
    std::vector<glm::vec2> cell;
    for (std::size_t k = 0; k < rings; ++k)
        for (int s = 0; s < spokes; ++s) {
            const std::size_t s0 = static_cast<std::size_t>(s), s1 = static_cast<std::size_t>((s + 1) % spokes);
            if (k == 0) cell = {at, ring[0][s0], ring[0][s1]};
            else        cell = {ring[k - 1][s0], ring[k][s0], ring[k][s1], ring[k - 1][s1]};
            if (signedArea(cell) < 0.0f) std::reverse(cell.begin(), cell.end());
            Piece piece;
            for (std::size_t i = 0; i + 2 < pane.outline.size(); i += 3) {
                glm::vec2 ta = pane.outline[i], tb = pane.outline[i + 1], tc = pane.outline[i + 2];
                if (cross2(tb - ta, tc - ta) < 0.0f) std::swap(tb, tc);
                std::vector<glm::vec2> part = clipToTri(cell, ta, tb, tc);
                if (part.size() >= 3 && signedArea(part) > kTiny) piece.push_back(std::move(part));
            }
            if (!piece.empty()) out.push_back(std::move(piece));
        }
    return out;
}

std::vector<Shard> makeShards(const Pane& pane, const std::vector<Piece>& pieces, const glm::vec2& at,
                              const glm::vec3& dir, float strength, std::uint32_t seed) {
    std::vector<Shard> out;
    Rng rng(seed);
    const glm::vec3 along = glm::length(dir) > 1e-6f ? glm::normalize(dir) : -pane.n;
    const float half = pane.thickness * 0.5f;
    float reachOut = 1e-3f;
    for (const glm::vec2& p : pane.outline) reachOut = std::max(reachOut, glm::length(p - at));

    for (const Piece& piece : pieces) {
        // Its middle, by area.
        glm::vec2 c(0.0f);
        float area = 0.0f;
        for (const auto& poly : piece)
            for (std::size_t i = 1; i + 1 < poly.size(); ++i) {
                const float a = 0.5f * cross2(poly[i] - poly[0], poly[i + 1] - poly[0]);
                c += a * (poly[0] + poly[i] + poly[i + 1]) / 3.0f;
                area += a;
            }
        if (area < kTiny) continue;
        c /= area;

        Shard sh;
        sh.pos  = pane.world(c);
        sh.face = pane.n;
        const auto local = [&](const glm::vec2& p, float side) {
            return pane.u * (p.x - c.x) + pane.v * (p.y - c.y) + pane.n * (side * half);
        };
        const auto vert = [&](const glm::vec3& p, const glm::vec3& nrm, const glm::vec2& q) {
            fitzel::Vertex v{};
            v.position = p;
            v.normal = nrm;
            v.uv = pane.uvAt(q);
            return v;
        };
        // An edge two of its polygons share (where a cell crossed the outline's
        // diagonal) is inside the shard and gets no wall.
        const auto shared = [&](const glm::vec2& a, const glm::vec2& b, std::size_t self) {
            for (std::size_t j = 0; j < piece.size(); ++j) {
                if (j == self) continue;
                const auto& o = piece[j];
                for (std::size_t i = 0; i < o.size(); ++i)
                    if (glm::length(o[i] - b) < 1e-5f && glm::length(o[(i + 1) % o.size()] - a) < 1e-5f) return true;
            }
            return false;
        };
        for (std::size_t pi = 0; pi < piece.size(); ++pi) {
            const auto& poly = piece[pi];
            for (std::size_t i = 1; i + 1 < poly.size(); ++i) {
                // Front, as it faced; back, the other way round.
                sh.shape.push_back(vert(local(poly[0], 1.0f), pane.n, poly[0]));
                sh.shape.push_back(vert(local(poly[i], 1.0f), pane.n, poly[i]));
                sh.shape.push_back(vert(local(poly[i + 1], 1.0f), pane.n, poly[i + 1]));
                sh.shape.push_back(vert(local(poly[0], -1.0f), -pane.n, poly[0]));
                sh.shape.push_back(vert(local(poly[i + 1], -1.0f), -pane.n, poly[i + 1]));
                sh.shape.push_back(vert(local(poly[i], -1.0f), -pane.n, poly[i]));
            }
            for (std::size_t i = 0; i < poly.size(); ++i) {
                const glm::vec2& a = poly[i];
                const glm::vec2& b = poly[(i + 1) % poly.size()];
                if (glm::length(b - a) < 1e-6f || shared(a, b, pi)) continue;
                const glm::vec3 edge = pane.u * (b.x - a.x) + pane.v * (b.y - a.y);
                const glm::vec3 outN = glm::normalize(glm::cross(edge, pane.n));
                const glm::vec3 af = local(a, 1.0f), ab = local(a, -1.0f), bf = local(b, 1.0f), bb = local(b, -1.0f);
                sh.shape.push_back(vert(af, outN, a));
                sh.shape.push_back(vert(ab, outN, a));
                sh.shape.push_back(vert(bb, outN, b));
                sh.shape.push_back(vert(af, outN, a));
                sh.shape.push_back(vert(bb, outN, b));
                sh.shape.push_back(vert(bf, outN, b));
            }
        }

        // Off it goes: with the shot, and out from the hole, the faster the nearer
        // it lay; tumbling; the outer ones a moment late, as the frame lets go.
        const glm::vec2 off = c - at;
        const float dist = glm::length(off);
        const float nearHole = 1.0f / (1.0f + (dist / 0.06f) * (dist / 0.06f));
        const glm::vec3 radial = dist > 1e-4f ? glm::normalize(pane.u * off.x + pane.v * off.y) : glm::vec3(0.0f);
        sh.vel = along * (strength * (0.4f + 2.6f * nearHole) * rng.uni(0.7f, 1.3f)) +
                 radial * (strength * 0.9f * nearHole * rng.uni(0.5f, 1.2f)) + rng.inBall() * 0.15f;
        sh.spin = rng.inBall() * (rng.uni(2.0f, 12.0f) * (0.4f + nearHole));
        sh.wait = dist > 0.06f ? rng.uni(0.0f, 0.18f) * std::min(1.0f, dist / std::max(reachOut, 0.3f)) : 0.0f;
        out.push_back(std::move(sh));
    }
    return out;
}

float lowest(const Shard& s) {
    float y = 1.0e30f;
    for (const fitzel::Vertex& v : s.shape) y = std::min(y, (s.q * v.position).y);
    return s.pos.y + y;
}

void step(Shard& s, float dt, const FloorFn& floor) {
    if (s.resting || s.lost || dt <= 0.0f) return;
    if (s.wait > 0.0f) {
        s.wait -= dt;
        if (s.wait > 0.0f) return;
    }
    if (!s.settling) {
        s.vel.y -= kG * dt;
        s.vel *= std::exp(-0.25f * dt);
        s.pos += s.vel * dt;
        const float w = glm::length(s.spin);
        if (w > 1e-6f) s.q = glm::normalize(glm::angleAxis(w * dt, s.spin / w) * s.q);
    }
    // What is under it: asked again once it has gone 20 cm sideways.
    const glm::vec2 moved(s.pos.x - s.floorAt.x, s.pos.z - s.floorAt.z);
    if (glm::dot(moved, moved) > 0.04f) {
        float y = 0.0f;
        s.hasFloor = floor && floor(s.pos + glm::vec3(0.0f, 0.05f, 0.0f), y);
        s.floorY = s.hasFloor ? y : -1.0e30f;
        s.floorAt = s.pos;
    }
    if (!s.hasFloor) {
        if (s.vel.y < -25.0f) s.lost = true;   // nothing below for seconds: gone
        return;
    }
    if (s.settling) {
        // Lying down: the nearer face turns to the ground.
        const glm::vec3 f = s.q * s.face;
        const glm::vec3 up(0.0f, f.y >= 0.0f ? 1.0f : -1.0f, 0.0f);
        const glm::quat flatQ = turnOnto(f, up) * s.q;
        if (glm::dot(f, up) > 0.9998f) {
            s.q = flatQ;
            s.resting = true;
        } else {
            s.q = glm::normalize(glm::slerp(s.q, flatQ, 1.0f - std::exp(-18.0f * dt)));
        }
        s.pos.y += s.floorY + 0.0005f - lowest(s);
        return;
    }
    const float low = lowest(s);
    if (low < s.floorY) {
        s.pos.y += s.floorY - low;
        if (s.vel.y < -1.5f) {   // a bounce, a little one
            s.vel.y = -s.vel.y * 0.22f;
            s.vel.x *= 0.45f;
            s.vel.z *= 0.45f;
            s.spin *= 0.5f;
        } else {
            s.settling = true;
            s.vel = glm::vec3(0.0f);
            s.spin = glm::vec3(0.0f);
        }
    }
}

// --- System -----------------------------------------------------------------------

System::System() = default;
System::~System() = default;

bool System::breakAt(const std::vector<Entity>& entities, ModelLibrary& models,
                     const std::vector<MaterialDef>& materials, const Document& document, int id,
                     const glm::vec3& hit, const glm::vec3& dir, float strength, int& vanished) {
    vanished = -1;
    if (id < 0) {
        // The world: whichever object near the point has glass there.
        for (const Entity& c : entities) {
            if (c.id < 0 || !c.activeInHierarchy || glm::length(hit - c.center) > glm::length(c.half) + 0.01f) continue;
            if (!c.components.get<ModelComponent>() && !c.components.get<MaterialComponent>()) continue;
            if (breakAt(entities, models, materials, document, c.id, hit, dir, strength, vanished)) return true;
        }
        return false;
    }
    const Entity* e = findEntity(entities, id);
    if (!e || !e->activeInHierarchy || m_vanished.count(id)) return false;
    // The shot struck the glass itself, not the frame a couple of centimetres off.
    constexpr float kReach = 0.006f;
    const auto matOf = [&](const fitzel::AssetId& a) -> const MaterialDef* {
        const int mi = document.materialIndex(a);
        return mi >= 0 && mi < static_cast<int>(materials.size()) ? &materials[static_cast<std::size_t>(mi)] : nullptr;
    };

    if (const auto* mdl = e->components.get<ModelComponent>()) {
        LoadedModel* lm = models.byId(mdl->modelId);
        if (!lm || lm->animated) return false;
        const glm::mat4 mm = modelToWorld(*e, *lm);
        for (std::size_t i = 0; i < lm->meshes.size() && i < lm->primMaterialId.size(); ++i) {
            const MaterialDef* md = matOf(lm->primMaterialId[i]);
            if (!md || !breakable(*md)) continue;
            const auto key = std::make_pair(lm->id, i);
            auto cpu = m_cpu.find(key);
            if (cpu == m_cpu.end()) cpu = m_cpu.emplace(key, lm->meshes[i].readback().vertices).first;
            const std::vector<fitzel::Vertex>& vs = cpu->second;
            const std::size_t nt = vs.size() / 3;
            if (nt == 0) continue;
            std::vector<glm::vec3> tris(nt * 3);
            std::vector<glm::vec2> uvs(nt * 3);
            for (std::size_t k = 0; k < nt * 3; ++k) {
                tris[k] = glm::vec3(mm * glm::vec4(vs[k].position, 1.0f));
                uvs[k] = vs[k].uv;
            }
            static const std::vector<bool> none;
            auto left = m_left.find({id, i});
            const std::vector<bool>& skip = left != m_left.end() ? left->second.gone : none;
            Pane pane;
            std::vector<std::uint32_t> taken;
            if (!paneAt(tris, uvs, skip, hit, kReach, pane, taken)) continue;
            // This copy draws what is left; the model and its other copies keep theirs.
            Left& l = m_left[{id, i}];
            if (l.gone.size() != nt) l.gone.assign(nt, false);
            for (std::uint32_t t : taken) l.gone[t] = true;
            std::vector<std::uint32_t> idx;
            for (std::size_t t = 0; t < nt; ++t)
                if (!l.gone[t])
                    for (std::uint32_t k = 0; k < 3; ++k) idx.push_back(static_cast<std::uint32_t>(t * 3) + k);
            l.mesh = idx.empty() ? fitzel::Mesh{} : fitzel::Mesh::createView(lm->meshes[i], idx);
            m_holes[id].push_back(pane);
            addFall(lm->primMaterialId[i], pane, hit, dir, strength);
            return true;
        }
        return false;
    }

    // An object of glass: broken as a whole, if it is a pane -- a Plane, a flat
    // Box, a modelled sheet -- and not a block or a bowl.
    if (e->type == EntityType::Empty || e->type == EntityType::Light || e->type == EntityType::Sun ||
        e->type == EntityType::Model || e->components.get<DecalComponent>())
        return false;
    const auto* mc = e->components.get<MaterialComponent>();
    const MaterialDef* md = mc ? matOf(mc->material) : nullptr;
    if (!md || !breakable(*md)) return false;
    std::vector<glm::vec3> tris;
    if (const auto* meshC = e->components.get<MeshComponent>()) {
        const modifiers::Shown shown = modifiers::shown(*e, *meshC);
        if (!shown.mesh) return false;
        addFaces(*shown.mesh, scenegraph::compose(e->center, e->rotation, editmesh::fitScale(*shown.mesh, e->half)), tris);
    } else if (isSolidPrimitive(e->type)) {
        addFaces(primitiveOf(*e), scenegraph::compose(e->center, e->rotation, glm::vec3(1.0f)), tris);
    }
    Pane pane;
    std::vector<std::uint32_t> taken;
    if (!paneAt(tris, {}, {}, hit, kReach, pane, taken) || taken.size() != tris.size() / 3) return false;
    m_vanished.insert(id);
    m_holes[id].push_back(pane);
    addFall(mc->material, pane, hit, dir, strength);
    vanished = id;
    return true;
}

void System::addFall(const fitzel::AssetId& material, const Pane& pane, const glm::vec3& hit,
                     const glm::vec3& dir, float strength) {
    m_seed = m_seed * 1664525u + 1013904223u;
    const glm::vec2 at = pane.flat(hit);
    auto f = std::make_unique<Fall>();
    f->material = material;
    f->shards = makeShards(pane, crack(pane, at, m_seed), at, dir, strength, m_seed ^ 0x5bd1e995u);
    if (f->shards.empty()) return;
    build(*f);
    m_falls.push_back(std::move(f));
    // Too many lying about: the oldest panes' shards are swept up.
    while (m_falls.size() > 1 && shardCount() > kMaxShards) m_falls.erase(m_falls.begin());
}

void System::build(Fall& f) {
    std::vector<fitzel::Vertex> vs;
    for (const Shard& s : f.shards)
        for (const fitzel::Vertex& v : s.shape) {
            fitzel::Vertex w = v;
            w.position = s.pos + s.q * v.position;
            w.normal = s.q * v.normal;
            vs.push_back(w);
        }
    if (vs.empty()) {
        f.mesh = fitzel::Mesh{};
        return;
    }
    if (f.mesh.vertexCount() == 0) f.mesh = fitzel::Mesh::create(vs);
    else                           f.mesh.update(vs);
}

bool System::gone(int id, const glm::vec3& p) const {
    if (id < 0) {
        for (const auto& [owner, panes] : m_holes)
            for (const Pane& pane : panes)
                if (pane.contains(p, 0.01f)) return true;
        return false;
    }
    if (m_vanished.count(id)) return true;
    auto it = m_holes.find(id);
    if (it == m_holes.end()) return false;
    for (const Pane& pane : it->second)
        if (pane.contains(p, 0.01f)) return true;
    return false;
}

const fitzel::Mesh* System::leftOf(int id, std::size_t prim) const {
    auto it = m_left.find({id, prim});
    return it != m_left.end() ? &it->second.mesh : nullptr;
}

void System::update(float dt, const FloorFn& floor) {
    if (dt <= 0.0f) return;
    // Steps of at most 1/120 s: a shard is thin, and a long frame would drop it
    // through a floor's top in one go.
    const int n = std::clamp(static_cast<int>(std::ceil(dt * 120.0f)), 1, 8);
    const float h = std::min(dt, 8.0f / 120.0f) / static_cast<float>(n);
    for (auto& f : m_falls) {
        if (!f->moving) continue;
        for (int k = 0; k < n; ++k)
            for (Shard& s : f->shards) step(s, h, floor);
        f->shards.erase(std::remove_if(f->shards.begin(), f->shards.end(), [](const Shard& s) { return s.lost; }),
                        f->shards.end());
        f->moving = false;
        for (const Shard& s : f->shards) f->moving = f->moving || !s.resting;
        build(*f);
    }
}

void System::submit(fitzel::Renderer& renderer, const std::vector<fitzel::Material>& gpuMats,
                    const std::vector<MaterialDef>& materials, const Document& document) const {
    for (const auto& f : m_falls) {
        if (f->mesh.vertexCount() == 0) continue;
        const int mi = document.materialIndex(f->material);
        if (mi < 0 || mi >= static_cast<int>(gpuMats.size()) || mi >= static_cast<int>(materials.size())) continue;
        const MaterialDef& md = materials[static_cast<std::size_t>(mi)];
        renderer.submit(f->mesh, gpuMats[static_cast<std::size_t>(mi)], glm::mat4(1.0f),
                        /*castsPointShadow=*/false, /*reflective=*/false, md.opacity,
                        /*forceTransparent=*/md.alphaMode == AlphaMode::Blend || md.glass);
    }
}

void System::clear() {
    m_falls.clear();
    m_left.clear();
    m_holes.clear();
    m_vanished.clear();
    m_cpu.clear();
}

std::size_t System::shardCount() const {
    std::size_t n = 0;
    for (const auto& f : m_falls) n += f->shards.size();
    return n;
}

} // namespace shatter
