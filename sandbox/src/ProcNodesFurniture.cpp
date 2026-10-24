#include "ProcNodeKit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// The procedural graphs' furniture: a node kind per piece -- table, chair,
// sofa, bed, cabinet, shelf, rug, lamp -- each a whole piece from a handful of
// measurements, the way a joiner would be told it ("a dining table, 1.80 by
// 0.90, four tapered legs and an apron"). Same rules as ProcNodes.cpp: a class
// each, its settings as Property rows, cook(), one line in the registrar at
// the bottom.
//
// Every piece stands on the floor (y = 0) round its own middle, its FRONT
// toward +Z -- where you sit down, open a drawer, climb into bed -- and takes a
// Position and a Turn, so a room is a Merge of pieces rather than a Merge of
// Transforms of pieces. Width runs along X, depth along Z.
//
// A piece is built from closed solids that may cross each other (a leg runs up
// into the table top, a cushion sinks into the frame): each solid is closed and
// wound outward on its own, so the result renders, collides and passes
// proccheck's closed-and-outward test, and nobody has to boolean anything.
//
// The materials are slots ("materialTop", "materialFabric" ...), like the
// Facade's: empty means the object's own, so a piece added by hand is one grey
// thing until it is dressed, and a preset dresses it from the library.

namespace {

using namespace proc::kit;
using fitzel::AssetId;

const glm::vec3 kUp(0.0f, 1.0f, 0.0f);
const glm::vec3 kZ(0.0f, 0.0f, 1.0f);

AssetId slot(const std::string& s) { return AssetId::fromString(s); }

float clampf(float v, float lo, float hi) { return std::clamp(v, lo, hi); }

// --- Outlines in plan (x, z) ------------------------------------------------------------

using Outline = std::vector<glm::vec2>;

Outline rectOutline(float hx, float hz) {
    return {{hx, hz}, {-hx, hz}, {-hx, -hz}, {hx, -hz}};
}

Outline ellipseOutline(float rx, float rz, int segs) {
    Outline o;
    for (int i = 0; i < segs; ++i) {
        const float a = kTau * static_cast<float>(i) / static_cast<float>(segs);
        o.push_back({rx * std::cos(a), rz * std::sin(a)});
    }
    return o;
}

// A rectangle with its corners rounded by `r`, `k` steps a corner.
Outline roundRectOutline(float hx, float hz, float r, int k = 6) {
    r = std::min({r, hx, hz});
    if (r < 1e-4f) return rectOutline(hx, hz);
    Outline o;
    const glm::vec2 c[4] = {{hx - r, hz - r}, {-(hx - r), hz - r}, {-(hx - r), -(hz - r)}, {hx - r, -(hz - r)}};
    for (int q = 0; q < 4; ++q)
        for (int i = 0; i <= k; ++i) {
            const float a = 0.5f * kPi * (static_cast<float>(q) + static_cast<float>(i) / static_cast<float>(k));
            o.push_back(c[q] + r * glm::vec2(std::cos(a), std::sin(a)));
        }
    return o;
}

std::vector<glm::vec3> lift(const Outline& o, float y, const glm::vec2& at = glm::vec2(0.0f)) {
    std::vector<glm::vec3> r;
    r.reserve(o.size());
    for (const glm::vec2& q : o) r.push_back({q.x + at.x, y, q.y + at.y});
    return r;
}

// The turn about X through `pivot` (y, z): positive tips +Y toward +Z.
glm::mat4 tiltAbout(float deg, float y, float z) {
    return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, y, z)) *
           glm::rotate(glm::mat4(1.0f), glm::radians(deg), glm::vec3(1.0f, 0.0f, 0.0f)) *
           glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -y, -z));
}

// --- The workshop ----------------------------------------------------------------------
//
// Collects solids into one mesh. Inside one solid, corners at the same place
// are one corner (that is what makes it closed); two solids never share one,
// so a leg touching the top at a corner does not stitch the two together.
// Every face is turned to look `away` from where it is told, so nothing here
// works out a winding by hand -- the solids are convex, or (the pipe) told
// face by face which way is out.
struct Shop {
    EditMesh& m;
    std::map<std::array<long long, 3>, int> weld;

    void begin() { weld.clear(); }

    int at(const glm::vec3& p) {
        const std::array<long long, 3> k = {std::llround(p.x * 1e5), std::llround(p.y * 1e5),
                                            std::llround(p.z * 1e5)};
        const auto it = weld.find(k);
        if (it != weld.end()) return it->second;
        const int i = addCorner(m, p);
        weld.emplace(k, i);
        return i;
    }

    void face(const std::vector<glm::vec3>& pts, const glm::vec3& away, const AssetId& mat) {
        std::vector<int> loop;
        for (const glm::vec3& p : pts) {
            const int i = at(p);
            if (loop.empty() || loop.back() != i) loop.push_back(i);
        }
        while (loop.size() > 1 && loop.front() == loop.back()) loop.pop_back();
        if (loop.size() < 3) return;
        glm::vec3 n(0.0f);
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const glm::vec3& a = m.verts[static_cast<std::size_t>(loop[i])];
            const glm::vec3& b = m.verts[static_cast<std::size_t>(loop[(i + 1) % loop.size()])];
            n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
        if (glm::dot(n, away) < 0.0f) std::reverse(loop.begin() + 1, loop.end());
        m.faces.push_back(std::move(loop));
        if (mat.valid()) m.setFaceMaterial(static_cast<int>(m.faces.size()) - 1, mat);
    }

    // A convex solid between two rings of as many corners: a leg, a top, a
    // frustum. The caps take their own materials (a drum shade's rim is the
    // shade, its top and bottom too).
    void loft(const std::vector<glm::vec3>& lo, const std::vector<glm::vec3>& hi, const AssetId& side,
              const AssetId& top, const AssetId& bottom) {
        const std::size_t n = std::min(lo.size(), hi.size());
        if (n < 3) return;
        begin();
        glm::vec3 c(0.0f), cl(0.0f), ch(0.0f);
        for (std::size_t i = 0; i < n; ++i) { cl += lo[i]; ch += hi[i]; }
        cl /= static_cast<float>(n);
        ch /= static_cast<float>(n);
        c = 0.5f * (cl + ch);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const glm::vec3 q = 0.25f * (lo[i] + lo[j] + hi[i] + hi[j]);
            face({lo[i], lo[j], hi[j], hi[i]}, q - c, side);
        }
        face(std::vector<glm::vec3>(hi.begin(), hi.begin() + static_cast<long>(n)), ch - c, top);
        face(std::vector<glm::vec3>(lo.begin(), lo.begin() + static_cast<long>(n)), cl - c, bottom);
    }
    void loft(const std::vector<glm::vec3>& lo, const std::vector<glm::vec3>& hi, const AssetId& mat) {
        loft(lo, hi, mat, mat, mat);
    }

    // An outline pulled up from y0 to y1.
    void slab(const Outline& o, float y0, float y1, const AssetId& mat, const glm::vec2& c = glm::vec2(0.0f)) {
        if (y1 - y0 < 1e-5f) return;
        loft(lift(o, y0, c), lift(o, y1, c), mat);
    }

    void block(const glm::vec3& mn, const glm::vec3& mx, const AssetId& mat) {
        const glm::vec3 a = glm::min(mn, mx), b = glm::max(mn, mx);
        if (b.x - a.x < 1e-5f || b.y - a.y < 1e-5f || b.z - a.z < 1e-5f) return;
        const glm::vec2 c(0.5f * (a.x + b.x), 0.5f * (a.z + b.z));
        slab(rectOutline(0.5f * (b.x - a.x), 0.5f * (b.z - a.z)), a.y, b.y, mat, c);
    }

    // A bar `w` x `h` from a to b: `h` along `up` as near as the run allows,
    // `w` across (cross(run, up)).
    void beam(const glm::vec3& a, const glm::vec3& b, float w, float h, glm::vec3 up, const AssetId& mat) {
        glm::vec3 t = b - a;
        const float len = glm::length(t);
        if (len < 1e-5f || w <= 0.0f || h <= 0.0f) return;
        t /= len;
        glm::vec3 side = glm::cross(t, up);
        if (glm::dot(side, side) < 1e-8f) side = glm::cross(t, glm::vec3(1.0f, 0.0f, 0.0f));
        if (glm::dot(side, side) < 1e-8f) side = glm::cross(t, kZ);
        side = glm::normalize(side);
        up = glm::cross(side, t);
        auto ring = [&](const glm::vec3& p) {
            const glm::vec3 s = side * (0.5f * w), u = up * (0.5f * h);
            return std::vector<glm::vec3>{p - s - u, p + s - u, p + s + u, p - s + u};
        };
        loft(ring(a), ring(b), mat);
    }

    // A round bar from a to b, radius ra there and rb here.
    void rod(const glm::vec3& a, const glm::vec3& b, float ra, float rb, int segs, const AssetId& mat) {
        glm::vec3 t = b - a;
        const float len = glm::length(t);
        if (len < 1e-5f) return;
        t /= len;
        glm::vec3 n = std::fabs(t.y) < 0.9f ? glm::cross(t, kUp) : glm::cross(t, glm::vec3(1.0f, 0.0f, 0.0f));
        n = glm::normalize(n);
        const glm::vec3 bn = glm::cross(t, n);
        std::vector<glm::vec3> lo, hi;
        for (int i = 0; i < segs; ++i) {
            const float ang = kTau * static_cast<float>(i) / static_cast<float>(segs);
            const glm::vec3 d = n * std::cos(ang) + bn * std::sin(ang);
            lo.push_back(a + d * ra);
            hi.push_back(b + d * rb);
        }
        loft(lo, hi, mat);
    }

    // A rounded box: every edge and corner rounded by `r` in `k` steps -- a
    // cushion, a mattress, an upholstered arm. The grid is spaced by angle
    // (tan of even steps), so the rounding comes out circular, not bulged.
    void cushion(const glm::vec3& mn, const glm::vec3& mx, float r, const AssetId& mat, int k = 3) {
        const glm::vec3 lo = glm::min(mn, mx), hi = glm::max(mn, mx);
        const glm::vec3 c = 0.5f * (lo + hi), h = 0.5f * (hi - lo);
        if (h.x < 1e-4f || h.y < 1e-4f || h.z < 1e-4f) return;
        r = std::min(r, 0.95f * std::min({h.x, h.y, h.z}));
        if (r < 2e-3f || k < 1) { block(lo, hi, mat); return; }
        begin();
        const glm::vec3 in = h - glm::vec3(r);
        auto coords = [&](float inA) {
            std::vector<float> v;
            for (int j = k; j >= 1; --j) v.push_back(-(inA + r * std::tan(0.25f * kPi * static_cast<float>(j) / static_cast<float>(k))));
            v.push_back(-inA);
            v.push_back(inA);
            for (int j = 1; j <= k; ++j) v.push_back(inA + r * std::tan(0.25f * kPi * static_cast<float>(j) / static_cast<float>(k)));
            return v;
        };
        const std::vector<float> cs[3] = {coords(in.x), coords(in.y), coords(in.z)};
        auto onto = [&](glm::vec3 p) {
            const glm::vec3 q = glm::clamp(p, -in, in);
            const glm::vec3 d = p - q;
            const float l = glm::length(d);
            return c + q + (l > 1e-9f ? d * (r / l) : glm::vec3(0.0f));
        };
        for (int a = 0; a < 3; ++a)
            for (int sg = -1; sg <= 1; sg += 2) {
                const int b = (a + 1) % 3, e = (a + 2) % 3;
                const std::vector<float>& ub = cs[b];
                const std::vector<float>& ue = cs[e];
                for (std::size_t i = 0; i + 1 < ub.size(); ++i)
                    for (std::size_t j = 0; j + 1 < ue.size(); ++j) {
                        auto pt = [&](std::size_t ii, std::size_t jj) {
                            glm::vec3 p(0.0f);
                            p[a] = static_cast<float>(sg) * h[a];
                            p[b] = ub[ii];
                            p[e] = ue[jj];
                            return onto(p);
                        };
                        const glm::vec3 q0 = pt(i, j), q1 = pt(i + 1, j), q2 = pt(i + 1, j + 1), q3 = pt(i, j + 1);
                        face({q0, q1, q2, q3}, 0.25f * (q0 + q1 + q2 + q3) - c, mat);
                    }
            }
    }

    // A round tube along a polyline, its corners bent with radius `bend`: a
    // steel chair frame, a table's sled runner, a lamp's arm. Open ends are
    // capped; a closed loop has no ends.
    void pipe(const std::vector<glm::vec3>& path, float rad, bool closed, float bend, const AssetId& mat,
              int segs = 10) {
        const int n = static_cast<int>(path.size());
        if (n < 2 || rad <= 0.0f) return;
        std::vector<glm::vec3> p;
        for (int i = 0; i < n; ++i) {
            const bool corner = closed ? n >= 3 : (i > 0 && i < n - 1);
            const glm::vec3 b = path[static_cast<std::size_t>(i)];
            if (!corner) { p.push_back(b); continue; }
            const glm::vec3 a = path[static_cast<std::size_t>((i + n - 1) % n)];
            const glm::vec3 c = path[static_cast<std::size_t>((i + 1) % n)];
            const float la = glm::length(a - b), lc = glm::length(c - b);
            const float d = std::min({bend, 0.45f * la, 0.45f * lc});
            if (d < 1e-4f) { p.push_back(b); continue; }
            const glm::vec3 A = b + (a - b) / la * d, C = b + (c - b) / lc * d;
            for (int s = 0; s <= 4; ++s) {
                const float t = static_cast<float>(s) / 4.0f;
                p.push_back((1.0f - t) * (1.0f - t) * A + 2.0f * (1.0f - t) * t * b + t * t * C);
            }
        }
        std::vector<glm::vec3> q;
        for (const glm::vec3& v : p)
            if (q.empty() || glm::length(v - q.back()) > 1e-5f) q.push_back(v);
        if (closed && q.size() > 1 && glm::length(q.front() - q.back()) <= 1e-5f) q.pop_back();
        const int k = static_cast<int>(q.size());
        if (k < 2) return;
        std::vector<glm::vec3> T(static_cast<std::size_t>(k));
        for (int i = 0; i < k; ++i) {
            glm::vec3 d;
            if (closed) d = q[static_cast<std::size_t>((i + 1) % k)] - q[static_cast<std::size_t>((i + k - 1) % k)];
            else if (i == 0) d = q[1] - q[0];
            else if (i == k - 1) d = q[static_cast<std::size_t>(k - 1)] - q[static_cast<std::size_t>(k - 2)];
            else d = q[static_cast<std::size_t>(i + 1)] - q[static_cast<std::size_t>(i - 1)];
            T[static_cast<std::size_t>(i)] = glm::normalize(d);
        }
        // Parallel transport: the ring does not twist round the tube.
        glm::vec3 N = std::fabs(T[0].y) < 0.9f ? glm::cross(T[0], kUp) : glm::cross(T[0], glm::vec3(1, 0, 0));
        N = glm::normalize(N);
        std::vector<std::vector<glm::vec3>> rings;
        for (int i = 0; i < k; ++i) {
            const glm::vec3 t = T[static_cast<std::size_t>(i)];
            N = N - glm::dot(N, t) * t;
            N = glm::normalize(N);
            const glm::vec3 B = glm::cross(t, N);
            std::vector<glm::vec3> ring;
            for (int s = 0; s < segs; ++s) {
                const float ang = kTau * static_cast<float>(s) / static_cast<float>(segs);
                ring.push_back(q[static_cast<std::size_t>(i)] + rad * (N * std::cos(ang) + B * std::sin(ang)));
            }
            rings.push_back(std::move(ring));
        }
        begin();
        const int spans = closed ? k : k - 1;
        for (int i = 0; i < spans; ++i) {
            const int j = (i + 1) % k;
            const glm::vec3 mid = 0.5f * (q[static_cast<std::size_t>(i)] + q[static_cast<std::size_t>(j)]);
            for (int s = 0; s < segs; ++s) {
                const int u = (s + 1) % segs;
                const auto& a = rings[static_cast<std::size_t>(i)];
                const auto& b = rings[static_cast<std::size_t>(j)];
                const glm::vec3 c4 = 0.25f * (a[static_cast<std::size_t>(s)] + a[static_cast<std::size_t>(u)] +
                                              b[static_cast<std::size_t>(u)] + b[static_cast<std::size_t>(s)]);
                face({a[static_cast<std::size_t>(s)], a[static_cast<std::size_t>(u)], b[static_cast<std::size_t>(u)],
                      b[static_cast<std::size_t>(s)]}, c4 - mid, mat);
            }
        }
        if (!closed) {
            face(rings.front(), -T.front(), mat);
            face(rings.back(), T.back(), mat);
        }
    }

    // Everything built since `from` moved by xf (a turn keeps the winding).
    std::size_t mark() const { return m.verts.size(); }
    void xform(std::size_t from, const glm::mat4& xf) {
        for (std::size_t i = from; i < m.verts.size(); ++i) m.verts[i] = glm::vec3(xf * glm::vec4(m.verts[i], 1.0f));
        weld.clear();
    }
};

