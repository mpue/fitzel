#include "ProcNodeKit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// The procedural graphs' architecture: node kinds for buildings and bridges.
// Same rules as ProcNodes.cpp -- a class each, its settings as Property rows,
// cook(), one line in the registrar at the bottom.
//
// A building is a footprint pulled up (Rectangle or Curve, filled, then
// Extrude), crowned by a Roof and dressed by a Facade: the walls cut into
// storeys and bays, windows and doors set back into them. Roof before Facade --
// the facade's ledges face up, and a roof would grow on every one of them.
// Blocks are combined the way houses are: a wing beside the main block, a
// tower on its corner, a setback on top, each its own little chain, merged.
//
// A bridge is lines: a path for the deck (swept with a rectangle), its edges
// (Offset) for railings, a Truss along it, Arches under it, cables hanging
// between towers (Arch curve with a negative rise) and hangers dropped from
// them onto the deck (Drop lines).

namespace {

using namespace proc::kit;

const glm::vec3 kUp(0.0f, 1.0f, 0.0f);

// --- Faces -------------------------------------------------------------------------

glm::vec3 newell(const EditMesh& m, const std::vector<int>& loop) {
    glm::vec3 n(0.0f);
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const glm::vec3& a = m.verts[static_cast<std::size_t>(loop[i])];
        const glm::vec3& b = m.verts[static_cast<std::size_t>(loop[(i + 1) % loop.size()])];
        n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    return n;
}

// A face of these corners that looks toward `away`, however they were listed:
// the one rule every piece below is built by, so nothing here works out a
// winding by hand. A corner repeating its neighbour is dropped (a ridge that
// met itself); under three left, no face. Grown out of face `like` (its
// material and texture placement), or plain when -1. Returns the face or -1.
int faceToward(EditMesh& m, std::vector<int> loop, const glm::vec3& away, int like = -1) {
    loop.resize(static_cast<std::size_t>(std::unique(loop.begin(), loop.end()) - loop.begin()));
    while (loop.size() > 1 && loop.front() == loop.back()) loop.pop_back();
    if (loop.size() < 3) return -1;
    if (glm::dot(newell(m, loop), away) < 0.0f) std::reverse(loop.begin() + 1, loop.end());
    if (like >= 0) return addFace(m, std::move(loop), like);
    const bool mats = !m.faceMat.empty(), uvs = !m.faceUV.empty();
    if (mats) m.syncFaceMat();
    if (uvs)  m.syncFaceUv();
    m.faces.push_back(std::move(loop));
    if (mats) m.faceMat.push_back(fitzel::AssetId{});
    if (uvs)  m.faceUV.push_back(EditMesh::FaceUV{});
    return static_cast<int>(m.faces.size()) - 1;
}

void dress(EditMesh& m, int f, const fitzel::AssetId& mat) {
    if (f >= 0 && mat.valid()) m.setFaceMaterial(f, mat);
}

// A box from `a` to `b`, `w` wide and `h` tall: a strut, a chord, a rail, a
// post, a ledge. Its height stands as near `up` as the run allows (a post's
// `up` is the way the railing runs, which squares it with the rails).
void addBeam(EditMesh& m, const glm::vec3& a, const glm::vec3& b, float w, float h,
             glm::vec3 up, const fitzel::AssetId& mat = {}) {
    glm::vec3 t = b - a;
    const float len = glm::length(t);
    if (len < 1e-5f || w <= 0.0f || h <= 0.0f) return;
    t /= len;
    glm::vec3 side = glm::cross(t, up);
    if (glm::dot(side, side) < 1e-8f) side = glm::cross(t, glm::vec3(1.0f, 0.0f, 0.0f));
    if (glm::dot(side, side) < 1e-8f) side = glm::cross(t, glm::vec3(0.0f, 0.0f, 1.0f));
    side = glm::normalize(side);
    up = glm::cross(side, t);
    int c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = addCorner(m, ((i & 1) ? b : a) + side * (((i & 2) ? 0.5f : -0.5f) * w) +
                                up * (((i & 4) ? 0.5f : -0.5f) * h));
    dress(m, faceToward(m, {c[0], c[2], c[6], c[4]}, -t), mat);
    dress(m, faceToward(m, {c[1], c[3], c[7], c[5]}, t), mat);
    dress(m, faceToward(m, {c[0], c[1], c[5], c[4]}, -side), mat);
    dress(m, faceToward(m, {c[2], c[3], c[7], c[6]}, side), mat);
    dress(m, faceToward(m, {c[0], c[1], c[3], c[2]}, -up), mat);
    dress(m, faceToward(m, {c[4], c[5], c[7], c[6]}, up), mat);
}

// --- Outlines in a plane --------------------------------------------------------------

float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

float area2(const std::vector<glm::vec2>& p) {
    float a = 0.0f;
    for (std::size_t i = 0; i < p.size(); ++i) a += cross2(p[i], p[(i + 1) % p.size()]);
    return 0.5f * a;
}

// Counter-clockwise and never turning right (straight on is fine).
bool convex2(const std::vector<glm::vec2>& p) {
    const std::size_t n = p.size();
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
        const glm::vec2 e1 = b - a, e2 = c - b;
        if (cross2(e1, e2) < -1e-4f * glm::length(e1) * glm::length(e2)) return false;
    }
    return true;
}

// What of a convex polygon lies where dot(p, nrm) <= c.
std::vector<glm::vec2> clipHalf(const std::vector<glm::vec2>& poly, const glm::vec2& nrm, float c) {
    std::vector<glm::vec2> out;
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec2& a = poly[i];
        const glm::vec2& b = poly[(i + 1) % n];
        const float da = glm::dot(a, nrm) - c, db = glm::dot(b, nrm) - c;
        const float eps = 1e-6f;
        if (da <= eps) out.push_back(a);
        if ((da < -eps && db > eps) || (da > eps && db < -eps))
            out.push_back(a + (b - a) * (da / (da - db)));
    }
    return out;
}

// Every edge of a counter-clockwise outline moved `d` outward (inward when
// negative), the corners mitred -- capped at four times `d`, so a needle of a
// corner does not throw a spike across the roof.
std::vector<glm::vec2> offsetLoop(const std::vector<glm::vec2>& p, float d) {
    const std::size_t n = p.size();
    std::vector<glm::vec2> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
        glm::vec2 e1 = b - a, e2 = c - b;
        e1 = glm::dot(e1, e1) > 1e-12f ? glm::normalize(e1) : glm::vec2(1.0f, 0.0f);
        e2 = glm::dot(e2, e2) > 1e-12f ? glm::normalize(e2) : e1;
        const glm::vec2 n1(e1.y, -e1.x), n2(e2.y, -e2.x);   // outward: right of a CCW run
        const float k = 1.0f + glm::dot(n1, n2);
        glm::vec2 off = k < 1e-3f ? n1 * d : (n1 + n2) * (d / k);
        const float cap = 4.0f * std::fabs(d);
        if (glm::length(off) > cap) off = glm::normalize(off) * cap;
        out[i] = b + off;
    }
    return out;
}

// A plane to draw an outline in: 2D points out, 3D points back, `n` up.
struct Frame2 {
    glm::vec3 o, u, v, n;
    glm::vec2 to2(const glm::vec3& p) const { const glm::vec3 d = p - o; return {glm::dot(d, u), glm::dot(d, v)}; }
    glm::vec3 to3(const glm::vec2& q, float h = 0.0f) const { return o + u * q.x + v * q.y + n * h; }
    glm::vec3 dir(const glm::vec2& q) const { return u * q.x + v * q.y; }
};

// Corners for one piece, found again by where they are: two faces meeting at
// a ridge share its corners although each worked the ridge out for itself.
// Seeded with corners the piece must reuse (the face it stands on).
struct Welder {
    EditMesh& m;
    std::vector<int> known;
    int at(const glm::vec3& p) {
        for (int i : known)
            if (glm::length(m.verts[static_cast<std::size_t>(i)] - p) < 1e-3f) return i;
        const int i = addCorner(m, p);
        known.push_back(i);
        return i;
    }
};

// ================================================================================
// Arches
// ================================================================================

const std::vector<std::string>& archShapes() {
    static const std::vector<std::string> l = {"Round", "Elliptic", "Pointed (gothic)", "Parabola"};
    return l;
}

