#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "ProcGraph.hpp"

// The node kinds' common kit (ProcNodes.cpp, ProcNodesBuild.cpp): Property rows
// for their settings, and the geometry steps more than one kind takes --
// moving a whole stream, appending one to another, growing corners and faces
// with their parallel arrays kept in step, cutting and smoothing lines.
namespace proc::kit {

using proc::Curve;
using proc::FaceSet;
using proc::Geo;

inline constexpr float kPi  = 3.14159265358979f;
inline constexpr float kTau = 6.28318530718f;

// --- Property rows ---------------------------------------------------------------

template <class T, class V>
Property field(const char* label, const char* key, PropKind kind, V T::*member) {
    Property p;
    p.label = label;
    p.key   = key;
    p.kind  = kind;
    p.field = [member](void* o) -> void* { return &(static_cast<T*>(o)->*member); };
    return p;
}
// A length, an angle, an amount: `step` is one click of the stepper.
template <class T>
Property number(const char* label, const char* key, float T::*m, float step,
                float lo, float hi, const char* fmt = "%.2f") {
    Property p = field(label, key, PropKind::Float, m);
    p.speed = step; p.min = lo; p.max = hi; p.fmt = fmt;
    return p;
}
template <class T>
Property whole(const char* label, const char* key, int T::*m, int lo, int hi, int step = 1) {
    Property p = field(label, key, PropKind::Int, m);
    p.speed = static_cast<float>(step);
    p.min   = static_cast<float>(lo);
    p.max   = static_cast<float>(hi);
    return p;
}
// Unbounded (min == max): a position or an offset can go anywhere.
template <class T>
Property vector3(const char* label, const char* key, glm::vec3 T::*m, float step,
                 const char* fmt = "%.1f") {
    Property p = field(label, key, PropKind::Vec3, m);
    p.speed = step; p.fmt = fmt;
    return p;
}
template <class T>
Property flag(const char* label, const char* key, bool T::*m) {
    return field(label, key, PropKind::Bool, m);
}
template <class T>
Property choice(const char* label, const char* key, int T::*m, std::vector<std::string> labels) {
    Property p = field(label, key, PropKind::EnumInt, m);
    p.enumLabels = std::move(labels);
    return p;
}
inline std::vector<std::string> axisLabels() { return {"X", "Y", "Z"}; }

// The face choice every face-picking node carries, as three rows.
template <class T>
void faceRows(std::vector<Property>& v) {
    Property s = field("Faces", "faces", PropKind::EnumInt, &T::faces);
    s.enumLabels = proc::faceSetLabels();
    v.push_back(std::move(s));
    v.push_back(number("Share", "share", &T::share, 0.05f, 0.0f, 1.0f, "%.2f"));
    Property seed = whole("Seed", "seed", &T::seed, 0, 99999);
    // Only matters when something is left to chance.
    seed.visible = [](const void* o) { return static_cast<const T*>(o)->share < 1.0f; };
    v.push_back(std::move(seed));
}

// --- Geometry helpers ---------------------------------------------------------------

// Stand a shape built along +Y on `axis` (0 X, 1 Y, 2 Z). A rotation, so the
// winding survives.
inline glm::mat4 axisFrame(int axis) {
    if (axis == 0) return glm::rotate(glm::mat4(1.0f), -0.5f * kPi, glm::vec3(0.0f, 0.0f, 1.0f));
    if (axis == 2) return glm::rotate(glm::mat4(1.0f),  0.5f * kPi, glm::vec3(1.0f, 0.0f, 0.0f));
    return glm::mat4(1.0f);
}
inline glm::vec3 axisVec(int axis) {
    return axis == 0 ? glm::vec3(1, 0, 0) : axis == 2 ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
}

// Degrees, in the scene's own order (Rz * Ry * Rx, see SceneTypes): a turn typed
// here reads the same as the same numbers typed into an object's rotation.
inline glm::mat4 eulerDeg(const glm::vec3& deg) {
    glm::mat4 r(1.0f);
    r = glm::rotate(r, glm::radians(deg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    r = glm::rotate(r, glm::radians(deg.y), glm::vec3(0.0f, 1.0f, 0.0f));
    r = glm::rotate(r, glm::radians(deg.x), glm::vec3(1.0f, 0.0f, 0.0f));
    return r;
}

// The turn that takes +Y onto `n`: what stands a copy up on a surface.
inline glm::mat4 upTo(const glm::vec3& n) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const float c = glm::dot(up, n);
    if (c > 0.9999f) return glm::mat4(1.0f);
    if (c < -0.9999f) return glm::rotate(glm::mat4(1.0f), kPi, glm::vec3(1.0f, 0.0f, 0.0f));
    return glm::rotate(glm::mat4(1.0f), std::acos(std::clamp(c, -1.0f, 1.0f)),
                       glm::normalize(glm::cross(up, n)));
}

// What carries a surface direction through `xf` (its inverse transpose), and
// the direction carried, still of unit length. A transform that flattens
// everything to a plane has no inverse; the directions stay as they were then.
inline glm::mat3 normalMatrix(const glm::mat4& xf) {
    const glm::mat3 m(xf);
    if (std::fabs(glm::determinant(m)) < 1e-12f) return glm::mat3(1.0f);
    return glm::transpose(glm::inverse(m));
}
inline glm::vec3 turnNormal(const glm::mat3& nx, const glm::vec3& n) {
    const glm::vec3 t = nx * n;
    return glm::dot(t, t) > 1e-12f ? glm::normalize(t) : n;
}

// Move every point. A mirroring transform turns faces inside out and runs
// curves the other way, so both are reversed back (a face keeps its first
// corner; a curve's selection is reversed with its points).
inline void transformGeo(Geo& g, const glm::mat4& xf) {
    for (glm::vec3& v : g.mesh.verts) v = glm::vec3(xf * glm::vec4(v, 1.0f));
    const glm::mat3 nx = normalMatrix(xf);
    for (Curve& c : g.curves) {
        for (glm::vec3& p : c.pts) p = glm::vec3(xf * glm::vec4(p, 1.0f));
        for (glm::vec3& n : c.nrm) n = turnNormal(nx, n);
    }
    for (proc::Instance& in : g.instances) in.xf = xf * in.xf;
    if (glm::determinant(glm::mat3(xf)) < 0.0f) {
        for (std::vector<int>& f : g.mesh.faces)
            if (f.size() > 2) std::reverse(f.begin() + 1, f.end());
        for (Curve& c : g.curves) {
            std::reverse(c.pts.begin(), c.pts.end());
            std::reverse(c.sel.begin(), c.sel.end());
            std::reverse(c.nrm.begin(), c.nrm.end());
        }
    }
}

// `s`'s faces added to `d`'s as a separate piece. The parallel arrays come
// along only if either side has them, and then for every face and corner.
inline void appendMesh(EditMesh& d, const EditMesh& s) {
    const int  base  = static_cast<int>(d.verts.size());
    const bool paint = !d.paint.empty() || !s.paint.empty();
    const bool mats  = !d.faceMat.empty() || !s.faceMat.empty();
    const bool uvs   = !d.faceUV.empty() || !s.faceUV.empty();
    if (paint) d.syncPaint();
    if (mats)  d.syncFaceMat();
    if (uvs)   d.syncFaceUv();
    d.verts.insert(d.verts.end(), s.verts.begin(), s.verts.end());
    if (paint)
        for (int i = 0; i < static_cast<int>(s.verts.size()); ++i) d.paint.push_back(s.paintAt(i));
    for (int f = 0; f < static_cast<int>(s.faces.size()); ++f) {
        std::vector<int> loop = s.faces[static_cast<std::size_t>(f)];
        for (int& v : loop) v += base;
        d.faces.push_back(std::move(loop));
        if (mats) d.faceMat.push_back(s.faceMaterial(f));
        if (uvs)  d.faceUV.push_back(s.faceUv(f));
    }
}

// All of `s` added to `d`: faces, curves and selection. A stream without a
// selection merged with one that has it comes in unselected -- Houdini's rule
// for a point that is not in the group.
inline void append(Geo& d, const Geo& s) {
    const bool sel = d.hasSel || s.hasSel;
    if (sel && !d.hasSel) {
        d.hasSel = true;
        d.meshSel.assign(d.mesh.verts.size(), 0);
        for (Curve& c : d.curves) c.sel.assign(c.pts.size(), 0);
    }
    d.syncSel();
    appendMesh(d.mesh, s.mesh);
    if (sel)
        for (int i = 0; i < static_cast<int>(s.mesh.verts.size()); ++i)
            d.meshSel.push_back(s.hasSel && s.meshPicked(i) ? 1 : 0);
    d.instances.insert(d.instances.end(), s.instances.begin(), s.instances.end());
    for (const Curve& c : s.curves) {
        Curve cc = c;
        if (sel) {
            cc.sel.resize(cc.pts.size(), 0);
            if (!s.hasSel) std::fill(cc.sel.begin(), cc.sel.end(), 0);
        } else {
            cc.sel.clear();
        }
        d.curves.push_back(std::move(cc));
    }
}

// A corner, and a face grown out of face `like` (its material and texture
// placement with it) -- the parallel arrays kept in step only where they exist.
inline int addCorner(EditMesh& m, const glm::vec3& p) {
    m.verts.push_back(p);
    if (!m.paint.empty()) m.syncPaint();
    return static_cast<int>(m.verts.size()) - 1;
}
inline int addFace(EditMesh& m, std::vector<int> loop, int like) {
    const fitzel::AssetId  mat = m.faceMaterial(like);
    const EditMesh::FaceUV uv  = m.faceUv(like);
    const bool mats = !m.faceMat.empty(), uvs = !m.faceUV.empty();
    if (mats) m.syncFaceMat();
    if (uvs)  m.syncFaceUv();
    m.faces.push_back(std::move(loop));
    if (mats) m.faceMat.push_back(mat);
    if (uvs)  m.faceUV.push_back(uv);
    return static_cast<int>(m.faces.size()) - 1;
}

// A loop of corners as one face, dropping a corner that repeats its
// neighbour (a tip ring collapsed to one point); nothing if fewer than three
// are left.
inline void addLoop(EditMesh& m, std::vector<int> q) {
    q.erase(std::unique(q.begin(), q.end()), q.end());
    while (q.size() > 1 && q.front() == q.back()) q.pop_back();
    if (q.size() >= 3) m.faces.push_back(std::move(q));
}

// Drop the faces marked in `drop`, and every corner no face uses any more --
// the selection of the corners kept in step. One pass for any number of faces.
inline void dropFaces(Geo& g, const std::vector<char>& drop) {
    EditMesh& m = g.mesh;
    EditMesh out;
    std::vector<char> sel;
    const bool mats = !m.faceMat.empty(), uvs = !m.faceUV.empty(), paint = !m.paint.empty();
    std::vector<int> remap(m.verts.size(), -1);
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        if (f < drop.size() && drop[f]) continue;
        std::vector<int> loop = m.faces[f];
        for (int& v : loop) {
            if (remap[static_cast<std::size_t>(v)] < 0) {
                remap[static_cast<std::size_t>(v)] = static_cast<int>(out.verts.size());
                out.verts.push_back(m.verts[static_cast<std::size_t>(v)]);
                if (paint) out.paint.push_back(m.paintAt(v));
                if (g.hasSel) sel.push_back(g.meshPicked(v) ? 1 : 0);
            }
            v = remap[static_cast<std::size_t>(v)];
        }
        out.faces.push_back(std::move(loop));
        if (mats) out.faceMat.push_back(m.faceMaterial(static_cast<int>(f)));
        if (uvs)  out.faceUV.push_back(m.faceUv(static_cast<int>(f)));
    }
    m = std::move(out);
    if (g.hasSel) g.meshSel = std::move(sel);
}

// Raise (or sink, `depth` < 0) a panel out of face `f`: a rim `inset` of the
// way in from its edges (0..1 of the face's own size, so one number suits a
// hull plate and a cap alike), walls `depth` along the face's normal, and the
// face itself on top -- it keeps its index and its material.
inline void panelFace(EditMesh& m, int f, float inset, float depth) {
    const std::vector<int> loop = m.faces[static_cast<std::size_t>(f)];
    const glm::vec3 c = m.faceCenter(f);
    const glm::vec3 n = m.faceNormal(f);
    const int k = static_cast<int>(loop.size());
    const float keep = 1.0f - std::clamp(inset, 0.0f, 0.95f);
    std::vector<int> inner(static_cast<std::size_t>(k)), top(static_cast<std::size_t>(k));
    for (int i = 0; i < k; ++i)
        inner[static_cast<std::size_t>(i)] =
            addCorner(m, c + (m.verts[static_cast<std::size_t>(loop[static_cast<std::size_t>(i)])] - c) * keep);
    const bool raised = std::fabs(depth) > 1e-5f;
    for (int i = 0; i < k; ++i)
        top[static_cast<std::size_t>(i)] =
            raised ? addCorner(m, m.verts[static_cast<std::size_t>(inner[static_cast<std::size_t>(i)])] + n * depth)
                   : inner[static_cast<std::size_t>(i)];
    for (int i = 0; i < k; ++i) {
        const std::size_t a = static_cast<std::size_t>(i), b = static_cast<std::size_t>((i + 1) % k);
        addFace(m, {loop[a], loop[b], inner[b], inner[a]}, f);
        if (raised) addFace(m, {inner[a], inner[b], top[b], top[a]}, f);
    }
    m.faces[static_cast<std::size_t>(f)] = top;
}

// Signed volume (fans from the origin): positive when the faces look out.
inline double signedVolume(const EditMesh& m) {
    double v = 0.0;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t i = 1; i + 1 < f.size(); ++i) {
            const glm::dvec3 a(m.verts[static_cast<std::size_t>(f[0])]);
            const glm::dvec3 b(m.verts[static_cast<std::size_t>(f[i])]);
            const glm::dvec3 c(m.verts[static_cast<std::size_t>(f[i + 1])]);
            v += glm::dot(a, glm::cross(b, c)) / 6.0;
        }
    return v;
}
inline void flipFaces(EditMesh& m, std::size_t from = 0) {
    for (std::size_t f = from; f < m.faces.size(); ++f)
        if (m.faces[f].size() > 2) std::reverse(m.faces[f].begin() + 1, m.faces[f].end());
}