// A leg from y0 to y1 at plan point c: square, round or tapered (thinner at
// the floor).
void leg(Shop& s, int shape, const glm::vec2& c, float y0, float y1, float size, const AssetId& mat) {
    if (y1 - y0 < 1e-4f) return;
    const float h = 0.5f * size;
    if (shape == 1) {
        s.rod({c.x, y0, c.y}, {c.x, y1, c.y}, h, h, 12, mat);
    } else if (shape == 2) {
        s.loft(lift(rectOutline(0.55f * h, 0.55f * h), y0, c), lift(rectOutline(h, h), y1, c), mat);
    } else {
        s.slab(rectOutline(h, h), y0, y1, mat, c);
    }
}

std::vector<std::string> legLabels() { return {"Square", "Round", "Tapered"}; }

// The two rows every piece ends with, and what they do.
template <class T>
void placeRows(std::vector<Property>& v) {
    v.push_back(vector3<T>("Position", "at", &T::at, 0.1f, "%.2f"));
    v.push_back(number<T>("Turn (deg)", "turn", &T::turn, 15.0f, -360.0f, 360.0f, "%.0f"));
}

template <class T>
Property only(Property p, bool (*when)(const T&)) {
    p.visible = [when](const void* o) { return when(*static_cast<const T*>(o)); };
    return p;
}

// The common half of a piece: where it stands, and the cook that puts it there.
template <class T>
class Piece : public proc::NodeOf<T> {
public:
    glm::vec3 at{0.0f};
    float     turn = 0.0f;

    std::string cook(const std::vector<const Geo*>&, Geo& out) const override {
        Shop s{out.mesh, {}};
        static_cast<const T*>(this)->build(s);
        if (out.mesh.faces.empty()) return "Nothing to build at these sizes";
        transformGeo(out, glm::translate(glm::mat4(1.0f), at) *
                              glm::rotate(glm::mat4(1.0f), glm::radians(turn), kUp));
        return "";
    }
};

// ================================================================================
// Table
// ================================================================================

// Dining, coffee, side or desk: a top in one of four shapes on legs, trestles,
// a pedestal, steel sled frames or slab ends ("waterfall").
class TableNode : public Piece<TableNode> {
public:
    int   shape      = 0;      // 0 rectangle, 1 rounded corners, 2 round, 3 oval
    float width      = 1.60f;
    float depth      = 0.90f;
    float height     = 0.75f;
    float top        = 0.03f;
    float corner     = 0.10f;
    int   base       = 0;      // 0 four legs, 1 trestles, 2 pedestal, 3 sled frames, 4 slab ends
    int   legShape   = 2;
    float legSize    = 0.06f;
    float inset      = 0.05f;
    bool  apron      = true;
    float apronDrop  = 0.08f;
    bool  stretchers = false;
    bool  shelf      = false;
    std::string matTop, matBase;