// An arch from (-span/2, 0) to (span/2, 0) through (0, rise), left to right.
// Round is the arc of a circle through those three points -- a half circle
// when the rise is half the span, a flat segmental arch below that, a
// horseshoe above; a negative rise hangs the curve down, which is a cable.
std::vector<glm::vec2> archLine(int shape, float span, float rise, int segs) {
    const float a = 0.5f * std::max(span, 0.01f);
    const bool down = rise < 0.0f;
    const float r = std::fabs(rise);
    segs = std::clamp(segs, 1, 512);
    std::vector<glm::vec2> p;
    if (r < 1e-4f) return {{-a, 0.0f}, {a, 0.0f}};
    auto round = [&] {
        const float yc = (r * r - a * a) / (2.0f * r);
        const float R  = r - yc;
        const float thR = std::atan2(-yc, a), thL = kPi - thR;
        for (int k = 0; k <= segs; ++k) {
            const float th = thL + (thR - thL) * static_cast<float>(k) / static_cast<float>(segs);
            p.emplace_back(R * std::cos(th), yc + R * std::sin(th));
        }
    };
    switch (shape) {
        case 1:
            for (int k = 0; k <= segs; ++k) {
                const float t = kPi * static_cast<float>(k) / static_cast<float>(segs);
                p.emplace_back(-a * std::cos(t), r * std::sin(t));
            }
            break;
        case 2: {
            if (r <= a * 1.0001f) { round(); break; }
            // Two arcs, each centred on the springing line on the far side:
            // its radius is what puts the apex at the rise.
            const float c = (r * r - a * a) / (2.0f * a);
            const float R = c + a;
            const int half = std::max(1, (segs + 1) / 2);
            const float phiA = std::atan2(r, -c);
            for (int k = 0; k <= half; ++k) {
                const float phi = kPi + (phiA - kPi) * static_cast<float>(k) / static_cast<float>(half);
                p.emplace_back(c + R * std::cos(phi), R * std::sin(phi));
            }
            for (int k = half - 1; k >= 0; --k) {
                const glm::vec2 q = p[static_cast<std::size_t>(k)];   // a copy: p grows under it
                p.emplace_back(-q.x, q.y);
            }
            break;
        }
        case 3:
            for (int k = 0; k <= segs; ++k) {
                const float x = -a + 2.0f * a * static_cast<float>(k) / static_cast<float>(segs);
                p.emplace_back(x, r * (1.0f - (x / a) * (x / a)));
            }
            break;
        default: round(); break;
    }
    p.front() = {-a, 0.0f};
    p.back()  = {a, 0.0f};
    if (down)
        for (glm::vec2& q : p) q.y = -q.y;
    return p;
}

// Span along X, or turned to run along Z.
glm::mat4 spanFrame(int along, const glm::vec3& center) {
    glm::mat4 xf = glm::translate(glm::mat4(1.0f), center);
    if (along == 1) xf = xf * glm::rotate(glm::mat4(1.0f), -0.5f * kPi, kUp);
    return xf;
}

class ArchCurveNode : public proc::NodeOf<ArchCurveNode> {
public:
    float     span     = 10.0f;
    float     rise     = 4.0f;
    int       shape    = 0;
    int       segments = 16;
    bool      filled   = false;
    int       along    = 0;
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "archcurve"; }
    const char* displayName() const override { return "Arch curve"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = ArchCurveNode;
            std::vector<Property> v;
            v.push_back(number("Span", "span", &S::span, 0.5f, 0.01f, 100000.0f));
            v.push_back(number("Rise (minus: hangs)", "rise", &S::rise, 0.5f, -100000.0f, 100000.0f));
            v.push_back(choice("Shape", "shape", &S::shape, archShapes()));
            v.push_back(whole("Segments", "segments", &S::segments, 1, 512));
            v.push_back(flag("Filled (closed by its chord)", "filled", &S::filled));
            v.push_back(choice("Span along", "along", &S::along, {"X", "Z"}));
            v.push_back(vector3("Center", "center", &S::center, 0.5f));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        std::vector<glm::vec3> pts;
        for (const glm::vec2& q : archLine(shape, span, rise, segments)) pts.emplace_back(q.x, q.y, 0.0f);
        if (filled && std::fabs(rise) > 1e-4f) {
            // Facing +Z (the way it is seen when spanning X), however it bends.
            glm::vec3 n(0.0f);
            for (std::size_t i = 0; i < pts.size(); ++i) n += glm::cross(pts[i], pts[(i + 1) % pts.size()]);
            if (n.z < 0.0f) std::reverse(pts.begin(), pts.end());
            emitLoop(out, pts, true, true);
        } else {
            emitLoop(out, pts, false, false);
        }
        transformGeo(out, spanFrame(along, center));
        return "";
    }
};

// A wall with an arched opening through it: one bay of an arcade, a viaduct,
// a city gate, a bridge tower's portal. Standing on y = 0, the opening's
// sides rising straight to `spring`, the arch above them, the wall `thickness`
// deep. A closed solid: copy it in a row and the bays stand side by side.
class ArchNode : public proc::NodeOf<ArchNode> {
public:
    float     width     = 10.0f;
    float     height    = 9.0f;
    float     thickness = 2.0f;
    float     span      = 6.0f;
    float     rise      = 3.0f;
    float     spring    = 3.5f;
    int       shape     = 0;
    int       segments  = 16;
    int       along     = 0;
    glm::vec3 center{0.0f};

    const char* typeId() const override { return "arch"; }
    const char* displayName() const override { return "Arch"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = ArchNode;
            std::vector<Property> v;
            v.push_back(number("Width", "width", &S::width, 0.5f, 0.2f, 100000.0f));
            v.push_back(number("Height", "height", &S::height, 0.5f, 0.1f, 100000.0f));
            v.push_back(number("Thickness", "thickness", &S::thickness, 0.25f, 0.01f, 100000.0f));
            v.push_back(number("Opening width", "span", &S::span, 0.5f, 0.05f, 100000.0f));
            v.push_back(number("Arch rise", "rise", &S::rise, 0.25f, 0.0f, 100000.0f));
            v.push_back(number("Sides up to", "spring", &S::spring, 0.25f, 0.0f, 100000.0f));
            v.push_back(choice("Shape", "shape", &S::shape, archShapes()));
            v.push_back(whole("Segments", "segments", &S::segments, 2, 256));
            v.push_back(choice("Wall along", "along", &S::along, {"X", "Z"}));
            v.push_back(vector3("Center", "center", &S::center, 0.5f));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        const float hx = 0.5f * std::max(width, 0.2f);
        const float a  = std::min(0.5f * std::max(span, 0.05f), hx - 0.05f);
        const float sp = std::max(spring, 0.0f);
        const float r  = std::max(rise, 0.0f);
        const float H  = std::max(height, sp + r + 0.1f);
        const float T  = std::max(thickness, 0.01f);
        std::vector<glm::vec2> A = archLine(shape, 2.0f * a, r, r < 1e-4f ? 1 : segments);
        for (glm::vec2& q : A) q.y += sp;
        const int N = static_cast<int>(A.size()) - 1;

        // Where a ray from the middle of the springing line through each arch
        // point leaves the wall: the arch point's partner on the outside, on
        // a side (0 left, 1 top, 2 right). Between two rays lies one face.
        const glm::vec2 c(0.0f, sp);
        std::vector<glm::vec2> O(A.size());
        std::vector<int> side(A.size());
        for (std::size_t k = 0; k < A.size(); ++k) {
            const glm::vec2 d = A[k] - c;
            const float tx = d.x < -1e-9f ? -hx / d.x : d.x > 1e-9f ? hx / d.x : 1e30f;
            const float ty = d.y > 1e-9f ? (H - sp) / d.y : 1e30f;
            if (tx <= ty) { O[k] = c + d * tx; side[k] = d.x < 0.0f ? 0 : 2; }
            else          { O[k] = c + d * ty; side[k] = 1; }
        }
        // The outline, counter-clockwise seen from +Z: along the foot, up and
        // over the opening, out along the foot, then round the outside -- up
        // the right side, over the top, down the left.
        std::vector<glm::vec2> ring;
        auto put = [&](const glm::vec2& q) {
            for (const glm::vec2& b : ring)
                if (glm::length(b - q) < 1e-5f) return;
            ring.push_back(q);
        };
        put({-hx, 0.0f});
        put({-a, 0.0f});
        for (const glm::vec2& q : A) put(q);
        put({a, 0.0f});
        put({hx, 0.0f});
        for (int k = N; k >= 0; --k) if (side[static_cast<std::size_t>(k)] == 2) put(O[static_cast<std::size_t>(k)]);
        put({hx, H});
        for (int k = N; k >= 0; --k) if (side[static_cast<std::size_t>(k)] == 1) put(O[static_cast<std::size_t>(k)]);
        put({-hx, H});
        for (int k = N; k >= 0; --k) if (side[static_cast<std::size_t>(k)] == 0) put(O[static_cast<std::size_t>(k)]);

        const std::size_t n = ring.size();
        EditMesh& m = out.mesh;
        std::vector<int> F(n), K(n);
        for (std::size_t i = 0; i < n; ++i) {
            F[i] = addCorner(m, {ring[i].x, ring[i].y, 0.5f * T});
            K[i] = addCorner(m, {ring[i].x, ring[i].y, -0.5f * T});
        }
        auto indexOf = [&](const glm::vec2& q) {
            for (std::size_t i = 0; i < n; ++i)
                if (glm::length(ring[i] - q) < 1e-5f) return static_cast<int>(i);
            return -1;
        };
        // One piece of the face, front and back.
        auto both = [&](const std::vector<glm::vec2>& piece) {
            std::vector<int> f, b;
            for (const glm::vec2& q : piece) {
                const int i = indexOf(q);
                if (i < 0) return;
                f.push_back(F[static_cast<std::size_t>(i)]);
                b.push_back(K[static_cast<std::size_t>(i)]);
            }
            faceToward(m, f, {0.0f, 0.0f, 1.0f});
            faceToward(m, b, {0.0f, 0.0f, -1.0f});
        };
        if (sp > 1e-5f) {
            both({{-hx, 0.0f}, {-a, 0.0f}, {-a, sp}, {-hx, sp}});
            both({{a, 0.0f}, {hx, 0.0f}, {hx, sp}, {a, sp}});
        }
        for (int k = 0; k < N; ++k) {
            const std::size_t i = static_cast<std::size_t>(k), j = i + 1;
            std::vector<glm::vec2> piece = {A[i], A[j], O[j]};
            // Back along the outside from the later ray to the earlier: past
            // the top corners between them.
            if (side[j] == 2 && side[i] != 2) piece.push_back({hx, H});
            if (side[i] == 0 && side[j] != 0) piece.push_back({-hx, H});
            piece.push_back(O[i]);
            both(piece);
        }
        // The rim: every edge of the outline drawn back through the wall.
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const glm::vec2 e = ring[j] - ring[i];
            faceToward(m, {F[i], F[j], K[j], K[i]}, {e.y, -e.x, 0.0f});
        }
        transformGeo(out, spanFrame(along, center));
        return "";
    }
};