// A closed loop of points as either a face (filled) or a curve. `pts` run
// counter-clockwise seen from where the face should look.
inline void emitLoop(Geo& out, const std::vector<glm::vec3>& pts, bool closed, bool filled) {
    if (pts.size() < 2) return;
    if (filled && closed && pts.size() >= 3) {
        std::vector<int> loop;
        for (const glm::vec3& p : pts) loop.push_back(addCorner(out.mesh, p));
        out.mesh.faces.push_back(std::move(loop));
        return;
    }
    Curve c;
    c.pts    = pts;
    c.closed = closed && pts.size() >= 3;
    out.curves.push_back(std::move(c));
}

// The first curve that is a line (not loose points), or null: what Sweep and
// Revolve can use.
inline const Curve* firstLine(const Geo& g) {
    for (const Curve& c : g.curves)
        if (!c.loose) return &c;
    return nullptr;
}

inline std::string needsInput(const std::vector<const Geo*>& in) {
    return (in.empty() || !in[0]) ? "Nothing wired into its input" : "";
}

// Where along a polyline, by arc length: `count` points spread evenly, both
// ends included on an open line, the seam not repeated on a closed one.
inline std::vector<glm::vec3> resampleLine(const std::vector<glm::vec3>& pts, bool closed, int count) {
    std::vector<glm::vec3> out;
    const std::size_t n = pts.size();
    if (n < 2 || count < 2) return pts;
    std::vector<float> acc(1, 0.0f);
    const std::size_t segs = closed ? n : n - 1;
    for (std::size_t i = 0; i < segs; ++i)
        acc.push_back(acc.back() + glm::length(pts[(i + 1) % n] - pts[i]));
    const float total = acc.back();
    if (total < 1e-6f) return pts;
    const int steps = closed ? count : count - 1;
    std::size_t seg = 0;
    for (int k = 0; k < count; ++k) {
        const float d = total * static_cast<float>(k) / static_cast<float>(steps);
        while (seg + 1 < acc.size() - 1 && acc[seg + 1] < d) ++seg;
        const float len = acc[seg + 1] - acc[seg];
        const float t = len > 1e-9f ? (d - acc[seg]) / len : 0.0f;
        out.push_back(glm::mix(pts[seg], pts[(seg + 1) % n], std::clamp(t, 0.0f, 1.0f)));
    }
    return out;
}