    const char* typeId() const override { return "table"; }
    const char* displayName() const override { return "Table"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = TableNode;
            std::vector<Property> v;
            v.push_back(choice("Top", "shape", &S::shape, {"Rectangle", "Rounded corners", "Round", "Oval"}));
            v.push_back(number("Width", "width", &S::width, 0.05f, 0.30f, 6.0f));
            v.push_back(only<S>(number("Depth", "depth", &S::depth, 0.05f, 0.30f, 3.0f),
                                [](const S& s) { return s.shape != 2; }));
            v.push_back(number("Height", "height", &S::height, 0.01f, 0.20f, 1.30f));
            v.push_back(number("Top thickness", "top", &S::top, 0.005f, 0.01f, 0.12f, "%.3f"));
            v.push_back(only<S>(number("Corner radius", "corner", &S::corner, 0.01f, 0.0f, 1.0f),
                                [](const S& s) { return s.shape == 1; }));
            v.push_back(choice("Base", "base", &S::base,
                               {"Four legs", "Trestles", "Pedestal", "Sled frames", "Slab ends"}));
            v.push_back(only<S>(choice("Legs", "legShape", &S::legShape, legLabels()),
                                [](const S& s) { return s.base == 0; }));
            v.push_back(number("Leg size", "legSize", &S::legSize, 0.005f, 0.02f, 0.20f, "%.3f"));
            v.push_back(number("Legs in from the edge", "inset", &S::inset, 0.01f, 0.0f, 1.0f));
            v.push_back(only<S>(flag("Apron", "apron", &S::apron),
                                [](const S& s) { return s.base == 0 || s.base == 3; }));
            v.push_back(only<S>(number("Apron drop", "apronDrop", &S::apronDrop, 0.01f, 0.02f, 0.30f),
                                [](const S& s) { return s.apron && (s.base == 0 || s.base == 3); }));
            v.push_back(only<S>(flag("Stretchers", "stretchers", &S::stretchers),
                                [](const S& s) { return s.base == 0; }));
            v.push_back(only<S>(flag("Shelf below", "shelf", &S::shelf),
                                [](const S& s) { return s.base == 0; }));
            v.push_back(field("Top material", "materialTop", PropKind::Text, &S::matTop));
            v.push_back(field("Base material", "materialBase", PropKind::Text, &S::matBase));
            placeRows<S>(v);
            return v;
        }();
        return p;
    }

    void build(Shop& s) const {
        const AssetId mt = slot(matTop), mb = slot(matBase);
        const int   sh = std::clamp(shape, 0, 3);
        const float H  = clampf(height, 0.2f, 1.3f);
        const float T  = clampf(top, 0.01f, std::min(0.12f, 0.5f * H));
        const float hw = 0.5f * clampf(width, 0.3f, 6.0f);
        const float hd = sh == 2 ? hw : 0.5f * clampf(depth, 0.3f, 3.0f);
        const float rc = std::min(clampf(corner, 0.0f, 1.0f), std::min(hw, hd));
        const float L  = clampf(legSize, 0.02f, 0.2f);
        const bool  curved = sh >= 2;
        Outline o = sh == 0 ? rectOutline(hw, hd)
                  : sh == 1 ? roundRectOutline(hw, hd, rc)
                            : ellipseOutline(hw, hd, 48);
        s.slab(o, H - T, H, mt);
        const float yu = H - T;   // the underside of the top

        // Where legs stand: in from the edge, on the diagonals of a round top.
        float ix = hw - inset - 0.5f * L, iz = hd - inset - 0.5f * L;
        if (curved) { ix *= 0.7071f; iz *= 0.7071f; }
        if (sh == 1) { ix -= 0.29f * rc; iz -= 0.29f * rc; }
        ix = std::max(ix, L);
        iz = std::max(iz, L);

        auto aprons = [&](float x, float z) {
            if (!apron) return;
            const float t = 0.022f, drop = clampf(apronDrop, 0.02f, std::min(0.3f, 0.6f * yu));
            const float zo = z + 0.5f * L - 0.012f - 0.5f * t, xo = x + 0.5f * L - 0.012f - 0.5f * t;
            for (int sg = -1; sg <= 1; sg += 2) {
                s.block({-x, yu - drop, sg * zo - 0.5f * t}, {x, yu, sg * zo + 0.5f * t}, mb);
                s.block({sg * xo - 0.5f * t, yu - drop, -z}, {sg * xo + 0.5f * t, yu, z}, mb);
            }
        };

        switch (std::clamp(base, 0, 4)) {
        case 0: {
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2) leg(s, legShape, {sx * ix, sz * iz}, 0.0f, yu, L, mb);
            aprons(ix, iz);
            if (stretchers) {
                const float y = std::min(0.15f, 0.3f * yu);
                for (int sx = -1; sx <= 1; sx += 2)
                    s.beam({sx * ix, y, -iz}, {sx * ix, y, iz}, 0.5f * L, 0.7f * L, kUp, mb);
                s.beam({-ix, y + 0.04f, 0.0f}, {ix, y + 0.04f, 0.0f}, 0.5f * L, 0.7f * L, kUp, mb);
            }
            if (shelf) {
                const float y = std::min(0.14f, 0.3f * yu);
                s.block({-ix, y, -iz}, {ix, y + std::max(0.8f * T, 0.015f), iz}, mt);
            }
            break;
        }
        case 1: {   // trestles: a foot, a post and a cleat at each end, a stretcher between
            const float px = curved ? 0.55f * hw : hw - std::max(inset, 0.06f) - 0.06f;
            const float reach = curved ? 0.7f * hd : hd - inset - 0.02f;
            const float tx = std::max(L, 0.05f), tz = std::max(2.2f * L, 0.12f);
            for (int sx = -1; sx <= 1; sx += 2) {
                const float x = sx * px;
                s.block({x - 0.5f * tx - 0.005f, 0.0f, -reach}, {x + 0.5f * tx + 0.005f, 0.06f, reach}, mb);
                s.block({x - 0.5f * tx, 0.06f, -0.5f * tz}, {x + 0.5f * tx, yu - 0.05f, 0.5f * tz}, mb);
                s.block({x - 0.5f * tx, yu - 0.05f, -(reach - 0.06f)}, {x + 0.5f * tx, yu, reach - 0.06f}, mb);
            }
            const float y = 0.3f * yu;
            s.block({-px, y, -0.03f}, {px, y + 0.08f, 0.03f}, mb);
            break;
        }
        case 2: {   // pedestal: a column on a foot; a long table gets two
            const float r = std::max(L, 0.035f);
            const bool two = !curved && hw > 1.4f * hd && hw > 0.8f;
            const float foot = std::min(hw, hd) * (two ? 0.75f : 0.6f);
            for (int k = 0; k < (two ? 2 : 1); ++k) {
                const float x = two ? (k ? 0.5f : -0.5f) * hw : 0.0f;
                s.rod({x, 0.03f, 0.0f}, {x, yu - 0.02f, 0.0f}, r, r, 20, mb);
                s.slab(ellipseOutline(0.6f * foot, 0.6f * foot, 32), yu - 0.02f, yu, mb, {x, 0.0f});
                if (curved || !two) {
                    s.slab(ellipseOutline(foot, foot, 40), 0.0f, 0.035f, mb, {x, 0.0f});
                } else {
                    s.block({x - 0.06f, 0.0f, -foot}, {x + 0.06f, 0.045f, foot}, mb);
                }
            }
            if (two) s.block({-0.5f * hw, 0.25f * yu, -0.025f}, {0.5f * hw, 0.25f * yu + 0.06f, 0.025f}, mb);
            break;
        }
        case 3: {   // sled frames: a closed steel loop under each end
            const float r = std::max(0.35f * L, 0.01f);
            float fx = hw - inset - r, fz = hd - inset - r;
            if (curved) { fx *= 0.7071f; fz *= 0.7071f; }
            fx = std::max(fx, 2.0f * r);
            fz = std::max(fz, 2.0f * r);
            for (int sx = -1; sx <= 1; sx += 2) {
                const float x = sx * fx;
                s.pipe({{x, r, -fz}, {x, r, fz}, {x, yu - r, fz}, {x, yu - r, -fz}}, r, true, 3.0f * r, mb);
            }
            aprons(fx, fz);
            break;
        }
        default: {   // slab ends: the top turned down at both ends
            const float pt = std::max(T, 0.03f);
            const float x  = curved ? 0.55f * hw : hw;
            const float z  = curved ? 0.6f * hd : hd - (sh == 1 ? rc : 0.0f);
            for (int sx = -1; sx <= 1; sx += 2)
                s.block({sx * x, 0.0f, -z}, {sx * (x - pt), yu, z}, curved ? mb : mt);
            break;
        }
        }
    }
};

// ================================================================================
// Chair
// ================================================================================

// A dining chair, a stool, a bar stool, a cantilever, an office chair: a seat
// on a base, a back of slats, rails, a board or a cushion, arms if asked.
class ChairNode : public Piece<ChairNode> {
public:
    int   base       = 0;      // 0 four legs, 1 cantilever, 2 swivel, 3 sled
    float seatWidth  = 0.46f;
    float seatDepth  = 0.46f;
    float seatHeight = 0.46f;
    float seat       = 0.03f;
    bool  roundSeat  = false;
    bool  cushion    = false;
    int   back       = 1;      // 0 none, 1 slats, 2 rails, 3 board, 4 upholstered
    float backHeight = 0.44f;
    float backTilt   = 8.0f;
    int   slats      = 3;
    bool  arms       = false;
    float armHeight  = 0.22f;
    int   legShape   = 0;
    float legSize    = 0.035f;
    bool  stretchers = false;
    std::string matFrame, matSeat;