// ================================================================================
// Lines for structures
// ================================================================================

// Every line moved sideways, level, square to its run -- the edges of a deck
// from its middle line, a railing's line inside a balcony's, a second cable
// beside the first. Positive is to the right walking along the line (seen
// from above); on a closed outline, outward.
class OffsetNode : public proc::NodeOf<OffsetNode> {
public:
    float distance = 2.0f;
    bool  both     = false;
    bool  keep     = false;

    const char* typeId() const override { return "offset"; }
    const char* displayName() const override { return "Offset"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            number("Distance", "distance", &OffsetNode::distance, 0.25f, -100000.0f, 100000.0f),
            flag("Both sides", "both", &OffsetNode::both),
            flag("Keep the original", "keep", &OffsetNode::keep),
        };
        return p;
    }
    static Curve shifted(const Curve& c, float d) {
        const int n = static_cast<int>(c.pts.size());
        const bool closed = c.closed && n >= 3;
        float dd = d;
        if (closed) {
            glm::vec3 nw(0.0f);
            for (int i = 0; i < n; ++i)
                nw += glm::cross(c.pts[static_cast<std::size_t>(i)], c.pts[static_cast<std::size_t>((i + 1) % n)]);
            if (nw.y < 0.0f) dd = -d;   // clockwise from above: outward is to the left
        }
        // Each run's right-hand side; a run straight up borrows its neighbour's.
        const int segs = closed ? n : n - 1;
        std::vector<glm::vec3> right(static_cast<std::size_t>(segs), glm::vec3(0.0f));
        for (int s = 0; s < segs; ++s) {
            const glm::vec3 t = c.pts[static_cast<std::size_t>((s + 1) % n)] - c.pts[static_cast<std::size_t>(s)];
            glm::vec3 r = glm::cross(t, kUp);
            r.y = 0.0f;
            if (glm::dot(r, r) > 1e-10f) right[static_cast<std::size_t>(s)] = glm::normalize(r);
        }
        for (int s = 1; s < segs; ++s)
            if (glm::dot(right[static_cast<std::size_t>(s)], right[static_cast<std::size_t>(s)]) < 0.5f)
                right[static_cast<std::size_t>(s)] = right[static_cast<std::size_t>(s - 1)];
        for (int s = segs - 2; s >= 0; --s)
            if (glm::dot(right[static_cast<std::size_t>(s)], right[static_cast<std::size_t>(s)]) < 0.5f)
                right[static_cast<std::size_t>(s)] = right[static_cast<std::size_t>(s + 1)];
        Curve o = c;
        o.nrm.clear();
        for (int i = 0; i < n; ++i) {
            const int sa = closed ? (i - 1 + segs) % segs : std::max(i - 1, 0);
            const int sb = closed ? i % segs : std::min(i, segs - 1);
            const glm::vec3 ra = right[static_cast<std::size_t>(sa)], rb = right[static_cast<std::size_t>(sb)];
            glm::vec3 off(0.0f);
            const float k = 1.0f + glm::dot(ra, rb);
            if (glm::dot(ra, ra) > 0.5f) off = k < 1e-3f ? ra * dd : (ra + rb) * (dd / k);
            const float cap = 4.0f * std::fabs(dd);
            if (glm::length(off) > cap) off = glm::normalize(off) * cap;
            o.pts[static_cast<std::size_t>(i)] += off;
        }
        return o;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        if (!firstLine(out)) return "No lines to offset";
        std::vector<Curve> made;
        for (const Curve& c : out.curves) {
            if (c.loose || c.pts.size() < 2) { made.push_back(c); continue; }
            if (keep) made.push_back(c);
            made.push_back(shifted(c, distance));
            if (both) made.push_back(shifted(c, -distance));
        }
        out.curves = std::move(made);
        return "";
    }
};

// A straight line down from every point (the selected ones) of the first
// input to the faces of the second below it -- or, with nothing below, to a
// height. Swept, the lines are piers under a deck, columns under a roof,
// hangers from a cable to the road.
class DropLinesNode : public proc::NodeOf<DropLinesNode> {
public:
    int   dir    = 0;     // 0 down, 1 up
    float height = 0.0f;  // where a line that hits nothing stops
    bool  keep   = false;

    const char* typeId() const override { return "droplines"; }
    const char* displayName() const override { return "Drop lines"; }
    int inputSlots() const override { return 2; }
    const char* inputName(int slot) const override { return slot == 0 ? "From points" : "Onto (optional)"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = {
            choice("Direction", "dir", &DropLinesNode::dir, {"Down", "Up"}),
            number("Else stop at height", "height", &DropLinesNode::height, 0.5f, -100000.0f, 100000.0f),
            flag("Keep the input", "keep", &DropLinesNode::keep),
        };
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (in.empty() || !in[0]) return "Nothing wired into From points";
        const Geo& src = *in[0];
        const Geo* onto = in.size() > 1 ? in[1] : nullptr;
        std::vector<glm::vec3> from;
        for (int i = 0; i < static_cast<int>(src.mesh.verts.size()); ++i)
            if (src.meshPicked(i)) from.push_back(src.mesh.verts[static_cast<std::size_t>(i)]);
        for (const Curve& c : src.curves)
            for (int i = 0; i < static_cast<int>(c.pts.size()); ++i)
                if (Geo::curvePicked(c, i, src.hasSel)) from.push_back(c.pts[static_cast<std::size_t>(i)]);
        if (from.empty()) return "No points to drop lines from";
        struct Tri { glm::vec3 a, b, c; };
        std::vector<Tri> tris;
        if (onto)
            for (const std::vector<int>& f : onto->mesh.faces)
                for (std::size_t k = 1; k + 1 < f.size(); ++k)
                    tris.push_back({onto->mesh.verts[static_cast<std::size_t>(f[0])],
                                    onto->mesh.verts[static_cast<std::size_t>(f[k])],
                                    onto->mesh.verts[static_cast<std::size_t>(f[k + 1])]});
        if (static_cast<double>(from.size()) * static_cast<double>(tris.size()) > 4e7)
            return "Too much to test: fewer points, or simpler faces to drop onto";
        if (keep) out = src;
        const float s = dir == 1 ? 1.0f : -1.0f;
        int made = 0;
        for (const glm::vec3& p : from) {
            // The nearest face straight below (above): its height under p, by
            // the triangle's own plane, wherever p falls inside it.
            float best = 1e30f;
            for (const Tri& t : tris) {
                const glm::vec2 a(t.a.x, t.a.z), b(t.b.x, t.b.z), c(t.c.x, t.c.z), q(p.x, p.z);
                const float den = cross2(b - a, c - a);
                if (std::fabs(den) < 1e-12f) continue;
                const float u = cross2(q - a, c - a) / den, v = cross2(b - a, q - a) / den;
                if (u < -1e-6f || v < -1e-6f || u + v > 1.0f + 1e-6f) continue;
                const float y = t.a.y + u * (t.b.y - t.a.y) + v * (t.c.y - t.a.y);
                const float d = (y - p.y) * s;
                if (d > 1e-4f && d < best) best = d;
            }
            glm::vec3 end;
            if (best < 1e29f) end = p + glm::vec3(0.0f, s * best, 0.0f);
            else if ((height - p.y) * s > 1e-3f) end = glm::vec3(p.x, height, p.z);
            else continue;
            Curve line;
            line.pts = {p, end};
            out.curves.push_back(std::move(line));
            ++made;
        }
        return made ? "" : "Every point is already past the stopping height";
    }
};

// Posts every so far along each line, a rail on top, rails between, and bars
// or a solid panel in each bay: a bridge's railing, a balcony's, a parapet.
// Each run of the line is cut into even bays, so a corner always has a post.
class RailingNode : public proc::NodeOf<RailingNode> {
public:
    float height     = 1.0f;
    float spacing    = 2.0f;
    float post       = 0.08f;
    float rail       = 0.06f;
    int   midRails   = 1;
    int   fill       = 0;     // 0 nothing, 1 bars, 2 panels
    float barSpacing = 0.12f;
    float bar        = 0.02f;