// A smooth line through the points (centripetal Catmull-Rom, the road's kind
// of curve, in 3D): `steps` samples per span.
inline std::vector<glm::vec3> smoothLine(const std::vector<glm::vec3>& p, bool closed, int steps) {
    const int n = static_cast<int>(p.size());
    if (n < 3 || steps < 2) return p;
    auto at = [&](int i) {
        if (closed) return p[static_cast<std::size_t>(((i % n) + n) % n)];
        if (i < 0) return p[0] * 2.0f - p[1];
        if (i >= n) return p[static_cast<std::size_t>(n - 1)] * 2.0f - p[static_cast<std::size_t>(n - 2)];
        return p[static_cast<std::size_t>(i)];
    };
    std::vector<glm::vec3> out;
    const int spans = closed ? n : n - 1;
    for (int s = 0; s < spans; ++s) {
        const glm::vec3 p0 = at(s - 1), p1 = at(s), p2 = at(s + 1), p3 = at(s + 2);
        auto knot = [](const glm::vec3& a, const glm::vec3& b) {
            return std::max(std::sqrt(glm::length(b - a)), 1e-4f);
        };
        const float t0 = 0.0f, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
        for (int k = 0; k < steps; ++k) {
            const float t = t1 + (t2 - t1) * static_cast<float>(k) / static_cast<float>(steps);
            const glm::vec3 a1 = (t1 - t) / (t1 - t0) * p0 + (t - t0) / (t1 - t0) * p1;
            const glm::vec3 a2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2;
            const glm::vec3 a3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3;
            const glm::vec3 b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2;
            const glm::vec3 b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3;
            out.push_back((t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2);
        }
    }
    if (!closed) out.push_back(p.back());
    return out;
}

// --- Welding: corners at one place as one point ------------------------------------
// A shape keeps separate corners across a crease (a box's edges, a tube's rim)
// so that shading stays sharp there. A step that moves points by where they
// ARE -- noise, smoothing, cutting facets -- has to move such twins as one, or
// the surface tears open along every crease. `group[i]` is the point corner i
// belongs to; every point has its place and a normal (of all the faces round
// it, by area) and, when asked for the links, its neighbours along the faces'
// edges, whether it lies on an open edge, and which piece -- connected part --
// it is in.
struct Welded {
    std::vector<int>              group;   // per corner
    std::vector<glm::vec3>        pos;     // per point
    std::vector<glm::vec3>        nrm;
    std::vector<std::vector<int>> next;    // per point (links)
    std::vector<char>             rim;     // per point (links)
    std::vector<int>              piece;   // per point (links)
    int                           pieces = 0;
};

inline Welded weld(const EditMesh& m, bool links) {
    struct Key {
        long long x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::uint64_t h = static_cast<std::uint64_t>(k.x) * 0x9E3779B97F4A7C15ull;
            h ^= static_cast<std::uint64_t>(k.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
            h ^= static_cast<std::uint64_t>(k.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
            return static_cast<std::size_t>(h);
        }
    };
    // A tenth of a millimetre: closer than that is the same place.
    constexpr double kQ = 1e4;
    Welded w;
    w.group.assign(m.verts.size(), -1);
    std::unordered_map<Key, int, KeyHash> at;
    at.reserve(m.verts.size());
    for (std::size_t i = 0; i < m.verts.size(); ++i) {
        const glm::vec3& p = m.verts[i];
        const Key k{std::llround(p.x * kQ), std::llround(p.y * kQ), std::llround(p.z * kQ)};
        auto [it, fresh] = at.try_emplace(k, static_cast<int>(w.pos.size()));
        if (fresh) w.pos.push_back(p);
        w.group[i] = it->second;
    }
    const std::size_t n = w.pos.size();
    w.nrm.assign(n, glm::vec3(0.0f));
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        if (!m.validFace(f)) continue;
        const glm::vec3 fn = m.faceNormal(f) * std::max(m.faceArea(f), 1e-8f);
        for (int v : m.faces[static_cast<std::size_t>(f)]) w.nrm[static_cast<std::size_t>(w.group[static_cast<std::size_t>(v)])] += fn;
    }
    for (glm::vec3& v : w.nrm) v = glm::dot(v, v) > 1e-16f ? glm::normalize(v) : glm::vec3(0.0f, 1.0f, 0.0f);
    if (!links) return w;
    w.next.assign(n, {});
    w.rim.assign(n, 0);
    std::unordered_map<std::uint64_t, int> uses;
    for (const std::vector<int>& f : m.faces) {
        if (f.size() < 2) continue;
        for (std::size_t i = 0; i < f.size(); ++i) {
            const int a = w.group[static_cast<std::size_t>(f[i])];
            const int b = w.group[static_cast<std::size_t>(f[(i + 1) % f.size()])];
            if (a == b) continue;
            w.next[static_cast<std::size_t>(a)].push_back(b);
            w.next[static_cast<std::size_t>(b)].push_back(a);
            ++uses[(static_cast<std::uint64_t>(std::min(a, b)) << 32) | static_cast<std::uint32_t>(std::max(a, b))];
        }
    }
    for (const auto& [k, c] : uses)
        if (c == 1) {
            w.rim[static_cast<std::size_t>(k >> 32)] = 1;
            w.rim[static_cast<std::size_t>(k & 0xFFFFFFFFull)] = 1;
        }
    for (std::vector<int>& nb : w.next) {
        std::sort(nb.begin(), nb.end());
        nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
    }
    // Pieces: a flood over the links.
    w.piece.assign(n, -1);
    std::vector<int> todo;
    for (std::size_t s = 0; s < n; ++s) {
        if (w.piece[s] >= 0) continue;
        const int id = w.pieces++;
        w.piece[s] = id;
        todo.assign(1, static_cast<int>(s));
        while (!todo.empty()) {
            const int v = todo.back();
            todo.pop_back();
            for (int u : w.next[static_cast<std::size_t>(v)])
                if (w.piece[static_cast<std::size_t>(u)] < 0) {
                    w.piece[static_cast<std::size_t>(u)] = id;
                    todo.push_back(u);
                }
        }
    }
    return w;
}

} // namespace proc::kit