    const char* typeId() const override { return "chair"; }
    const char* displayName() const override { return "Chair"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = ChairNode;
            std::vector<Property> v;
            v.push_back(choice("Base", "base", &S::base, {"Four legs", "Cantilever", "Swivel (office)", "Sled"}));
            v.push_back(number("Seat width", "seatWidth", &S::seatWidth, 0.01f, 0.25f, 1.2f));
            v.push_back(only<S>(number("Seat depth", "seatDepth", &S::seatDepth, 0.01f, 0.25f, 1.0f),
                                [](const S& s) { return !s.roundSeat; }));
            v.push_back(number("Seat height", "seatHeight", &S::seatHeight, 0.01f, 0.20f, 1.0f));
            v.push_back(number("Seat thickness", "seat", &S::seat, 0.005f, 0.01f, 0.12f, "%.3f"));
            v.push_back(only<S>(flag("Round seat", "roundSeat", &S::roundSeat),
                                [](const S& s) { return s.base == 0 && s.back == 0; }));
            v.push_back(flag("Cushion", "cushion", &S::cushion));
            v.push_back(choice("Back", "back", &S::back, {"None (a stool)", "Slats", "Rails", "Board", "Upholstered"}));
            auto backed = [](const S& s) { return s.back != 0; };
            v.push_back(only<S>(number("Back height", "backHeight", &S::backHeight, 0.01f, 0.1f, 1.2f), backed));
            v.push_back(only<S>(number("Back tilt (deg)", "backTilt", &S::backTilt, 1.0f, -5.0f, 30.0f, "%.0f"), backed));
            v.push_back(only<S>(whole("Slats", "slats", &S::slats, 1, 12),
                                [](const S& s) { return s.back == 1 || s.back == 2; }));
            v.push_back(flag("Arms", "arms", &S::arms));
            v.push_back(only<S>(number("Arm height", "armHeight", &S::armHeight, 0.01f, 0.1f, 0.5f),
                                [](const S& s) { return s.arms; }));
            v.push_back(only<S>(choice("Legs", "legShape", &S::legShape, legLabels()),
                                [](const S& s) { return s.base == 0; }));
            v.push_back(number("Leg / tube size", "legSize", &S::legSize, 0.005f, 0.015f, 0.1f, "%.3f"));
            v.push_back(only<S>(flag("Stretchers / foot rest", "stretchers", &S::stretchers),
                                [](const S& s) { return s.base == 0; }));
            v.push_back(field("Frame material", "materialFrame", PropKind::Text, &S::matFrame));
            v.push_back(field("Seat material", "materialSeat", PropKind::Text, &S::matSeat));
            placeRows<S>(v);
            return v;
        }();
        return p;
    }

    void build(Shop& s) const {
        const AssetId mf = slot(matFrame), ms = slot(matSeat);
        const int   bs = std::clamp(base, 0, 3);
        const int   bk = std::clamp(back, 0, 4);
        const float SH = clampf(seatHeight, 0.2f, 1.0f);
        const float hw = 0.5f * clampf(seatWidth, 0.25f, 1.2f);
        const bool  round = roundSeat && bs == 0 && bk == 0;
        const float hd = round ? hw : 0.5f * clampf(seatDepth, 0.25f, 1.0f);
        const float ST = clampf(seat, 0.01f, 0.12f);
        const float L  = clampf(legSize, 0.015f, 0.1f);
        const bool  soft = cushion || bs == 2;
        const float ct = soft ? std::min(0.06f, 0.25f * SH) : 0.0f;
        const float yb = SH - ct;        // top of the seat board
        const float ys = yb - ST;        // its underside
        const float yTop = SH + clampf(backHeight, 0.1f, 1.2f);
        const float tilt = -clampf(backTilt, -5.0f, 30.0f);
        const float tube = std::max(0.35f * L, 0.011f);

        // The seat.
        const Outline so = round ? ellipseOutline(hw, hw, 40) : roundRectOutline(hw, hd, 0.02f, 3);
        s.slab(so, ys, yb, ms);
        if (soft) {
            const float in = 0.01f;
            if (round) s.slab(ellipseOutline(hw - in, hw - in, 40), yb, SH, ms);
            else s.cushion({-hw + in, yb - 0.01f, -hd + in}, {hw - in, SH, hd - in}, 0.45f * ct, ms);
        }

        // The back plane and the posts that carry it.
        const bool tubes = bs == 1 || bs == 3;
        const float px = tubes ? hw - tube : hw - 0.5f * L - 0.01f;
        const float z0 = tubes ? -hd + tube + 0.01f : -hd + 0.5f * L + 0.01f;
        auto tilted = [&](glm::vec3 p) { return glm::vec3(tiltAbout(tilt, ys, z0) * glm::vec4(p, 1.0f)); };

        // --- Base -----------------------------------------------------------
        if (bs == 0) {
            float lx = hw - 0.5f * L - 0.01f, lz = hd - 0.5f * L - 0.01f;
            if (round) lx = lz = (hw - 0.5f * L - 0.02f) * 0.7071f;
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2) leg(s, legShape, {sx * lx, sz * lz}, 0.0f, ys, L, mf);
            if (stretchers) {
                const float y = SH < 0.6f ? 0.15f : 0.42f * SH;
                const float w = 0.55f * L;
                for (int sg = -1; sg <= 1; sg += 2) {
                    s.beam({-lx, y, sg * lz}, {lx, y, sg * lz}, w, w, kUp, mf);
                    s.beam({sg * lx, y + 0.03f, -lz}, {sg * lx, y + 0.03f, lz}, w, w, kUp, mf);
                }
            }
        } else if (bs == 1) {
            // One tube: up the back, along the seat, down at the front, back
            // along the floor, across, and the same on the other side.
            const float zf = hd - 0.03f, zb = z0, yseat = ys - tube;
            std::vector<glm::vec3> path;
            for (int side = 0; side < 2; ++side) {
                const float x = side == 0 ? -px : px;
                std::vector<glm::vec3> half;
                if (bk != 0) half.push_back(tilted({x, yTop - 0.03f, zb}));
                half.push_back({x, yseat, zb});
                half.push_back({x, yseat, zf});
                half.push_back({x, tube, zf + 0.06f});
                half.push_back({x, tube, zb - 0.06f});
                if (side == 1) std::reverse(half.begin(), half.end());
                path.insert(path.end(), half.begin(), half.end());
            }
            s.pipe(path, tube, false, 0.07f, mf);
        } else if (bs == 2) {
            const float yh = 0.07f;
            s.rod({0.0f, 0.045f, 0.0f}, {0.0f, yh + 0.03f, 0.0f}, 0.045f, 0.04f, 16, mf);
            s.rod({0.0f, yh + 0.03f, 0.0f}, {0.0f, ys - 0.04f, 0.0f}, 0.026f, 0.026f, 14, mf);
            s.block({-0.12f, ys - 0.04f, -0.12f}, {0.12f, ys, 0.12f}, mf);
            for (int k = 0; k < 5; ++k) {
                const float a = glm::radians(90.0f + 72.0f * static_cast<float>(k));
                const glm::vec3 d(std::cos(a), 0.0f, std::sin(a));
                const float R = std::max(0.30f, 0.65f * hw + 0.12f);
                s.beam(glm::vec3(0.0f, yh, 0.0f), d * R + glm::vec3(0.0f, yh - 0.01f, 0.0f), 0.045f, 0.03f, kUp, mf);
                const glm::vec3 w = d * R;
                s.rod({w.x, 0.0f, w.z}, {w.x, yh - 0.02f, w.z}, 0.024f, 0.024f, 10, mf);
            }
        } else {
            const float zf = hd - 0.03f, zb = z0, yseat = ys - tube;
            for (int sx = -1; sx <= 1; sx += 2) {
                const float x = sx * px;
                s.pipe({{x, tube, zb}, {x, tube, zf}, {x, yseat, zf}, {x, yseat, zb}}, tube, true, 0.06f, mf);
            }
        }

        // --- Back --------------------------------------------------------------
        if (bk != 0) {
            const std::size_t from = s.mark();
            const float yB = SH + 0.05f;               // lowest part of the back
            if (bs == 0) {
                for (int sx = -1; sx <= 1; sx += 2) leg(s, legShape == 2 ? 0 : legShape, {sx * px, z0}, ys, yTop, L, mf);
            } else if (bs == 3) {
                for (int sx = -1; sx <= 1; sx += 2)
                    s.pipe({{sx * px, ys - tube, z0}, {sx * px, yTop - 0.03f, z0}}, tube, false, 0.0f, mf);
            } else if (bs == 2) {
                s.beam({0.0f, ys - 0.02f, -hd - 0.03f}, {0.0f, SH + 0.25f * (yTop - SH), -hd - 0.03f}, 0.06f, 0.025f, kZ, mf);
            }
            const float inner = bs == 2 ? hw * 0.95f : px - (tubes ? tube : 0.5f * L);
            const int style = bs == 2 ? 4 : bk;
            const float zc = bs == 2 ? -hd - 0.01f : z0;
            switch (style) {
            case 1: {
                s.beam({-px, yTop - 0.04f, zc}, {px, yTop - 0.04f, zc}, 0.022f, 0.07f, kUp, mf);
                s.beam({-px, yB + 0.02f, zc}, {px, yB + 0.02f, zc}, 0.02f, 0.04f, kUp, mf);
                const int n = std::clamp(slats, 1, 12);
                const float iw = 2.0f * inner;
                const float w = std::min(0.07f, iw / (static_cast<float>(n) * 1.6f));
                for (int i = 0; i < n; ++i) {
                    const float x = -0.5f * iw + iw * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
                    s.beam({x, yB + 0.03f, zc}, {x, yTop - 0.06f, zc}, w, 0.015f, kZ, mf);
                }
                break;
            }
            case 2: {
                const int n = std::clamp(slats, 1, 12);
                for (int i = 0; i < n; ++i) {
                    const float t = n == 1 ? 1.0f : static_cast<float>(i) / static_cast<float>(n - 1);
                    const float y = yB + 0.03f + t * (yTop - yB - 0.07f);
                    s.beam({-px, y, zc}, {px, y, zc}, 0.02f, 0.05f, kUp, mf);
                }
                break;
            }
            case 3:
                s.beam({-px, 0.5f * (yB + yTop), zc}, {px, 0.5f * (yB + yTop), zc}, 0.018f, yTop - yB, kUp, mf);
                break;
            default: {
                const float x = bs == 2 ? inner : px + (tubes ? tube : 0.5f * L);
                const float y0 = bs == 2 ? SH + 0.08f : yB;
                s.cushion({-x, y0, zc - 0.03f}, {x, yTop, zc + 0.04f}, 0.025f, ms, 2);
                break;
            }
            }
            if (bs != 1) s.xform(from, tiltAbout(tilt, ys, z0));
        }

        // --- Arms ----------------------------------------------------------------
        if (arms) {
            const float ya = SH + clampf(armHeight, 0.1f, 0.5f);
            for (int sx = -1; sx <= 1; sx += 2) {
                if (bs == 2) {
                    const float x = sx * (hw + 0.02f);
                    s.beam({x, ys - 0.02f, 0.0f}, {x, ya - 0.02f, 0.0f}, 0.04f, 0.02f, kZ, mf);
                    s.cushion({x - 0.035f, ya - 0.03f, -0.16f}, {x + 0.035f, ya, 0.12f}, 0.012f, ms, 2);
                    continue;
                }
                const float x = sx * px;
                const float zf = hd - 0.05f;
                if (tubes) s.pipe({{x, ys - tube, zf}, {x, ya - tube, zf}, {x, ya - tube, z0}}, tube, false, 0.05f, mf);
                else leg(s, legShape, {x, zf}, ys, ya - 0.02f, 0.85f * L, mf);
                s.beam({x, ya - 0.0125f, z0}, {x, ya - 0.0125f, zf + 0.03f}, std::max(1.3f * L, 0.045f), 0.025f, kUp, mf);
            }
        }
    }
};

// ================================================================================
// Sofa
// ================================================================================

// From an armchair (one seat) to a three-seater with a chaise: a base, a back,
// arms, seat and back cushions and a pair of throw pillows.
class SofaNode : public Piece<SofaNode> {
public:
    float width       = 2.10f;
    float depth       = 0.92f;
    float height      = 0.82f;
    float seatHeight  = 0.44f;
    int   seats       = 3;
    int   arms        = 1;     // 0 none, 1 square, 2 rolled, 3 slim
    float armWidth    = 0.16f;
    float armHeight   = 0.62f;
    int   chaise      = 0;     // 0 none, 1 left, 2 right
    float chaiseDepth = 1.55f;
    bool  backCushions = true;
    int   pillows     = 2;
    int   base        = 0;     // 0 legs, 1 plinth
    float legHeight   = 0.10f;
    float softness    = 0.05f;
    std::string matFabric, matCushion, matLegs;