    const char* typeId() const override { return "railing"; }
    const char* displayName() const override { return "Railing"; }
    int inputSlots() const override { return 1; }
    const char* inputName(int) const override { return "Along lines"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = RailingNode;
            std::vector<Property> v;
            v.push_back(number("Height", "height", &S::height, 0.1f, 0.05f, 1000.0f));
            v.push_back(number("Post every", "spacing", &S::spacing, 0.25f, 0.1f, 1000.0f));
            v.push_back(number("Post size", "post", &S::post, 0.01f, 0.005f, 100.0f, "%.3f"));
            v.push_back(number("Rail size", "rail", &S::rail, 0.01f, 0.005f, 100.0f, "%.3f"));
            v.push_back(whole("Rails between", "midRails", &S::midRails, 0, 8));
            v.push_back(choice("Fill", "fill", &S::fill, {"Nothing", "Bars", "Panels"}));
            auto bars = [](Property pr) {
                pr.visible = [](const void* o) { return static_cast<const S*>(o)->fill == 1; };
                return pr;
            };
            v.push_back(bars(number("Bar every", "barSpacing", &S::barSpacing, 0.01f, 0.03f, 10.0f, "%.3f")));
            v.push_back(bars(number("Bar size", "bar", &S::bar, 0.005f, 0.002f, 10.0f, "%.3f")));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        if (!firstLine(*in[0])) return "No lines to run along";
        EditMesh& m = out.mesh;
        const float h = std::max(height, 0.05f);
        const float gap = std::min(0.1f, 0.2f * h);
        for (const Curve& c : in[0]->curves) {
            const int n = static_cast<int>(c.pts.size());
            if (c.loose || n < 2) continue;
            const bool closed = c.closed && n >= 3;
            std::vector<glm::vec3> posts;
            const int segs = closed ? n : n - 1;
            for (int s = 0; s < segs; ++s) {
                const glm::vec3 a = c.pts[static_cast<std::size_t>(s)], b = c.pts[static_cast<std::size_t>((s + 1) % n)];
                const int parts = std::max(1, static_cast<int>(std::ceil(glm::length(b - a) / std::max(spacing, 0.1f) - 1e-4f)));
                for (int k = 0; k < parts; ++k) posts.push_back(glm::mix(a, b, static_cast<float>(k) / static_cast<float>(parts)));
            }
            if (!closed) posts.push_back(c.pts.back());
            const int np = static_cast<int>(posts.size());
            const int bays = closed ? np : np - 1;
            for (int i = 0; i < np; ++i) {
                const glm::vec3 p = posts[static_cast<std::size_t>(i)];
                const glm::vec3 nb = posts[static_cast<std::size_t>(closed ? (i + 1) % np : std::min(i + 1, np - 1))];
                const glm::vec3 pb = posts[static_cast<std::size_t>(closed ? (i - 1 + np) % np : std::max(i - 1, 0))];
                glm::vec3 run = nb - pb;
                run.y = 0.0f;
                if (glm::dot(run, run) < 1e-10f) run = glm::vec3(1.0f, 0.0f, 0.0f);
                addBeam(m, p, p + kUp * h, post, post, glm::normalize(run));
            }
            for (int i = 0; i < bays; ++i) {
                const glm::vec3 a = posts[static_cast<std::size_t>(i)], b = posts[static_cast<std::size_t>((i + 1) % np)];
                addBeam(m, a + kUp * (h - 0.5f * rail), b + kUp * (h - 0.5f * rail), rail, rail, kUp);
                for (int r = 1; r <= midRails; ++r) {
                    const float y = gap + (h - gap) * static_cast<float>(r) / static_cast<float>(midRails + 1);
                    addBeam(m, a + kUp * y, b + kUp * y, rail * 0.7f, rail * 0.7f, kUp);
                }
                const float len = glm::length(b - a);
                if (fill == 1) {
                    const int nbars = std::max(0, static_cast<int>(len / std::max(barSpacing, 0.03f)) - 1);
                    for (int k = 1; k <= nbars; ++k) {
                        const glm::vec3 q = glm::mix(a, b, static_cast<float>(k) / static_cast<float>(nbars + 1));
                        addBeam(m, q + kUp * gap, q + kUp * (h - rail), bar, bar, b - a);
                    }
                } else if (fill == 2 && len > post) {
                    const glm::vec3 t = (b - a) / len;
                    const float mid = 0.5f * (gap + h - rail);
                    addBeam(m, a + t * (0.5f * post) + kUp * mid, b - t * (0.5f * post) + kUp * mid,
                            post * 0.5f, h - rail - gap, kUp);
                }
                if (m.faces.size() > 400000) return "Stopped at 400000 faces: fewer bars";
            }
        }
        return "";
    }
};

// A girder of struts along each line: chords top and bottom, posts, and
// diagonals in the pattern bridges are named after. Two sides with floor
// beams (and bracing over the top) for a through-truss bridge; one side for
// a roof truss or a crane's arm.
class TrussNode : public proc::NodeOf<TrussNode> {
public:
    float height     = 4.0f;
    float width      = 6.0f;
    float panel      = 4.0f;
    float size       = 0.3f;
    int   pattern    = 1;   // 0 Warren, 1 Pratt, 2 Howe, 3 X
    int   sides      = 0;   // 0 two sides, 1 one side
    bool  topBracing = true;
    bool  floorBeams = true;

    const char* typeId() const override { return "truss"; }
    const char* displayName() const override { return "Truss"; }
    int inputSlots() const override { return 1; }
    const char* inputName(int) const override { return "Along lines"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = TrussNode;
            std::vector<Property> v;
            auto two = [](Property pr) {
                pr.visible = [](const void* o) { return static_cast<const S*>(o)->sides == 0; };
                return pr;
            };
            v.push_back(number("Height", "height", &S::height, 0.25f, 0.05f, 10000.0f));
            v.push_back(two(number("Width", "width", &S::width, 0.25f, 0.05f, 10000.0f)));
            v.push_back(number("Panel length", "panel", &S::panel, 0.25f, 0.1f, 10000.0f));
            v.push_back(number("Strut size", "size", &S::size, 0.05f, 0.005f, 100.0f, "%.3f"));
            v.push_back(choice("Pattern", "pattern", &S::pattern, {"Warren", "Pratt", "Howe", "X-braced"}));
            v.push_back(choice("Sides", "sides", &S::sides, {"Two (a bridge)", "One (a roof truss)"}));
            v.push_back(two(flag("Bracing over the top", "top", &S::topBracing)));
            v.push_back(two(flag("Floor beams", "floor", &S::floorBeams)));
            return v;
        }();
        return p;
    }
    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        if (!firstLine(*in[0])) return "No lines to run along";
        EditMesh& m = out.mesh;
        const float sz = std::max(size, 0.005f);
        for (const Curve& c : in[0]->curves) {
            const int n = static_cast<int>(c.pts.size());
            if (c.loose || n < 2) continue;
            const bool closed = c.closed && n >= 3;
            std::vector<glm::vec3> st;   // stations: where the posts stand
            const int segs = closed ? n : n - 1;
            for (int s = 0; s < segs; ++s) {
                const glm::vec3 a = c.pts[static_cast<std::size_t>(s)], b = c.pts[static_cast<std::size_t>((s + 1) % n)];
                const int parts = std::max(1, static_cast<int>(std::round(glm::length(b - a) / std::max(panel, 0.1f))));
                for (int k = 0; k < parts; ++k) st.push_back(glm::mix(a, b, static_cast<float>(k) / static_cast<float>(parts)));
            }
            if (!closed) st.push_back(c.pts.back());
            const int ns = static_cast<int>(st.size());
            const int panels = closed ? ns : ns - 1;
            // Each station's frame: along, level across, and up square to both.
            std::vector<glm::vec3> T(static_cast<std::size_t>(ns)), S(static_cast<std::size_t>(ns)), U(static_cast<std::size_t>(ns));
            for (int i = 0; i < ns; ++i) {
                const int a = closed ? (i - 1 + ns) % ns : std::max(i - 1, 0);
                const int b = closed ? (i + 1) % ns : std::min(i + 1, ns - 1);
                glm::vec3 t = st[static_cast<std::size_t>(b)] - st[static_cast<std::size_t>(a)];
                t = glm::dot(t, t) > 1e-12f ? glm::normalize(t) : glm::vec3(1.0f, 0.0f, 0.0f);
                glm::vec3 s = glm::cross(t, kUp);
                s = glm::dot(s, s) > 1e-8f ? glm::normalize(s) : glm::vec3(0.0f, 0.0f, 1.0f);
                T[static_cast<std::size_t>(i)] = t;
                S[static_cast<std::size_t>(i)] = s;
                U[static_cast<std::size_t>(i)] = glm::cross(s, t);
            }
            auto wrap = [&](int i) { return static_cast<std::size_t>(((i % ns) + ns) % ns); };
            const int planes = sides == 0 ? 2 : 1;
            auto bot = [&](int pl, int i) {
                const float off = planes == 2 ? (pl == 0 ? -0.5f : 0.5f) * width : 0.0f;
                return st[wrap(i)] + S[wrap(i)] * off;
            };
            auto topAt = [&](int pl, int i) { return bot(pl, i) + U[wrap(i)] * height; };
            for (int pl = 0; pl < planes; ++pl) {
                for (int i = 0; i < panels; ++i) {
                    addBeam(m, bot(pl, i), bot(pl, i + 1), sz, sz, U[wrap(i)]);
                    addBeam(m, topAt(pl, i), topAt(pl, i + 1), sz, sz, U[wrap(i)]);
                    // Pratt's diagonals lean down toward the middle, Howe's
                    // up toward it; Warren's zig-zag; X has both.
                    const bool firstHalf = (static_cast<float>(i) + 0.5f) < 0.5f * static_cast<float>(panels);
                    bool fall = false, climb = false;
                    if (pattern == 0)      { if (i % 2 == 0) climb = true; else fall = true; }
                    else if (pattern == 1) { if (firstHalf) fall = true; else climb = true; }
                    else if (pattern == 2) { if (firstHalf) climb = true; else fall = true; }
                    else                   { fall = climb = true; }
                    if (fall)  addBeam(m, topAt(pl, i), bot(pl, i + 1), sz * 0.8f, sz * 0.8f, T[wrap(i)]);
                    if (climb) addBeam(m, bot(pl, i), topAt(pl, i + 1), sz * 0.8f, sz * 0.8f, T[wrap(i)]);
                }
                for (int i = 0; i < ns; ++i) {
                    const bool end = !closed && (i == 0 || i == ns - 1);
                    if (pattern != 0 || end) addBeam(m, bot(pl, i), topAt(pl, i), sz, sz, T[wrap(i)]);
                }
            }
            if (planes == 2) {
                for (int i = 0; i < ns; ++i) {
                    if (floorBeams) addBeam(m, bot(0, i), bot(1, i), sz, sz, U[wrap(i)]);
                    if (topBracing) addBeam(m, topAt(0, i), topAt(1, i), sz * 0.8f, sz * 0.8f, U[wrap(i)]);
                }
                if (topBracing)
                    for (int i = 0; i < panels; ++i) {
                        const int a = i % 2;
                        addBeam(m, topAt(a, i), topAt(1 - a, i + 1), sz * 0.6f, sz * 0.6f, U[wrap(i)]);
                    }
            }
            if (m.faces.size() > 400000) return "Stopped at 400000 faces: longer panels";
        }
        return "";
    }
};

// ================================================================================
// Buildings
// ================================================================================

// A roof on each picked face (the tops of the blocks): flat behind a
// parapet, a gable, a hip, a pyramid or a lean-to shed, at one pitch, with
// eaves reaching out past the walls. The face goes; the roof is a closed
// solid on the walls' top, so it can overhang without a hole under it.
// Pitched roofs need a convex outline (a hip on an L is a skeleton this does
// not draw): on one that is not, the roof stays flat -- build the L of two
// blocks and give each its own roof, as builders do.
class RoofNode : public proc::NodeOf<RoofNode> {
public:
    int   faces    = static_cast<int>(FaceSet::Up);
    float share    = 1.0f;
    int   seed     = 1;
    int   kind     = 2;      // 0 flat, 1 gable, 2 hip, 3 pyramid, 4 shed
    float pitch    = 35.0f;  // degrees
    float overhang = 0.4f;
    bool  across   = false;  // gable/shed: the ridge across the short way
    float parapet  = 1.0f;
    float wall     = 0.3f;
    std::string material;

    const char* typeId() const override { return "roof"; }
    const char* displayName() const override { return "Roof"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = RoofNode;
            std::vector<Property> v;
            faceRows<S>(v);
            v.push_back(choice("Kind", "kind", &S::kind,
                               {"Flat, with a parapet", "Gable", "Hip", "Pyramid", "Shed (lean-to)"}));
            auto pitched = [](Property pr) {
                pr.visible = [](const void* o) { return static_cast<const S*>(o)->kind != 0; };
                return pr;
            };
            auto flat = [](Property pr) {
                pr.visible = [](const void* o) { return static_cast<const S*>(o)->kind == 0; };
                return pr;
            };
            v.push_back(pitched(number("Pitch (deg)", "pitch", &S::pitch, 5.0f, 0.0f, 80.0f, "%.0f")));
            v.push_back(pitched(number("Overhang", "overhang", &S::overhang, 0.1f, 0.0f, 100.0f)));
            Property ac = flag("Ridge across the short way", "across", &S::across);
            ac.visible = [](const void* o) { const int k = static_cast<const S*>(o)->kind; return k == 1 || k == 4; };
            v.push_back(std::move(ac));
            v.push_back(flat(number("Parapet height", "parapet", &S::parapet, 0.1f, 0.0f, 100.0f)));
            v.push_back(flat(number("Parapet thickness", "wall", &S::wall, 0.05f, 0.01f, 100.0f)));
            v.push_back(field("Material", "material", PropKind::Text, &S::material));
            return v;
        }();
        return p;
    }

    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        const std::vector<int> fs = proc::pickFaces(out, static_cast<FaceSet>(faces), share, seed);
        if (fs.empty()) return "No faces of that kind";
        const fitzel::AssetId mat = fitzel::AssetId::fromString(material);
        std::vector<char> gone(out.mesh.faces.size(), 0);
        int flattened = 0;
        for (int f : fs) {
            bool flat = false;
            if (roofOn(out.mesh, f, mat, flat)) gone[static_cast<std::size_t>(f)] = 1;
            if (flat) ++flattened;
        }
        out.syncSel();
        dropFaces(out, gone);
        return flattened ? std::to_string(flattened) + " outline(s) not convex: flat roofs there" : "";
    }