    const char* typeId() const override { return "sofa"; }
    const char* displayName() const override { return "Sofa"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = SofaNode;
            std::vector<Property> v;
            v.push_back(number("Width", "width", &S::width, 0.05f, 0.6f, 6.0f));
            v.push_back(number("Depth", "depth", &S::depth, 0.02f, 0.5f, 1.6f));
            v.push_back(number("Height", "height", &S::height, 0.02f, 0.5f, 1.4f));
            v.push_back(number("Seat height", "seatHeight", &S::seatHeight, 0.01f, 0.25f, 0.7f));
            v.push_back(whole("Seats", "seats", &S::seats, 1, 6));
            v.push_back(choice("Arms", "arms", &S::arms, {"None", "Square", "Rolled", "Slim"}));
            auto armed = [](const S& s) { return s.arms != 0; };
            v.push_back(only<S>(number("Arm width", "armWidth", &S::armWidth, 0.01f, 0.04f, 0.4f), armed));
            v.push_back(only<S>(number("Arm height", "armHeight", &S::armHeight, 0.01f, 0.3f, 1.2f), armed));
            v.push_back(choice("Chaise", "chaise", &S::chaise, {"None", "Left", "Right"}));
            v.push_back(only<S>(number("Chaise length", "chaiseDepth", &S::chaiseDepth, 0.05f, 1.0f, 2.5f),
                                [](const S& s) { return s.chaise != 0; }));
            v.push_back(flag("Back cushions", "backCushions", &S::backCushions));
            v.push_back(whole("Throw pillows", "pillows", &S::pillows, 0, 6));
            v.push_back(choice("Base", "base", &S::base, {"Legs", "Plinth"}));
            v.push_back(only<S>(number("Leg height", "legHeight", &S::legHeight, 0.01f, 0.02f, 0.3f),
                                [](const S& s) { return s.base == 0; }));
            v.push_back(number("Softness", "softness", &S::softness, 0.005f, 0.0f, 0.12f, "%.3f"));
            v.push_back(field("Fabric", "materialFabric", PropKind::Text, &S::matFabric));
            v.push_back(field("Cushions", "materialCushion", PropKind::Text, &S::matCushion));
            v.push_back(field("Legs", "materialLegs", PropKind::Text, &S::matLegs));
            placeRows<S>(v);
            return v;
        }();
        return p;
    }

    void build(Shop& s) const {
        const AssetId mf = slot(matFabric), mc = slot(matCushion), ml = slot(matLegs);
        const float H  = clampf(height, 0.5f, 1.4f);
        const float SH = clampf(seatHeight, 0.25f, std::min(0.7f, H - 0.1f));
        const float hw = 0.5f * clampf(width, 0.6f, 6.0f);
        const float hd = 0.5f * clampf(depth, 0.5f, 1.6f);
        const int   ar = std::clamp(arms, 0, 3);
        const int   ch = std::clamp(chaise, 0, 2);
        const float aw = ar == 0 ? 0.0f : ar == 3 ? 0.06f : clampf(armWidth, 0.04f, std::min(0.4f, 0.3f * hw));
        const float AH = clampf(armHeight, SH + 0.05f, H);
        const float rd = clampf(softness, 0.0f, 0.12f);
        const float yLeg = base == 0 ? clampf(legHeight, 0.02f, 0.3f) : 0.04f;
        const float ct = std::min(0.16f, 0.55f * (SH - yLeg));
        const float yF = SH - ct;
        const float backT = std::min(0.20f, 0.22f * 2.0f * hd) + (backCushions ? 0.0f : 0.08f);
        const bool armL = ar != 0 && ch != 1, armR = ar != 0 && ch != 2;
        const float x0 = -hw + (armL ? aw : 0.0f), x1 = hw - (armR ? aw : 0.0f);
        const float zFront = hd;
        const float zChaise = -hd + std::max(clampf(chaiseDepth, 1.0f, 2.5f), 2.0f * hd);
        const int   n = std::clamp(seats, 1, 6);
        const float cw = ch ? std::min(x1 - x0, std::max(0.75f, (x1 - x0) / static_cast<float>(n))) : 0.0f;
        const float cx0 = ch == 1 ? x0 : x1 - cw, cx1 = ch == 1 ? x0 + cw : x1;

        // The base and the back, all fabric.
        s.cushion({x0, yLeg, -hd}, {x1, yF, zFront - 0.02f}, std::min(rd, 0.03f), mf, 2);
        if (ch) s.cushion({cx0, yLeg, -hd}, {cx1, yF, zChaise - 0.02f}, std::min(rd, 0.03f), mf, 2);
        s.cushion({x0 - (armL ? 0.02f : 0.0f), yF - 0.02f, -hd}, {x1 + (armR ? 0.02f : 0.0f), H, -hd + backT},
                  std::min(rd + 0.01f, 0.5f * backT), mf);
        for (int side = 0; side < 2; ++side) {
            if (!(side == 0 ? armL : armR)) continue;
            const float xa = side == 0 ? -hw : hw - aw, xb = side == 0 ? -hw + aw : hw;
            const float r = ar == 2 ? 0.48f * aw : ar == 3 ? 0.02f : std::min(rd + 0.01f, 0.4f * aw);
            s.cushion({xa, yLeg, -hd}, {xb, AH, hd}, r, mf);
        }

        // Seat cushions: one per seat across, the chaise's running forward.
        const float zs0 = -hd + backT + (backCushions ? 0.16f : 0.0f) - 0.03f;
        const float gap = 0.006f;
        struct Span { float a, b; bool chaise; };
        std::vector<Span> spans;
        {
            const int rest = ch ? n - 1 : n;
            const float ra = ch == 1 ? cx1 : x0, rb = ch == 2 ? cx0 : x1;
            if (ch == 1) spans.push_back({cx0, cx1, true});
            for (int i = 0; i < rest && rb - ra > 0.1f; ++i) {
                const float w = (rb - ra) / static_cast<float>(rest);
                spans.push_back({ra + w * static_cast<float>(i), ra + w * static_cast<float>(i + 1), false});
            }
            if (ch == 2) spans.push_back({cx0, cx1, true});
            if (ch && n == 1) spans = {{x0, x1, true}};
        }
        for (const Span& sp : spans) {
            const float zEnd = sp.chaise ? zChaise : zFront + 0.01f;
            s.cushion({sp.a + gap, yF - 0.01f, zs0}, {sp.b - gap, SH, zEnd}, rd, mc);
        }
        // Back cushions, leaning back a little.
        if (backCushions)
            for (const Span& sp : spans) {
                const std::size_t from = s.mark();
                const float zb = -hd + backT - 0.03f;
                s.cushion({sp.a + gap, SH - 0.02f, zb}, {sp.b - gap, H - 0.04f, zb + 0.19f}, rd + 0.01f, mc);
                s.xform(from, tiltAbout(-10.0f, SH, zb));
            }
        // Throw pillows, in the corners, tipped back and turned a little.
        const float zp = -hd + backT + (backCushions ? 0.12f : 0.0f);
        const float ps = std::min(0.45f, 0.8f * (H - SH));
        for (int k = 0; k < std::clamp(pillows, 0, 6); ++k) {
            const bool left = (k % 2 == 0);
            const float off = 0.04f + static_cast<float>(k / 2) * 0.32f;
            const float xa = left ? x0 + off : x1 - off - ps;
            if (xa < x0 || xa + ps > x1) continue;
            const std::size_t from = s.mark();
            s.cushion({xa, SH - 0.02f, zp}, {xa + ps, SH - 0.02f + ps, zp + 0.14f}, 0.06f, mc);
            const glm::vec3 c(xa + 0.5f * ps, SH, zp);
            s.xform(from, glm::translate(glm::mat4(1.0f), c) *
                              glm::rotate(glm::mat4(1.0f), glm::radians(left ? 12.0f : -12.0f), kUp) *
                              glm::rotate(glm::mat4(1.0f), glm::radians(-18.0f), glm::vec3(1, 0, 0)) *
                              glm::translate(glm::mat4(1.0f), -c));
        }

        // Legs, or a dark plinth.
        if (base == 0) {
            std::vector<glm::vec2> at = {{-hw + 0.06f, -hd + 0.06f}, {hw - 0.06f, -hd + 0.06f},
                                         {-hw + 0.06f, hd - 0.06f}, {hw - 0.06f, hd - 0.06f}};
            if (ch) {
                at.push_back({cx0 + 0.06f, zChaise - 0.08f});
                at.push_back({cx1 - 0.06f, zChaise - 0.08f});
            }
            if (hw > 1.2f) { at.push_back({0.0f, -hd + 0.06f}); at.push_back({0.0f, hd - 0.06f}); }
            for (const glm::vec2& p : at) leg(s, 2, p, 0.0f, yLeg + 0.01f, 0.05f, ml);
        } else {
            s.block({-hw + 0.03f, 0.0f, -hd + 0.03f}, {hw - 0.03f, yLeg + 0.01f, hd - 0.05f}, ml);
            if (ch) s.block({cx0 + 0.03f, 0.0f, 0.0f}, {cx1 - 0.03f, yLeg + 0.01f, zChaise - 0.05f}, ml);
        }
    }
};

// ================================================================================
// Bed
// ================================================================================

// A single to a king: a frame (on legs, a platform, a box spring, four
// posts), a mattress, a duvet, pillows, a headboard and a footboard. The head
// is at the back (-Z): the foot is the front, where you get in.
class BedNode : public Piece<BedNode> {
public:
    float width       = 1.60f;   // mattress
    float length      = 2.00f;
    float mattress    = 0.20f;
    int   frame       = 0;       // 0 on legs, 1 platform, 2 box spring, 3 four-poster
    float frameHeight = 0.28f;   // under the mattress
    float rim         = 0.04f;
    int   headboard   = 2;       // 0 none, 1 panel, 2 upholstered, 3 slats
    float headHeight  = 1.00f;
    bool  footboard   = false;
    float footHeight  = 0.50f;
    int   bedding     = 1;       // 0 bare, 1 duvet, 2 duvet turned back
    int   pillows     = 2;
    std::string matFrame, matHead, matMattress, matBedding;

    const char* typeId() const override { return "bed"; }
    const char* displayName() const override { return "Bed"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = BedNode;
            std::vector<Property> v;
            v.push_back(number("Mattress width", "width", &S::width, 0.1f, 0.6f, 2.4f));
            v.push_back(number("Mattress length", "length", &S::length, 0.1f, 1.4f, 2.4f));
            v.push_back(number("Mattress height", "mattress", &S::mattress, 0.01f, 0.06f, 0.4f));
            v.push_back(choice("Frame", "frame", &S::frame, {"On legs", "Platform", "Box spring", "Four-poster"}));
            v.push_back(number("Frame height", "frameHeight", &S::frameHeight, 0.01f, 0.08f, 0.8f));
            v.push_back(only<S>(number("Frame rim", "rim", &S::rim, 0.005f, 0.01f, 0.2f, "%.3f"),
                                [](const S& s) { return s.frame != 2; }));
            v.push_back(choice("Headboard", "headboard", &S::headboard, {"None", "Panel", "Upholstered", "Slats"}));
            v.push_back(only<S>(number("Headboard height", "headHeight", &S::headHeight, 0.02f, 0.4f, 2.0f),
                                [](const S& s) { return s.headboard != 0; }));
            v.push_back(flag("Footboard", "footboard", &S::footboard));
            v.push_back(only<S>(number("Footboard height", "footHeight", &S::footHeight, 0.02f, 0.2f, 1.2f),
                                [](const S& s) { return s.footboard; }));
            v.push_back(choice("Bedding", "bedding", &S::bedding, {"Bare mattress", "Duvet", "Duvet turned back"}));
            v.push_back(whole("Pillows", "pillows", &S::pillows, 0, 4));
            v.push_back(field("Frame material", "materialFrame", PropKind::Text, &S::matFrame));
            v.push_back(field("Headboard material", "materialHeadboard", PropKind::Text, &S::matHead));
            v.push_back(field("Mattress material", "materialMattress", PropKind::Text, &S::matMattress));
            v.push_back(field("Bedding material", "materialBedding", PropKind::Text, &S::matBedding));
            placeRows<S>(v);
            return v;
        }();
        return p;
    }

    void build(Shop& s) const {
        const AssetId mf = slot(matFrame), mh = slot(matHead), mm = slot(matMattress), mb = slot(matBedding);
        const int   fr = std::clamp(frame, 0, 3);
        const float MW = clampf(width, 0.6f, 2.4f), ML = clampf(length, 1.4f, 2.4f);
        const float MT = clampf(mattress, 0.06f, 0.4f);
        const float FH = clampf(frameHeight, 0.08f, 0.8f);
        const float rm = fr == 2 ? 0.01f : clampf(rim, 0.01f, 0.2f);
        const float hw = 0.5f * MW + rm, hl = 0.5f * ML + rm;
        const float yM1 = FH + MT;

        // --- Frame ----------------------------------------------------------
        if (fr == 0 || fr == 3) {
            const float rt = FH + 0.05f, rb = std::max(rt - 0.18f, 0.05f);
            s.block({-hw, rb, -hl}, {-hw + rm, rt, hl}, mf);
            s.block({hw - rm, rb, -hl}, {hw, rt, hl}, mf);
            s.block({-hw + rm, rb, -hl}, {hw - rm, rt, -hl + rm}, mf);
            s.block({-hw + rm, rb, hl - rm}, {hw - rm, rt, hl}, mf);
            s.block({-0.5f * MW, FH - 0.02f, -0.5f * ML}, {0.5f * MW, FH, 0.5f * ML}, mf);
            const float ls = std::max(0.06f, rm);
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2)
                    leg(s, 0, {sx * (hw - 0.5f * ls), sz * (hl - 0.5f * ls)}, 0.0f, rb, ls, mf);
            if (fr == 3) {
                const float top = std::max(2.0f, headHeight + 0.6f), ps = std::max(0.08f, rm);
                for (int sx = -1; sx <= 1; sx += 2)
                    for (int sz = -1; sz <= 1; sz += 2)
                        leg(s, 0, {sx * (hw - 0.5f * ps), sz * (hl - 0.5f * ps)}, 0.0f, top, ps, mf);
                const float y = top - 0.04f, x = hw - 0.5f * ps, z = hl - 0.5f * ps;
                for (int sg = -1; sg <= 1; sg += 2) {
                    s.beam({-x, y, sg * z}, {x, y, sg * z}, 0.05f, 0.07f, kUp, mf);
                    s.beam({sg * x, y, -z}, {sg * x, y, z}, 0.05f, 0.07f, kUp, mf);
                }
            }
        } else if (fr == 1) {
            s.block({-hw + 0.08f, 0.0f, -hl + 0.08f}, {hw - 0.08f, 0.08f, hl - 0.08f}, mf);
            s.block({-hw, 0.08f, -hl}, {hw, FH, hl}, mf);
        } else {
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2)
                    leg(s, 2, {sx * (hw - 0.08f), sz * (hl - 0.08f)}, 0.0f, 0.09f, 0.05f, mf);
            s.cushion({-hw, 0.08f, -hl}, {hw, FH, hl}, 0.02f, mh, 2);
        }

        // --- Mattress, duvet, pillows ---------------------------------------------
        s.cushion({-0.5f * MW, FH, -0.5f * ML}, {0.5f * MW, yM1, 0.5f * ML}, std::min(0.04f, 0.3f * MT), mm, 2);
        if (bedding != 0) {
            const float over = 0.035f;
            const float drop = std::min(0.14f, MT * 0.7f);
            const float z0 = -0.5f * ML + (bedding == 2 ? 0.80f : 0.55f);
            s.cushion({-0.5f * MW - over, yM1 - drop, z0}, {0.5f * MW + over, yM1 + 0.06f, 0.5f * ML + over},
                      0.04f, mb);
            if (bedding == 2)
                s.cushion({-0.5f * MW - over, yM1 - 0.02f, z0 - 0.14f}, {0.5f * MW + over, yM1 + 0.11f, z0 + 0.12f},
                          0.06f, mb);
        }
        const int np = std::clamp(pillows, 0, 4);
        if (np > 0) {
            const float g = 0.04f;
            const float pw = std::min(0.80f, (MW - 0.06f - g * static_cast<float>(np - 1)) / static_cast<float>(np));
            const float row = pw * static_cast<float>(np) + g * static_cast<float>(np - 1);
            const float zf = -0.5f * ML + 0.47f;
            for (int i = 0; i < np; ++i) {
                const float xa = -0.5f * row + static_cast<float>(i) * (pw + g);
                const std::size_t from = s.mark();
                s.cushion({xa, yM1, zf - 0.42f}, {xa + pw, yM1 + 0.14f, zf}, 0.06f, mb);
                s.xform(from, tiltAbout(20.0f, yM1, zf));
            }
        }

        // --- Headboard and footboard -----------------------------------------------
        const float HH = clampf(headHeight, std::max(0.4f, yM1 + 0.1f), 2.0f);
        switch (std::clamp(headboard, 0, 3)) {
        case 1:
            s.block({-hw, 0.0f, -hl - 0.04f}, {hw, HH, -hl}, mh);
            break;
        case 2:
            s.cushion({-hw - 0.02f, 0.6f * FH, -hl - 0.10f}, {hw + 0.02f, HH, -hl}, 0.04f, mh);
            break;
        case 3: {
            const float ps = 0.07f, z = -hl - 0.5f * ps;
            for (int sx = -1; sx <= 1; sx += 2) leg(s, 0, {sx * (hw - 0.5f * ps), z}, 0.0f, HH, ps, mh);
            s.beam({-hw + ps, HH - 0.05f, z}, {hw - ps, HH - 0.05f, z}, 0.04f, 0.08f, kUp, mh);
            const float yb = yM1 + 0.02f;
            s.beam({-hw + ps, yb, z}, {hw - ps, yb, z}, 0.04f, 0.06f, kUp, mh);
            const int n = std::max(2, static_cast<int>((2.0f * hw - 2.0f * ps) / 0.13f));
            for (int i = 0; i < n; ++i) {
                const float x = -hw + ps + (2.0f * hw - 2.0f * ps) * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
                s.beam({x, yb + 0.03f, z}, {x, HH - 0.09f, z}, 0.05f, 0.02f, kZ, mh);
            }
            break;
        }
        default: break;
        }
        if (footboard)
            s.block({-hw, 0.0f, hl}, {hw, clampf(footHeight, 0.2f, 1.2f), hl + 0.03f}, mh);
    }
};