private:
    // Roof face f. True when the face was replaced (and goes); `flat` says a
    // pitched roof had to be flat.
    bool roofOn(EditMesh& m, int f, const fitzel::AssetId& mat, bool& flat) const {
        std::vector<int> loop = m.faces[static_cast<std::size_t>(f)];
        if (loop.size() < 3) return false;
        const glm::vec3 n = m.faceNormal(f);
        // In the face's own plane, x along its longest edge.
        std::size_t le = 0;
        float best = -1.0f;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const float l = glm::length(m.verts[static_cast<std::size_t>(loop[(i + 1) % loop.size()])] -
                                        m.verts[static_cast<std::size_t>(loop[i])]);
            if (l > best) { best = l; le = i; }
        }
        glm::vec3 u = m.verts[static_cast<std::size_t>(loop[(le + 1) % loop.size()])] -
                      m.verts[static_cast<std::size_t>(loop[le])];
        u -= n * glm::dot(u, n);
        if (glm::dot(u, u) < 1e-12f) return false;
        u = glm::normalize(u);
        const Frame2 F{m.faceCenter(f), u, glm::cross(n, u), n};
        std::vector<glm::vec2> P;
        for (int v : loop) P.push_back(F.to2(m.verts[static_cast<std::size_t>(v)]));
        if (area2(P) < 0.0f) {   // a face runs counter-clockwise round its normal; be sure
            std::reverse(P.begin(), P.end());
            std::reverse(loop.begin(), loop.end());
        }
        // The outline without the corners that only lie on a straight edge (a
        // facade's cuts): kept[k] is where outline corner k sits in `loop`.
        std::vector<int> kept;
        const std::size_t ln = loop.size();
        for (std::size_t i = 0; i < ln; ++i) {
            const glm::vec2 a = P[(i + ln - 1) % ln], b = P[i], c = P[(i + 1) % ln];
            const glm::vec2 e1 = b - a, e2 = c - b;
            if (glm::length(e2) < 1e-5f) continue;
            if (std::fabs(cross2(e1, e2)) <= 1e-4f * glm::length(e1) * glm::length(e2) && glm::dot(e1, e2) > 0.0f)
                continue;
            kept.push_back(static_cast<int>(i));
        }
        if (kept.size() < 3) return false;
        std::vector<glm::vec2> Q;
        for (int k : kept) Q.push_back(P[static_cast<std::size_t>(k)]);

        int type = kind;
        if (type != 0 && !convex2(Q)) { type = 0; flat = true; }
        if ((type == 1 || type == 4) && Q.size() != 4) type = 2;

        Welder W{m, {}};
        for (int v : loop) W.known.push_back(v);
        // A face along the outline at overhang 0 meets the walls' top edge;
        // the corners a facade cut into that edge go into it too, so no crack
        // opens between the two.
        auto chains = [&](const std::vector<int>& f2) {
            std::vector<int> outl;
            for (std::size_t i = 0; i < f2.size(); ++i) {
                const int a = f2[i], b = f2[(i + 1) % f2.size()];
                outl.push_back(a);
                for (std::size_t k = 0; k < kept.size(); ++k) {
                    const std::size_t s = static_cast<std::size_t>(kept[k]);
                    const std::size_t e = static_cast<std::size_t>(kept[(k + 1) % kept.size()]);
                    std::vector<int> mid;
                    for (std::size_t j = (s + 1) % ln; j != e; j = (j + 1) % ln) mid.push_back(loop[j]);
                    if (mid.empty()) continue;
                    if (a == loop[s] && b == loop[e]) outl.insert(outl.end(), mid.begin(), mid.end());
                    else if (a == loop[e] && b == loop[s]) outl.insert(outl.end(), mid.rbegin(), mid.rend());
                }
            }
            return outl;
        };
        auto outward = [&](const glm::vec2& a, const glm::vec2& b) {
            const glm::vec2 e = b - a;
            return F.dir(glm::vec2(e.y, -e.x));
        };
        auto roofFace = [&](const std::vector<int>& c) { dress(m, faceToward(m, chains(c), n, f), mat); };
        auto wallFace = [&](const std::vector<int>& c, const glm::vec3& away) { faceToward(m, chains(c), away, f); };

        if (type == 0) {
            if (parapet <= 1e-4f) {
                dress(m, f, mat);   // a flat roof is the face itself
                return false;
            }
            // The whole outline (cuts and all): the parapet carries the walls on up.
            const std::vector<glm::vec2> I = offsetLoop(P, -std::max(wall, 0.01f));
            const float h = parapet;
            std::vector<int> topO(ln), topI(ln), inB(ln);
            for (std::size_t i = 0; i < ln; ++i) {
                topO[i] = W.at(F.to3(P[i], h));
                topI[i] = W.at(F.to3(I[i], h));
                inB[i]  = W.at(F.to3(I[i], 0.0f));
            }
            for (std::size_t i = 0; i < ln; ++i) {
                const std::size_t j = (i + 1) % ln;
                const glm::vec3 out3 = outward(P[i], P[j]);
                faceToward(m, {loop[i], loop[j], topO[j], topO[i]}, out3, f);
                faceToward(m, {topO[i], topO[j], topI[j], topI[i]}, n, f);
                faceToward(m, {topI[i], topI[j], inB[j], inB[i]}, -out3, f);
            }
            dress(m, faceToward(m, inB, n, f), mat);
            return true;
        }

        const float tanP = std::tan(glm::radians(std::clamp(pitch, 0.0f, 80.0f)));
        const float o = std::max(overhang, 0.0f);
        const std::vector<glm::vec2> E = o > 1e-5f ? offsetLoop(Q, o) : Q;
        const float base = -o * tanP;   // the eaves: the slope carried on down past the wall
        const std::size_t ne = E.size();
        // Each edge's inward direction, and how far in from it a point is.
        std::vector<glm::vec2> inw(ne);
        std::vector<float> c0(ne);
        for (std::size_t i = 0; i < ne; ++i) {
            const glm::vec2 e = glm::normalize(E[(i + 1) % ne] - E[i]);
            inw[i] = glm::vec2(-e.y, e.x);
            c0[i] = glm::dot(E[i], inw[i]);
        }
        auto dist = [&](std::size_t i, const glm::vec2& p) { return glm::dot(p, inw[i]) - c0[i]; };
        auto eave = [&](std::size_t i) { return W.at(F.to3(E[i], base)); };
        // The part of the outline nearer edge i than any of `rivals`, lifted
        // by its distance from i: one slope of the roof. For a convex outline
        // the slopes of all edges are exactly a hip roof's.
        auto slope = [&](std::size_t i, const std::vector<std::size_t>& rivals) {
            std::vector<glm::vec2> R = E;
            for (std::size_t j : rivals) {
                if (j == i) continue;
                const glm::vec2 d = inw[i] - inw[j];
                if (glm::dot(d, d) < 1e-10f) continue;
                R = clipHalf(R, d, c0[i] - c0[j]);
            }
            std::vector<int> c;
            for (const glm::vec2& q : R) c.push_back(W.at(F.to3(q, base + tanP * dist(i, q))));
            roofFace(c);
        };

        if (type == 2) {
            std::vector<std::size_t> every(ne);
            for (std::size_t i = 0; i < ne; ++i) every[i] = i;
            for (std::size_t i = 0; i < ne; ++i) slope(i, every);
        } else if (type == 3) {
            glm::vec2 ctr(0.0f);
            float ar = 0.0f;
            for (std::size_t i = 0; i < ne; ++i) {
                const float w = cross2(E[i], E[(i + 1) % ne]);
                ctr += (E[i] + E[(i + 1) % ne]) * w;
                ar += w;
            }
            ctr = std::fabs(ar) > 1e-9f ? ctr / (3.0f * ar) : E[0];
            float mean = 0.0f;
            for (std::size_t i = 0; i < ne; ++i) mean += dist(i, ctr);
            mean /= static_cast<float>(ne);
            const int apex = W.at(F.to3(ctr, base + tanP * mean));
            for (std::size_t i = 0; i < ne; ++i) roofFace({eave(i), eave((i + 1) % ne), apex});
        } else {
            // Gable and shed: four sides, two of them carrying the roof.
            const float l02 = glm::length(E[1] - E[0]) + glm::length(E[3] - E[2]);
            const float l13 = glm::length(E[2] - E[1]) + glm::length(E[0] - E[3]);
            const std::size_t s1 = ((l02 >= l13) != across) ? 0 : 1;
            const std::size_t s2 = s1 + 2;
            if (type == 1) {
                slope(s1, {s2});
                slope(s2, {s1});
                for (std::size_t g : {(s1 + 1) % 4, (s1 + 3) % 4}) {
                    // The gable: up to where the two slopes meet over this edge.
                    const glm::vec2 a = E[g], b = E[(g + 1) % 4];
                    const float fa = dist(s1, a) - dist(s2, a), fb = dist(s1, b) - dist(s2, b);
                    const float t = std::fabs(fa - fb) > 1e-9f ? fa / (fa - fb) : 0.5f;
                    const glm::vec2 r = glm::mix(a, b, std::clamp(t, 0.0f, 1.0f));
                    const int ridge = W.at(F.to3(r, base + tanP * dist(s1, r)));
                    wallFace({eave(g), eave((g + 1) % 4), ridge}, outward(a, b));
                }
            } else {
                // Shed: one slope rising from edge s1 to the far side.
                std::vector<int> lifted(4);
                for (std::size_t i = 0; i < 4; ++i) lifted[i] = W.at(F.to3(E[i], base + tanP * dist(s1, E[i])));
                roofFace(lifted);
                for (std::size_t g = 0; g < 4; ++g) {
                    if (g == s1) continue;
                    const std::size_t h = (g + 1) % 4;
                    wallFace({eave(g), eave(h), lifted[h], lifted[g]}, outward(E[g], E[h]));
                }
            }
        }
        if (o > 1e-5f) {
            // Under the eaves.
            std::vector<int> c;
            for (std::size_t i = 0; i < ne; ++i) c.push_back(eave(i));
            faceToward(m, c, -n, f);
        }
        return true;
    }
};

// Walls into storeys and bays, a window set back into each bay of each
// storey -- or on the ground floor a door, shop windows, or nothing -- with a
// frame round the glass and a ledge along every floor. Works on the picked
// faces that stand up and have four corners (the walls of an extruded
// footprint); the ground floor is the bottom storey of the walls that reach
// lowest, so a setback tower on a podium gets windows all the way down.
// Corners cut into an edge two walls share are shared, and pushed into the
// faces beside (the roof, the floor) too: the result stays closed.
class FacadeNode : public proc::NodeOf<FacadeNode> {
public:
    int   faces        = static_cast<int>(FaceSet::Sides);
    float share        = 1.0f;
    int   seed         = 1;
    float floorHeight  = 3.2f;
    float groundHeight = 4.0f;
    float bay          = 3.0f;
    float winWidth     = 1.4f;
    float winHeight    = 1.6f;
    float sill         = 0.9f;
    float depth        = 0.2f;
    float frame        = 0.08f;
    int   ground       = 2;      // 0 windows like above, 1 shop windows, 2 a door in the middle, 3 plain
    float doorWidth    = 1.6f;
    float doorHeight   = 2.6f;
    float ledge        = 0.12f;
    float ledgeHeight  = 0.25f;
    std::string matGlass, matFrame, matDoor, matLedge;

    const char* typeId() const override { return "facade"; }
    const char* displayName() const override { return "Facade"; }
    int inputSlots() const override { return 1; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = FacadeNode;
            std::vector<Property> v;
            faceRows<S>(v);
            v.push_back(number("Storey height", "floorHeight", &S::floorHeight, 0.1f, 1.0f, 100.0f));
            v.push_back(number("Ground floor height", "groundHeight", &S::groundHeight, 0.1f, 1.0f, 100.0f));
            v.push_back(number("Bay width", "bay", &S::bay, 0.1f, 0.5f, 100.0f));
            v.push_back(number("Window width", "winWidth", &S::winWidth, 0.1f, 0.1f, 100.0f));
            v.push_back(number("Window height", "winHeight", &S::winHeight, 0.1f, 0.1f, 100.0f));
            v.push_back(number("Sill height", "sill", &S::sill, 0.1f, 0.0f, 100.0f));
            v.push_back(number("Set back", "depth", &S::depth, 0.05f, 0.0f, 10.0f));
            v.push_back(number("Frame", "frame", &S::frame, 0.01f, 0.0f, 1.0f, "%.3f"));
            v.push_back(choice("Ground floor", "ground", &S::ground,
                               {"Windows like above", "Shop windows", "A door in the middle", "Plain wall"}));
            auto door = [](Property pr) {
                pr.visible = [](const void* o) { return static_cast<const S*>(o)->ground == 2; };
                return pr;
            };
            auto ledged = [](Property pr) {
                pr.visible = [](const void* o) { return static_cast<const S*>(o)->ledge > 0.0f; };
                return pr;
            };
            v.push_back(door(number("Door width", "doorWidth", &S::doorWidth, 0.1f, 0.4f, 100.0f)));
            v.push_back(door(number("Door height", "doorHeight", &S::doorHeight, 0.1f, 1.0f, 100.0f)));
            v.push_back(number("Ledge depth", "ledge", &S::ledge, 0.02f, 0.0f, 10.0f, "%.3f"));
            v.push_back(ledged(number("Ledge height", "ledgeHeight", &S::ledgeHeight, 0.05f, 0.01f, 10.0f)));
            // Material pickers: the editor draws every "material..." key as one.
            v.push_back(field("Glass", "materialGlass", PropKind::Text, &S::matGlass));
            v.push_back(field("Frames", "materialFrame", PropKind::Text, &S::matFrame));
            v.push_back(door(field("Doors", "materialDoor", PropKind::Text, &S::matDoor)));
            v.push_back(ledged(field("Ledges", "materialLedge", PropKind::Text, &S::matLedge)));
            return v;
        }();
        return p;
    }

    struct Wall {
        int f;
        int c[4];          // bottom-left, bottom-right, top-right, top-left (the face's own order)
        glm::vec3 n, up;
        float W, H, bottom;
    };
    struct Opening { float u0, u1, v0, v1; bool door; };

    std::string cook(const std::vector<const Geo*>& in, Geo& out) const override {
        if (std::string e = needsInput(in); !e.empty()) return e;
        out = *in[0];
        EditMesh& m = out.mesh;
        std::vector<Wall> walls;
        for (int f : proc::pickFaces(out, static_cast<FaceSet>(faces), share, seed)) {
            const std::vector<int>& L = m.faces[static_cast<std::size_t>(f)];
            if (L.size() != 4) continue;
            const glm::vec3 n = m.faceNormal(f);
            if (std::fabs(n.y) > 0.7f) continue;
            const glm::vec3 up = glm::normalize(kUp - n * n.y);
            int lo = 0;
            float low = 1e30f;
            for (int i = 0; i < 4; ++i) {
                const float h = glm::dot(m.verts[static_cast<std::size_t>(L[static_cast<std::size_t>(i)])] +
                                         m.verts[static_cast<std::size_t>(L[static_cast<std::size_t>((i + 1) % 4)])], up);
                if (h < low) { low = h; lo = i; }
            }
            Wall w;
            w.f = f;
            for (int i = 0; i < 4; ++i) w.c[i] = L[static_cast<std::size_t>((lo + i) % 4)];
            w.n = n;
            w.up = up;
            auto V = [&](int i) { return m.verts[static_cast<std::size_t>(w.c[i])]; };
            w.W = 0.5f * (glm::length(V(1) - V(0)) + glm::length(V(2) - V(3)));
            w.H = 0.5f * (glm::length(V(3) - V(0)) + glm::length(V(2) - V(1)));
            w.bottom = std::min(V(0).y, V(1).y);
            if (w.W < 1e-3f || w.H < 1e-3f) continue;
            walls.push_back(w);
        }
        if (walls.empty()) return "No walls among those faces (standing, four corners)";
        float lowest = 1e30f;
        for (const Wall& w : walls) lowest = std::min(lowest, w.bottom);

        const fitzel::AssetId glass = fitzel::AssetId::fromString(matGlass);
        const fitzel::AssetId frm   = fitzel::AssetId::fromString(matFrame);
        const fitzel::AssetId door  = fitzel::AssetId::fromString(matDoor);
        const fitzel::AssetId led   = fitzel::AssetId::fromString(matLedge);

        // Corners cut into an edge of the input, by the edge and how far
        // along it from its lower-numbered end: the wall beside finds them.
        std::unordered_map<std::uint64_t, std::vector<std::pair<float, int>>> cuts;
        auto key = [](int a, int b) {
            return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | static_cast<std::uint32_t>(std::max(a, b));
        };
        auto onEdge = [&](int a, int b, float t) {
            if (t < 1e-5f) return a;
            if (t > 1.0f - 1e-5f) return b;
            const float tt = a < b ? t : 1.0f - t;
            std::vector<std::pair<float, int>>& at = cuts[key(a, b)];
            for (const auto& pr : at)
                if (std::fabs(pr.first - tt) < 1e-4f) return pr.second;
            const int lo = std::min(a, b), hi = std::max(a, b);
            const int i = addCorner(m, glm::mix(m.verts[static_cast<std::size_t>(lo)], m.verts[static_cast<std::size_t>(hi)], tt));
            cuts[key(a, b)].push_back({tt, i});
            return i;
        };

        std::vector<char> gone(m.faces.size(), 0);
        for (const Wall& w : walls) {
            const glm::vec3 p00 = m.verts[static_cast<std::size_t>(w.c[0])], p10 = m.verts[static_cast<std::size_t>(w.c[1])];
            const glm::vec3 p11 = m.verts[static_cast<std::size_t>(w.c[2])], p01 = m.verts[static_cast<std::size_t>(w.c[3])];
            auto P = [&](float um, float vm) {
                const float a = um / w.W, b = vm / w.H;
                return glm::mix(glm::mix(p00, p10, a), glm::mix(p01, p11, a), b);
            };
            // Storeys.
            const bool groundRule = w.bottom - lowest < 0.5f;
            const float g = groundRule ? groundHeight : floorHeight;
            std::vector<float> lines = {0.0f};
            if (w.H < g + 0.5f * floorHeight) {
                lines.push_back(w.H);
            } else {
                const int ups = std::max(1, static_cast<int>(std::round((w.H - g) / std::max(floorHeight, 1.0f))));
                const float fh = (w.H - g) / static_cast<float>(ups);
                lines.push_back(g);
                for (int k = 1; k <= ups; ++k) lines.push_back(g + fh * static_cast<float>(k));
                lines.back() = w.H;
            }
            // Bays, and an opening in each.
            const int nb = std::max(1, static_cast<int>(std::round(w.W / std::max(bay, 0.5f))));
            const float bw = w.W / static_cast<float>(nb);
            std::vector<Opening> ops;
            for (std::size_t s = 0; s + 1 < lines.size(); ++s) {
                const float b0 = lines[s], fh = lines[s + 1] - lines[s];
                const int mode = (groundRule && s == 0) ? ground : 0;
                // Under two metres is no storey: a parapet, a plinth.
                if (mode == 3 || fh < 2.0f) continue;
                for (int b = 0; b < nb; ++b) {
                    const float uc = (static_cast<float>(b) + 0.5f) * bw;
                    if (mode == 2 && b == nb / 2) {
                        const float dw = std::min(doorWidth, bw - 0.3f), dh = std::min(doorHeight, fh - 0.3f);
                        if (dw >= 0.4f && dh >= 1.0f) ops.push_back({uc - 0.5f * dw, uc + 0.5f * dw, b0, b0 + dh, true});
                        continue;
                    }
                    float ww, v0, v1;
                    if (mode == 1) {
                        ww = bw - 0.5f;
                        v0 = b0 + 0.4f;
                        v1 = b0 + fh - 0.5f;
                    } else {
                        ww = std::min(winWidth, bw - 0.4f);
                        const float wh = std::min(winHeight, fh - 0.5f);
                        const float s0 = std::max(0.1f, std::min(sill, fh - wh - 0.25f));
                        v0 = b0 + s0;
                        v1 = v0 + wh;
                    }
                    if (ww < 0.3f || v1 - v0 < 0.3f) continue;
                    ops.push_back({uc - 0.5f * ww, uc + 0.5f * ww, v0, v1, false});
                }
            }
            // One grid over the whole wall through every edge of every
            // opening: each cell is wall or glass, and every corner is shared.
            std::vector<float> U = {0.0f, w.W}, Vv = lines;
            for (const Opening& o : ops) {
                U.push_back(o.u0); U.push_back(o.u1);
                Vv.push_back(o.v0); Vv.push_back(o.v1);
            }
            auto tidy = [](std::vector<float>& x) {
                std::sort(x.begin(), x.end());
                std::vector<float> y;
                for (float t : x)
                    if (y.empty() || t - y.back() > 1e-3f) y.push_back(t);
                x = std::move(y);
            };
            tidy(U);
            tidy(Vv);
            U.back() = w.W;
            Vv.back() = w.H;
            const int nu = static_cast<int>(U.size()), nv = static_cast<int>(Vv.size());
            auto nearest = [](const std::vector<float>& x, float t) {
                int best = 0;
                for (int i = 1; i < static_cast<int>(x.size()); ++i)
                    if (std::fabs(x[static_cast<std::size_t>(i)] - t) < std::fabs(x[static_cast<std::size_t>(best)] - t)) best = i;
                return best;
            };
            std::vector<int> G(static_cast<std::size_t>(nu * nv));
            auto gi = [&](int i, int j) -> int& { return G[static_cast<std::size_t>(j * nu + i)]; };
            for (int j = 0; j < nv; ++j)
                for (int i = 0; i < nu; ++i) {
                    const float a = U[static_cast<std::size_t>(i)] / w.W, b = Vv[static_cast<std::size_t>(j)] / w.H;
                    int& c = gi(i, j);
                    if (j == 0)           c = onEdge(w.c[0], w.c[1], a);
                    else if (j == nv - 1) c = onEdge(w.c[3], w.c[2], a);
                    else if (i == 0)      c = onEdge(w.c[0], w.c[3], b);
                    else if (i == nu - 1) c = onEdge(w.c[1], w.c[2], b);
                    else                  c = addCorner(m, P(U[static_cast<std::size_t>(i)], Vv[static_cast<std::size_t>(j)]));
                }
            auto inOpening = [&](float u, float v) {
                for (const Opening& o : ops)
                    if (u > o.u0 && u < o.u1 && v > o.v0 && v < o.v1) return true;
                return false;
            };
            for (int j = 0; j + 1 < nv; ++j)
                for (int i = 0; i + 1 < nu; ++i) {
                    const float uc = 0.5f * (U[static_cast<std::size_t>(i)] + U[static_cast<std::size_t>(i + 1)]);
                    const float vc = 0.5f * (Vv[static_cast<std::size_t>(j)] + Vv[static_cast<std::size_t>(j + 1)]);
                    if (inOpening(uc, vc)) continue;
                    addFace(m, {gi(i, j), gi(i + 1, j), gi(i + 1, j + 1), gi(i, j + 1)}, w.f);
                }
            // The openings: a reveal round each, the glass (or the door) at
            // the back inside a frame.
            const float d = std::max(depth, 0.0f);
            for (const Opening& o : ops) {
                const int i0 = nearest(U, o.u0), i1 = nearest(U, o.u1), j0 = nearest(Vv, o.v0), j1 = nearest(Vv, o.v1);
                std::vector<std::pair<int, int>> ring;
                for (int i = i0; i < i1; ++i) ring.push_back({i, j0});
                for (int j = j0; j < j1; ++j) ring.push_back({i1, j});
                for (int i = i1; i > i0; --i) ring.push_back({i, j1});
                for (int j = j1; j > j0; --j) ring.push_back({i0, j});
                const glm::vec3 mid = P(0.5f * (o.u0 + o.u1), 0.5f * (o.v0 + o.v1)) - w.n * (0.5f * d);
                std::vector<int> front, back;
                for (const auto& [i, j] : ring) {
                    front.push_back(gi(i, j));
                    back.push_back(d > 1e-5f ? addCorner(m, P(U[static_cast<std::size_t>(i)], Vv[static_cast<std::size_t>(j)]) - w.n * d)
                                             : gi(i, j));
                }
                const std::size_t rn = ring.size();
                if (d > 1e-5f)
                    for (std::size_t k = 0; k < rn; ++k) {
                        const std::size_t k2 = (k + 1) % rn;
                        const glm::vec3 em = 0.5f * (m.verts[static_cast<std::size_t>(front[k])] +
                                                     m.verts[static_cast<std::size_t>(front[k2])]) - w.n * (0.5f * d);
                        faceToward(m, {front[k], front[k2], back[k2], back[k]}, mid - em, w.f);
                    }
                const fitzel::AssetId pane = o.door ? door : glass;
                const float fr = std::min(std::max(frame, 0.0f), 0.3f * std::min(o.u1 - o.u0, o.v1 - o.v0));
                if (fr < 1e-4f) {
                    dress(m, faceToward(m, back, w.n, w.f), pane);
                    continue;
                }
                // The frame: every corner of the back pulled in by its width,
                // corners that land on one another made one.
                std::vector<int> inner;
                std::vector<glm::vec2> seen;
                std::vector<int> seenAt;
                for (const auto& [i, j] : ring) {
                    const float u = std::clamp(U[static_cast<std::size_t>(i)], o.u0 + fr, o.u1 - fr);
                    const float v = std::clamp(Vv[static_cast<std::size_t>(j)], o.v0 + fr, o.v1 - fr);
                    int at = -1;
                    for (std::size_t q = 0; q < seen.size(); ++q)
                        if (glm::length(seen[q] - glm::vec2(u, v)) < 1e-4f) at = seenAt[q];
                    if (at < 0) {
                        at = addCorner(m, P(u, v) - w.n * d);
                        seen.emplace_back(u, v);
                        seenAt.push_back(at);
                    }
                    inner.push_back(at);
                }
                for (std::size_t k = 0; k < rn; ++k) {
                    const std::size_t k2 = (k + 1) % rn;
                    dress(m, faceToward(m, {back[k], back[k2], inner[k2], inner[k]}, w.n, w.f), frm);
                }
                dress(m, faceToward(m, inner, w.n, w.f), pane);
            }
            // Ledges: a band under every floor line and the top, reaching past
            // the wall's ends by its own depth so two walls' bands meet.
            if (ledge > 1e-4f) {
                const float lh = std::max(ledgeHeight, 0.01f);
                const glm::vec3 off = w.n * (0.5f * ledge);
                for (std::size_t s = 1; s < lines.size(); ++s) {
                    const float v = lines[s] - 0.5f * lh;
                    const std::size_t before = m.faces.size();
                    addBeam(m, P(-ledge, v) + off, P(w.W + ledge, v) + off, ledge, lh, w.up);
                    for (std::size_t q = before; q < m.faces.size(); ++q) {
                        if (led.valid()) m.setFaceMaterial(static_cast<int>(q), led);
                        else if (!m.faceMat.empty()) m.faceMat[q] = m.faceMaterial(w.f);
                    }
                }
            }
            gone[static_cast<std::size_t>(w.f)] = 1;
        }
        // The faces beside the walls (roof, floor, a neighbour that was not
        // picked) take the corners cut into the edges they share with them.
        for (std::size_t f = 0; f < m.faces.size(); ++f) {
            if (f < gone.size() && gone[f]) continue;
            const std::vector<int>& L = m.faces[f];
            std::vector<int> grown;
            bool changed = false;
            for (std::size_t i = 0; i < L.size(); ++i) {
                const int a = L[i], b = L[(i + 1) % L.size()];
                grown.push_back(a);
                const auto it = cuts.find(key(a, b));
                if (it == cuts.end()) continue;
                std::vector<std::pair<float, int>> along = it->second;
                // Measured from a's end (the cut's own measure runs from the lower number).
                if (a > b)
                    for (auto& pr : along) pr.first = 1.0f - pr.first;
                std::sort(along.begin(), along.end());
                for (const auto& pr : along)
                    if (std::find(L.begin(), L.end(), pr.second) == L.end()) {
                        grown.push_back(pr.second);
                        changed = true;
                    }
            }
            if (changed) m.faces[f] = std::move(grown);
        }
        out.syncSel();
        dropFaces(out, gone);
        return "";
    }
};