// ================================================================================
// Cabinet
// ================================================================================

// The box with fronts every room has: sideboard, wardrobe, chest of drawers,
// nightstand, kitchen base and wall units, a TV board. A carcass cut into
// columns and rows; doors, drawers, glass or nothing in front; a plinth,
// legs or the wall under it; a worktop if it is a kitchen.
class CabinetNode : public Piece<CabinetNode> {
public:
    float width       = 1.60f;
    float height      = 0.80f;
    float depth       = 0.45f;
    float board       = 0.019f;
    int   columns     = 3;
    int   rows        = 1;
    int   fronts      = 0;     // 0 doors, 1 drawers, 2 open, 3 glass doors, 4 drawers over doors
    int   base        = 0;     // 0 plinth, 1 legs, 2 wall-hung
    float baseHeight  = 0.08f;
    bool  worktop     = false;
    float topThickness = 0.03f;
    float overhang    = 0.02f;
    int   handles     = 1;     // 0 none, 1 bar, 2 knob, 3 grip strip
    int   shelves     = 1;
    std::string matBody, matFront, matHandle, matGlass, matTop;

    const char* typeId() const override { return "cabinet"; }
    const char* displayName() const override { return "Cabinet"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = CabinetNode;
            std::vector<Property> v;
            v.push_back(number("Width", "width", &S::width, 0.05f, 0.2f, 6.0f));
            v.push_back(number("Height", "height", &S::height, 0.05f, 0.2f, 3.0f));
            v.push_back(number("Depth", "depth", &S::depth, 0.01f, 0.15f, 1.2f));
            v.push_back(number("Board", "board", &S::board, 0.001f, 0.008f, 0.06f, "%.3f"));
            v.push_back(whole("Columns", "columns", &S::columns, 1, 12));
            v.push_back(only<S>(whole("Rows", "rows", &S::rows, 1, 12), [](const S& s) { return s.fronts != 4; }));
            v.push_back(choice("Fronts", "fronts", &S::fronts,
                               {"Doors", "Drawers", "Open", "Glass doors", "Drawers over doors"}));
            v.push_back(only<S>(whole("Shelves inside", "shelves", &S::shelves, 0, 8),
                                [](const S& s) { return s.fronts == 2 || s.fronts == 3; }));
            v.push_back(choice("Base", "base", &S::base, {"Plinth", "Legs", "Wall-hung"}));
            v.push_back(number("Base height", "baseHeight", &S::baseHeight, 0.01f, 0.0f, 2.5f));
            v.push_back(flag("Worktop", "worktop", &S::worktop));
            auto top = [](const S& s) { return s.worktop; };
            v.push_back(only<S>(number("Worktop thickness", "topThickness", &S::topThickness, 0.005f, 0.01f, 0.1f, "%.3f"), top));
            v.push_back(only<S>(number("Worktop overhang", "overhang", &S::overhang, 0.005f, 0.0f, 0.3f, "%.3f"), top));
            v.push_back(only<S>(choice("Handles", "handles", &S::handles, {"None", "Bar", "Knob", "Grip strip"}),
                                [](const S& s) { return s.fronts != 2; }));
            v.push_back(field("Body material", "materialBody", PropKind::Text, &S::matBody));
            v.push_back(field("Front material", "materialFront", PropKind::Text, &S::matFront));
            v.push_back(field("Handle material", "materialHandle", PropKind::Text, &S::matHandle));
            v.push_back(field("Glass material", "materialGlass", PropKind::Text, &S::matGlass));
            v.push_back(only<S>(field("Worktop material", "materialTop", PropKind::Text, &S::matTop), top));
            placeRows<S>(v);
            return v;
        }();
        return p;
    }

    void build(Shop& s) const {
        const AssetId mbd = slot(matBody), mfr = slot(matFront), mhd = slot(matHandle), mgl = slot(matGlass),
                      mtp = slot(matTop);
        const float W  = clampf(width, 0.2f, 6.0f), D = clampf(depth, 0.15f, 1.2f);
        const float H  = clampf(height, 0.2f, 3.0f);
        const float b  = clampf(board, 0.008f, std::min(0.06f, 0.2f * D));
        const float hw = 0.5f * W, hd = 0.5f * D;
        const int   bs = std::clamp(base, 0, 2);
        const float wt = worktop ? clampf(topThickness, 0.01f, 0.1f) : 0.0f;
        const float y0 = clampf(baseHeight, 0.0f, H - wt - 0.1f);
        const float y1 = H - wt;
        const float ft = std::max(b, 0.016f);
        const float zc0 = -hd, zc1 = hd - ft;
        const float back = std::min(0.008f, 0.3f * b);
        const int   cols = std::clamp(columns, 1, 12);
        const int   fm = std::clamp(fronts, 0, 4);
        const int   rws = fm == 4 ? 2 : std::clamp(rows, 1, 12);
        const float gap = 0.003f;
        if (y1 - y0 < 2.5f * b) return;

        // Carcass.
        s.block({-hw, y0, zc0}, {-hw + b, y1, zc1}, mbd);
        s.block({hw - b, y0, zc0}, {hw, y1, zc1}, mbd);
        s.block({-hw + b, y1 - b, zc0}, {hw - b, y1, zc1}, mbd);
        s.block({-hw + b, y0, zc0}, {hw - b, y0 + b, zc1}, mbd);
        s.block({-hw + b, y0 + b, zc0}, {hw - b, y1 - b, zc0 + back}, mbd);
        const float cw = W / static_cast<float>(cols);
        for (int c = 1; c < cols; ++c) {
            const float x = -hw + cw * static_cast<float>(c);
            s.block({x - 0.5f * b, y0 + b, zc0 + back}, {x + 0.5f * b, y1 - b, zc1}, mbd);
        }
        // Where the rows meet (a drawer over a door, shelves behind glass).
        std::vector<float> ry(static_cast<std::size_t>(rws + 1));
        for (int r = 0; r <= rws; ++r) ry[static_cast<std::size_t>(r)] = y0 + (y1 - y0) * static_cast<float>(r) / static_cast<float>(rws);
        if (fm == 4) ry[1] = y1 - std::min(0.22f, 0.3f * (y1 - y0));
        if (fm != 1)
            for (int r = 1; r < rws; ++r) {
                const float y = ry[static_cast<std::size_t>(r)];
                s.block({-hw + b, y - 0.5f * b, zc0 + back}, {hw - b, y + 0.5f * b, zc1}, mbd);
            }

        const bool lowHandle = bs == 2 && y0 > 1.0f;   // a wall unit opens from below
        auto handle = [&](float xa, float xb, float ya, float yb, bool door, int edge) {
            if (handles <= 0) return;
            const float zf = hd;
            if (handles == 3) {   // a dark grip strip along the top (bottom on a wall unit) edge
                const float y = lowHandle ? ya + 0.004f : yb - 0.024f;
                s.block({xa + 0.002f, y, zf - 0.001f}, {xb - 0.002f, y + 0.02f, zf + 0.002f}, mhd);
                return;
            }
            glm::vec3 c;
            bool vertical = door;
            if (door) {
                const float x = edge > 0 ? xb - 0.045f : xa + 0.045f;
                float y = (yb - ya) > 1.2f ? 1.0f : lowHandle ? ya + 0.1f : yb - 0.1f;
                y = clampf(y, ya + 0.1f, yb - 0.1f);
                c = {x, y, zf};
            } else {
                c = {0.5f * (xa + xb), (yb - ya) < 0.2f ? 0.5f * (ya + yb) : yb - 0.07f, zf};
            }
            if (handles == 2) {
                s.rod(c, c + glm::vec3(0, 0, 0.022f), 0.008f, 0.008f, 10, mhd);
                s.rod(c + glm::vec3(0, 0, 0.018f), c + glm::vec3(0, 0, 0.03f), 0.016f, 0.016f, 14, mhd);
                return;
            }
            const float span = vertical ? std::min(0.16f, 0.5f * (yb - ya)) : std::min(0.16f, 0.6f * (xb - xa));
            const glm::vec3 d = vertical ? glm::vec3(0, 0.5f * span, 0) : glm::vec3(0.5f * span, 0, 0);
            const glm::vec3 out(0.0f, 0.0f, 0.03f);
            s.beam(c - d * 1.15f + out, c + d * 1.15f + out, 0.012f, 0.012f, vertical ? kZ : kUp, mhd);
            s.beam(c - d, c - d + out, 0.01f, 0.01f, kUp, mhd);
            s.beam(c + d, c + d + out, 0.01f, 0.01f, kUp, mhd);
        };

        for (int c = 0; c < cols; ++c) {
            const float xa = -hw + cw * static_cast<float>(c), xb = xa + cw;
            // Doors in pairs open from the middle; an odd one opens on the right.
            const int edge = (cols % 2 == 1 && c == cols / 2) ? 1 : (c < cols / 2 ? 1 : -1);
            for (int r = 0; r < rws; ++r) {
                const float ya = ry[static_cast<std::size_t>(r)], yb = ry[static_cast<std::size_t>(r + 1)];
                const bool drawer = fm == 1 || (fm == 4 && r == 1);
                const bool glass = fm == 3;
                if (fm == 2 || glass) {
                    const int n = std::clamp(shelves, 0, 8);
                    const float ia = ya + (r == 0 ? b : 0.5f * b), ib = yb - (r == rws - 1 ? b : 0.5f * b);
                    for (int k = 1; k <= n; ++k) {
                        const float y = ia + (ib - ia) * static_cast<float>(k) / static_cast<float>(n + 1);
                        s.block({xa + (c == 0 ? b : 0.5f * b), y - 0.5f * b, zc0 + back},
                                {xb - (c == cols - 1 ? b : 0.5f * b), y + 0.5f * b, zc1 - 0.01f}, mbd);
                    }
                }
                if (fm == 2) continue;
                const float fa = xa + 0.5f * gap, fb = xb - 0.5f * gap, ga = ya + 0.5f * gap, gb = yb - 0.5f * gap;
                if (glass) {
                    const float fw = std::min(0.05f, 0.2f * std::min(fb - fa, gb - ga));
                    s.block({fa, ga, zc1}, {fb, ga + fw, hd}, mfr);
                    s.block({fa, gb - fw, zc1}, {fb, gb, hd}, mfr);
                    s.block({fa, ga + fw, zc1}, {fa + fw, gb - fw, hd}, mfr);
                    s.block({fb - fw, ga + fw, zc1}, {fb, gb - fw, hd}, mfr);
                    s.block({fa + fw, ga + fw, zc1 + 0.4f * ft}, {fb - fw, gb - fw, zc1 + 0.6f * ft}, mgl);
                } else {
                    s.block({fa, ga, zc1}, {fb, gb, hd}, mfr);
                }
                handle(fa, fb, ga, gb, !drawer, edge);
            }
        }

        // What it stands on.
        if (bs == 0 && y0 > 0.005f) {
            s.block({-hw + 0.005f, 0.0f, zc0 + 0.02f}, {hw - 0.005f, y0, zc1 - 0.04f}, mbd);
        } else if (bs == 1 && y0 > 0.005f) {
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2)
                    leg(s, 2, {sx * (hw - 0.05f), sz * (hd - 0.06f)}, 0.0f, y0, 0.04f, mbd);
        }
        if (worktop) s.block({-hw, y1, -hd}, {hw, H, hd + clampf(overhang, 0.0f, 0.3f)}, mtp);
    }
};

// ================================================================================
// Shelf
// ================================================================================

// A bookcase, an open frame, boards on the wall, a ladder shelf leaning on it
// -- and books on the boards, as many as asked, in three colours at random.
class ShelfNode : public Piece<ShelfNode> {
public:
    int   style   = 0;      // 0 bookcase, 1 open frame, 2 wall boards, 3 ladder
    float width   = 0.80f;
    float height  = 1.90f;
    float depth   = 0.30f;
    float board   = 0.025f;
    int   boards  = 6;
    int   columns = 1;
    bool  back    = true;
    float books   = 0.6f;
    int   seed    = 1;
    std::string matBody, matBooks, matBooksB, matBooksC;

    const char* typeId() const override { return "shelf"; }
    const char* displayName() const override { return "Shelf"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = ShelfNode;
            std::vector<Property> v;
            v.push_back(choice("Style", "style", &S::style, {"Bookcase", "Open frame", "Wall boards", "Ladder"}));
            v.push_back(number("Width", "width", &S::width, 0.05f, 0.2f, 6.0f));
            v.push_back(number("Height", "height", &S::height, 0.05f, 0.2f, 3.0f));
            v.push_back(number("Depth", "depth", &S::depth, 0.01f, 0.1f, 0.8f));
            v.push_back(number("Board", "board", &S::board, 0.002f, 0.01f, 0.08f, "%.3f"));
            v.push_back(whole("Boards", "boards", &S::boards, 1, 16));
            v.push_back(only<S>(whole("Columns", "columns", &S::columns, 1, 8),
                                [](const S& s) { return s.style <= 1; }));
            v.push_back(only<S>(flag("Back panel", "back", &S::back), [](const S& s) { return s.style <= 1; }));
            v.push_back(number("Books (share)", "books", &S::books, 0.05f, 0.0f, 1.0f));
            v.push_back(only<S>(whole("Seed", "seed", &S::seed, 0, 99999), [](const S& s) { return s.books > 0.0f; }));
            v.push_back(field("Body material", "materialBody", PropKind::Text, &S::matBody));
            v.push_back(field("Books", "materialBooks", PropKind::Text, &S::matBooks));
            v.push_back(field("Books, second colour", "materialBooksB", PropKind::Text, &S::matBooksB));
            v.push_back(field("Books, third colour", "materialBooksC", PropKind::Text, &S::matBooksC));
            placeRows<S>(v);
            return v;
        }();
        return p;
    }

    // Books on a board from x0 to x1, standing on y, in `clear` of height,
    // between zb (the back) and zf (the front).
    void fill(Shop& s, float x0, float x1, float y, float clear, float zb, float zf, int& die) const {
        const float share = clampf(books, 0.0f, 1.0f);
        if (share <= 0.0f || clear < 0.12f || zf - zb < 0.08f || x1 - x0 < 0.05f) return;
        const AssetId mats[3] = {slot(matBooks), slot(matBooksB), slot(matBooksC)};
        auto roll = [&]() { return proc::random01(die++, seed); };
        auto colour = [&]() { return mats[std::min(2, static_cast<int>(roll() * 3.0f))]; };
        const float end = x0 + share * (x1 - x0);
        const float tall = std::min(clear - 0.02f, 0.34f);
        float x = x0 + 0.01f;
        while (x < end - 0.02f) {
            const float r = roll();
            if (r < 0.06f) { x += 0.03f + 0.05f * roll(); continue; }            // a gap
            if (r < 0.14f && end - x > 0.26f) {                                   // a stack lying down
                const int n = 2 + static_cast<int>(roll() * 4.0f);
                const float w = 0.16f + 0.08f * roll(), d = std::min(zf - zb - 0.02f, 0.13f + 0.08f * roll());
                float yy = y;
                for (int k = 0; k < n && yy < y + clear - 0.06f; ++k) {
                    const float t = 0.02f + 0.025f * roll();
                    const float sh = 0.01f * (roll() - 0.5f);
                    s.block({x + sh, yy, zf - 0.01f - d}, {x + w + sh, yy + t, zf - 0.01f}, colour());
                    yy += t;
                }
                x += w + 0.02f;
                continue;
            }
            const float t = 0.018f + 0.035f * roll();
            const float h = tall * (0.62f + 0.38f * roll());
            const float d = std::min(zf - zb - 0.015f, 0.14f + 0.09f * roll());
            if (x + t > end) break;
            if (end - x < 0.12f && end - x > t + 0.06f && roll() < 0.6f) {      // the last one leans
                const float ang = 12.0f + 14.0f * roll();
                const std::size_t from = s.mark();
                const float xr = x + t + h * std::sin(glm::radians(ang));
                s.block({xr - t, y, zf - 0.01f - d}, {xr, y + h, zf - 0.01f}, colour());
                s.xform(from, glm::translate(glm::mat4(1.0f), glm::vec3(xr, y, 0.0f)) *
                                  glm::rotate(glm::mat4(1.0f), glm::radians(ang), kZ) *
                                  glm::translate(glm::mat4(1.0f), glm::vec3(-xr, -y, 0.0f)));
                break;
            }
            s.block({x, y, zf - 0.01f - d}, {x + t, y + h, zf - 0.01f}, colour());
            x += t + 0.001f;
        }
    }

    void build(Shop& s) const {
        const AssetId mb = slot(matBody);
        const int   st = std::clamp(style, 0, 3);
        const float W = clampf(width, 0.2f, 6.0f), H = clampf(height, 0.2f, 3.0f), D = clampf(depth, 0.1f, 0.8f);
        const float b = clampf(board, 0.01f, 0.08f);
        const float hw = 0.5f * W, hd = 0.5f * D;
        const int   n = std::clamp(boards, 1, 16);
        const int   cols = st <= 1 ? std::clamp(columns, 1, 8) : 1;
        int die = 0;

        if (st == 0 || st == 1) {
            const bool box = st == 0;
            const float plinth = box ? 0.07f : 0.06f;
            const int levels = std::max(n, 2);
            std::vector<float> ly;   // underside of each board
            for (int i = 0; i < levels; ++i)
                ly.push_back(plinth + (H - plinth - b) * static_cast<float>(i) / static_cast<float>(levels - 1));
            const float post = box ? b : 0.03f;
            const float zb = -hd + (back ? 0.008f : 0.0f);
            if (box) {
                s.block({-hw, 0.0f, -hd}, {-hw + b, H, hd}, mb);
                s.block({hw - b, 0.0f, -hd}, {hw, H, hd}, mb);
                s.block({-hw + b, 0.0f, hd - 0.03f}, {hw - b, plinth, hd - 0.01f}, mb);
            } else {
                for (int sx = -1; sx <= 1; sx += 2)
                    for (int sz = -1; sz <= 1; sz += 2)
                        leg(s, 0, {sx * (hw - 0.5f * post), sz * (hd - 0.5f * post)}, 0.0f, H, post, mb);
            }
            if (back) s.block({-hw + post, plinth, -hd}, {hw - post, H - (box ? b : 0.0f), -hd + 0.008f}, mb);
            const float cw = (W - 2.0f * post) / static_cast<float>(cols);
            for (int c = 1; c < cols; ++c) {
                const float x = -hw + post + cw * static_cast<float>(c);
                s.block({x - 0.5f * b, ly.front() + b, zb}, {x + 0.5f * b, ly.back(), hd - (box ? 0.0f : 0.01f)}, mb);
            }
            const float bx0 = box ? -hw + b : -hw, bx1 = box ? hw - b : hw;
            for (int i = 0; i < levels; ++i) {
                s.block({bx0, ly[static_cast<std::size_t>(i)], zb}, {bx1, ly[static_cast<std::size_t>(i)] + b, hd}, mb);
                if (i + 1 == levels) break;
                const float clear = ly[static_cast<std::size_t>(i + 1)] - ly[static_cast<std::size_t>(i)] - b;
                for (int c = 0; c < cols; ++c) {
                    const float xa = -hw + post + cw * static_cast<float>(c) + (c ? 0.5f * b : 0.0f);
                    const float xb = -hw + post + cw * static_cast<float>(c + 1) - (c + 1 < cols ? 0.5f * b : 0.0f);
                    fill(s, xa, xb, ly[static_cast<std::size_t>(i)] + b, clear, zb, hd, die);
                }
            }
        } else if (st == 2) {
            const float step = 0.34f;
            for (int i = 0; i < n; ++i) {
                const float y = H - b - step * static_cast<float>(i);
                if (y < 0.05f) break;
                s.block({-hw, y, -hd}, {hw, y + b, hd}, mb);
                for (int sx = -1; sx <= 1; sx += 2) {
                    const float x = sx * (hw - std::min(0.15f, 0.2f * W));
                    s.block({x - 0.012f, y - 0.12f, -hd}, {x + 0.012f, y, -hd + 0.02f}, mb);
                    s.beam({x, y - 0.012f, -hd + 0.02f}, {x, y - 0.012f, hd - 0.04f}, 0.024f, 0.024f, kUp, mb);
                }
                fill(s, -hw, hw, y + b, std::min(step - b, 0.3f), -hd, hd, die);
            }
        } else {
            // Two rails leaning on the wall, boards hung between them -- deep
            // at the floor, shallow at the top.
            const float zTop = -hd + 0.16f, zBot = hd;
            auto railZ = [&](float y) { return zBot + (zTop - zBot) * (y / H); };
            for (int sx = -1; sx <= 1; sx += 2) {
                const float x = sx * (hw - 0.0175f);
                s.beam({x, 0.0f, zBot}, {x, H, zTop}, 0.035f, 0.05f, kZ, mb);
            }
            const int levels = std::max(n, 1);
            for (int i = 0; i < levels; ++i) {
                const float y = 0.12f + (H - 0.30f) * (levels == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(levels - 1));
                const float zf = railZ(y) - 0.01f;
                s.block({-hw + 0.035f, y, -hd + 0.02f}, {hw - 0.035f, y + b, zf}, mb);
                const float clear = levels == 1 ? 0.3f : (H - 0.30f) / static_cast<float>(levels - 1) - b;
                fill(s, -hw + 0.035f, hw - 0.035f, y + b, std::min(clear, 0.32f), -hd + 0.02f, zf, die);
            }
        }
    }
};