// ================================================================================
// Registration
// ================================================================================

struct RegisterBuildNodes {
    template <class T>
    static void add(const char* category, const char* tip) {
        const T t;
        proc::registerType({t.typeId(), t.displayName(), category, tip,
                            [] { return std::unique_ptr<proc::Node>(std::make_unique<T>()); }});
    }
    RegisterBuildNodes() {
        add<ArchCurveNode>("Curves", "An arch as a line: round, elliptic, pointed or a parabola.\n"
                                     "A negative rise hangs it: a suspension bridge's cable.");
        add<OffsetNode>("Curves", "Lines moved sideways: a deck's edges from its middle,\n"
                                  "a railing inside a balcony, a second cable.");
        add<RoofNode>("Buildings", "A roof on the top faces: flat behind a parapet, gable, hip,\n"
                                   "pyramid or shed, with eaves over the walls.");
        add<FacadeNode>("Buildings", "Walls into storeys and bays with windows set back into\n"
                                     "them; a door, shop windows or nothing on the ground floor,\n"
                                     "ledges along the floors. Put it after the Roof.");
        add<ArchNode>("Buildings", "A wall with an arched opening: an arcade's bay, a viaduct,\n"
                                   "a gate, a bridge tower. Copy it in a row.");
        add<TrussNode>("Structures", "A girder of struts along lines: Warren, Pratt, Howe or\n"
                                     "X-braced; two sides for a bridge, one for a roof truss.");
        add<RailingNode>("Structures", "Posts and rails along lines, with bars or panels between.");
        add<DropLinesNode>("Structures", "A line straight down from every point onto the faces below,\n"
                                         "or to a height: sweep them into piers, columns, hangers.");
    }
} g_registerBuildNodes;

} // namespace