// ================================================================================
// Rug
// ================================================================================

class RugNode : public Piece<RugNode> {
public:
    int   shape     = 0;    // 0 rectangle, 1 round, 2 oval
    float width     = 2.00f;
    float depth     = 1.40f;
    float thickness = 0.012f;
    bool  border    = true;
    float borderWidth = 0.10f;
    std::string matRug, matBorder;

    const char* typeId() const override { return "rug"; }
    const char* displayName() const override { return "Rug"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = RugNode;
            std::vector<Property> v;
            v.push_back(choice("Shape", "shape", &S::shape, {"Rectangle", "Round", "Oval"}));
            v.push_back(number("Width", "width", &S::width, 0.1f, 0.3f, 10.0f));
            v.push_back(only<S>(number("Depth", "depth", &S::depth, 0.1f, 0.3f, 10.0f),
                                [](const S& s) { return s.shape != 1; }));
            v.push_back(number("Thickness", "thickness", &S::thickness, 0.002f, 0.002f, 0.06f, "%.3f"));
            v.push_back(flag("Border", "border", &S::border));
            v.push_back(only<S>(number("Border width", "borderWidth", &S::borderWidth, 0.01f, 0.01f, 1.0f),
                                [](const S& s) { return s.border; }));
            v.push_back(field("Rug material", "materialRug", PropKind::Text, &S::matRug));
            v.push_back(only<S>(field("Border material", "materialBorder", PropKind::Text, &S::matBorder),
                                [](const S& s) { return s.border; }));
            placeRows<S>(v);
            return v;
        }();
        return p;
    }

    void build(Shop& s) const {
        const int   sh = std::clamp(shape, 0, 2);
        const float hw = 0.5f * clampf(width, 0.3f, 10.0f);
        const float hd = sh == 1 ? hw : 0.5f * clampf(depth, 0.3f, 10.0f);
        const float t  = clampf(thickness, 0.002f, 0.06f);
        auto outline = [&](float inset) {
            return sh == 0 ? roundRectOutline(hw - inset, hd - inset, 0.01f, 2) : ellipseOutline(hw - inset, hd - inset, 56);
        };
        const float bw = clampf(borderWidth, 0.01f, 0.45f * std::min(hw, hd));
        if (border) {
            s.slab(outline(0.0f), 0.0f, t, slot(matBorder));
            s.slab(outline(bw), 0.0f, t + 0.002f, slot(matRug));
        } else {
            s.slab(outline(0.0f), 0.0f, t, slot(matRug));
        }
    }
};

// ================================================================================
// Lamp
// ================================================================================

class LampNode : public Piece<LampNode> {
public:
    int   kind        = 0;     // 0 floor, 1 table, 2 pendant
    int   shade       = 0;     // 0 drum, 1 cone, 2 dome
    float height      = 1.60f; // floor and table: to the top of the shade; pendant: the ceiling
    float shadeRadius = 0.22f;
    float shadeHeight = 0.28f;
    float drop        = 0.80f; // pendant: ceiling to the top of the shade
    std::string matBody, matShade;

    const char* typeId() const override { return "lamp"; }
    const char* displayName() const override { return "Lamp"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> p = [] {
            using S = LampNode;
            std::vector<Property> v;
            v.push_back(choice("Kind", "kind", &S::kind, {"Floor", "Table", "Pendant"}));
            v.push_back(choice("Shade", "shade", &S::shade, {"Drum", "Cone", "Dome"}));
            v.push_back(number("Height", "height", &S::height, 0.05f, 0.2f, 5.0f));
            v.push_back(number("Shade radius", "shadeRadius", &S::shadeRadius, 0.01f, 0.05f, 1.0f));
            v.push_back(number("Shade height", "shadeHeight", &S::shadeHeight, 0.01f, 0.05f, 1.0f));
            v.push_back(only<S>(number("Drop", "drop", &S::drop, 0.05f, 0.1f, 4.0f),
                                [](const S& s) { return s.kind == 2; }));
            v.push_back(field("Body material", "materialBody", PropKind::Text, &S::matBody));
            v.push_back(field("Shade material", "materialShade", PropKind::Text, &S::matShade));
            placeRows<S>(v);
            return v;
        }();
        return p;
    }

    void build(Shop& s) const {
        const AssetId mb = slot(matBody), msd = slot(matShade);
        const int   k  = std::clamp(kind, 0, 2);
        const float H  = clampf(height, 0.2f, 5.0f);
        const float R  = clampf(shadeRadius, 0.05f, 1.0f);
        const float SH = clampf(shadeHeight, 0.05f, std::min(1.0f, 0.8f * H));
        float top;   // top of the shade
        if (k == 2) {
            const float dr = clampf(drop, 0.1f, H - SH - 0.01f);
            s.rod({0.0f, H - 0.04f, 0.0f}, {0.0f, H, 0.0f}, 0.06f, 0.06f, 20, mb);
            s.rod({0.0f, H - dr, 0.0f}, {0.0f, H - 0.04f, 0.0f}, 0.004f, 0.004f, 6, mb);
            top = H - dr;
        } else if (k == 1) {
            const float body = std::max(0.05f, H - SH - 0.04f);
            s.rod({0.0f, 0.0f, 0.0f}, {0.0f, 0.02f, 0.0f}, 0.6f * R, 0.6f * R, 24, mb);
            s.rod({0.0f, 0.02f, 0.0f}, {0.0f, body, 0.0f}, 0.4f * R, 0.25f * R, 20, mb);
            s.rod({0.0f, body, 0.0f}, {0.0f, H - 0.3f * SH, 0.0f}, 0.008f, 0.008f, 8, mb);
            top = H;
        } else {
            s.rod({0.0f, 0.0f, 0.0f}, {0.0f, 0.025f, 0.0f}, 0.7f * R, 0.7f * R, 28, mb);
            s.rod({0.0f, 0.025f, 0.0f}, {0.0f, H - 0.3f * SH, 0.0f}, 0.012f, 0.012f, 10, mb);
            top = H;
        }
        const float y0 = top - SH;
        const int segs = 32;
        if (shade == 2) {
            // A dome: rings on a quarter circle, from the rim up to a little cap.
            std::vector<std::vector<glm::vec3>> rings;
            const int steps = 6;
            for (int i = 0; i <= steps; ++i) {
                const float a = 0.5f * kPi * static_cast<float>(i) / static_cast<float>(steps) * 0.92f;
                rings.push_back(lift(ellipseOutline(R * std::cos(a), R * std::cos(a), segs), y0 + SH * std::sin(a)));
            }
            for (int i = 0; i + 1 < static_cast<int>(rings.size()); ++i)
                s.loft(rings[static_cast<std::size_t>(i)], rings[static_cast<std::size_t>(i + 1)], msd);
        } else {
            const float rt = shade == 1 ? 0.45f * R : 0.88f * R;
            s.loft(lift(ellipseOutline(R, R, segs), y0), lift(ellipseOutline(rt, rt, segs), top), msd);
        }
    }
};

// ================================================================================
// Registration
// ================================================================================

struct RegisterFurnitureNodes {
    template <class T>
    static void add(const char* tip) {
        const T t;
        proc::registerType({t.typeId(), t.displayName(), "Furniture", tip,
                            [] { return std::unique_ptr<proc::Node>(std::make_unique<T>()); }});
    }
    RegisterFurnitureNodes() {
        add<TableNode>("A table: dining, coffee, side or desk. Rectangular, rounded, round or\n"
                       "oval, on legs, trestles, a pedestal, sled frames or slab ends.");
        add<ChairNode>("A chair or a stool: four legs, a cantilever, a swivel or a sled,\n"
                       "with a back of slats, rails, a board or a cushion.");
        add<SofaNode>("A sofa or an armchair (one seat): arms, cushions, pillows, a chaise.");
        add<BedNode>("A bed with its mattress, duvet and pillows: on legs, a platform, a box\n"
                     "spring or four posts; a panel, padded or slatted headboard.");
        add<CabinetNode>("Sideboard, wardrobe, chest of drawers, nightstand, kitchen unit:\n"
                         "columns and rows of doors, drawers, glass or open shelves.");
        add<ShelfNode>("A bookcase, an open frame, boards on the wall or a ladder shelf,\n"
                       "filled with books at random.");
        add<RugNode>("A rug, with or without a border.");
        add<LampNode>("A floor, table or pendant lamp with a drum, cone or dome shade.");
    }
} g_registerFurnitureNodes;

} // namespace
