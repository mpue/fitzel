#include "TraceShapes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pathtrace {
namespace {

constexpr float  kInf = std::numeric_limits<float>::infinity();
constexpr double kPiD = 3.14159265358979323846;
constexpr float  kPi  = 3.14159265358979323846f;

// How far past a rejected CSG crossing the walk resumes. Relative, so a scene
// built in kilometres does not find the same crossing again, and small, so two
// genuinely separate surfaces a hair apart are not jumped over together.
inline float resumeAfter(float t) { return t + 1e-5f * std::max(1.0f, std::fabs(t)); }

// The ray against an axis-aligned box: entry and exit distances, and the axes
// of the faces it enters and leaves through (with the sign in bit 2: set for
// the +face). False when the box is missed or lies outside (tMin, tMax).
bool slab(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& o, const glm::vec3& d,
          float tMin, float tMax, float& t0, float& t1, int& f0, int& f1) {
    t0 = -kInf;
    t1 = kInf;
    f0 = f1 = 0;
    for (int a = 0; a < 3; ++a) {
        if (d[a] == 0.0f) {
            if (o[a] < lo[a] || o[a] > hi[a]) return false;
            continue;
        }
        const float inv = 1.0f / d[a];
        float ta = (lo[a] - o[a]) * inv, tb = (hi[a] - o[a]) * inv;
        int fa = a, fb = a | 4;          // entering through lo, leaving through hi
        if (ta > tb) { std::swap(ta, tb); std::swap(fa, fb); }
        if (ta > t0) { t0 = ta; f0 = fa; }
        if (tb < t1) { t1 = tb; f1 = fb; }
        if (t0 > t1) return false;
    }
    return t1 > tMin && t0 < tMax;
}

bool slabHit(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& o, const glm::vec3& d,
             float tMin, float tMax) {
    float t0, t1;
    int f0, f1;
    return slab(lo, hi, o, d, tMin, tMax, t0, t1, f0, f1);
}

glm::vec3 faceNormal(int f) {
    glm::vec3 n(0.0f);
    n[f & 3] = (f & 4) ? 1.0f : -1.0f;
    return n;
}

// a t^2 + 2 b t + c = 0, in double: the half-b form, with the root that would
// cancel computed from the other one instead (Vieta). A float quadratic on a
// sphere seen from a hundred metres away loses most of its digits to exactly
// that cancellation, and the sphere's silhouette comes out ragged.
int quadratic(double a, double b, double c, double& r0, double& r1) {
    if (std::fabs(a) < 1e-14) {
        if (std::fabs(b) < 1e-14) return 0;
        r0 = -c / (2.0 * b);
        return 1;
    }
    const double disc = b * b - a * c;
    if (disc < 0.0) return 0;
    const double q = -(b + std::copysign(std::sqrt(disc), b));
    r0 = q / a;
    r1 = q != 0.0 ? c / q : r0;
    if (r0 > r1) std::swap(r0, r1);
    return 2;
}

// --- Quartic, for the torus ---------------------------------------------------
// Closed form after Schwarze (Graphics Gems I, "Cubic and quartic roots"),
// in double, with the roots polished by Newton on the original polynomial
// afterwards. The closed form alone loses a few digits in the resolvent cubic,
// which shows as speckle along the inner rim where two roots nearly meet.
inline bool nearZero(double x) { return std::fabs(x) < 1e-12; }

int solveQuadric(const double c[3], double s[2]) {
    const double p = c[1] / (2.0 * c[2]), q = c[0] / c[2];
    const double D = p * p - q;
    if (nearZero(D)) { s[0] = -p; return 1; }
    if (D < 0.0) return 0;
    const double r = std::sqrt(D);
    s[0] = r - p;
    s[1] = -r - p;
    return 2;
}

int solveCubic(const double c[4], double s[3]) {
    const double A = c[2] / c[3], B = c[1] / c[3], C = c[0] / c[3];
    const double sqA = A * A;
    const double p = (1.0 / 3.0) * (-(1.0 / 3.0) * sqA + B);
    const double q = 0.5 * ((2.0 / 27.0) * A * sqA - (1.0 / 3.0) * A * B + C);
    const double cbp = p * p * p;
    const double D = q * q + cbp;
    int num;
    if (nearZero(D)) {
        if (nearZero(q)) { s[0] = 0.0; num = 1; }
        else {
            const double u = std::cbrt(-q);
            s[0] = 2.0 * u;
            s[1] = -u;
            num = 2;
        }
    } else if (D < 0.0) {
        const double phi = (1.0 / 3.0) * std::acos(std::clamp(-q / std::sqrt(-cbp), -1.0, 1.0));
        const double t = 2.0 * std::sqrt(-p);
        s[0] = t * std::cos(phi);
        s[1] = -t * std::cos(phi + kPiD / 3.0);
        s[2] = -t * std::cos(phi - kPiD / 3.0);
        num = 3;
    } else {
        const double r = std::sqrt(D);
        s[0] = std::cbrt(r - q) - std::cbrt(r + q);
        num = 1;
    }
    const double sub = A / 3.0;
    for (int i = 0; i < num; ++i) s[i] -= sub;
    return num;
}

int solveQuartic(const double c[5], double s[4]) {
    const double A = c[3] / c[4], B = c[2] / c[4], C = c[1] / c[4], D = c[0] / c[4];
    const double sqA = A * A;
    const double p = -0.375 * sqA + B;
    const double q = 0.125 * sqA * A - 0.5 * A * B + C;
    const double r = -(3.0 / 256.0) * sqA * sqA + (1.0 / 16.0) * sqA * B - 0.25 * A * C + D;
    int num;
    double k[4];
    if (nearZero(r)) {
        k[0] = q; k[1] = p; k[2] = 0.0; k[3] = 1.0;
        num = solveCubic(k, s);
        s[num++] = 0.0;
    } else {
        k[0] = 0.5 * r * p - 0.125 * q * q;
        k[1] = -r;
        k[2] = -0.5 * p;
        k[3] = 1.0;
        double cs[3];
        solveCubic(k, cs);
        const double z = cs[0];
        double u = z * z - r, v = 2.0 * z - p;
        if (nearZero(u)) u = 0.0; else if (u > 0.0) u = std::sqrt(u); else return 0;
        if (nearZero(v)) v = 0.0; else if (v > 0.0) v = std::sqrt(v); else return 0;
        double qd[3] = {z - u, q < 0.0 ? -v : v, 1.0};
        num = solveQuadric(qd, s);
        qd[0] = z + u;
        qd[1] = q < 0.0 ? v : -v;
        num += solveQuadric(qd, s + num);
    }
    const double sub = 0.25 * A;
    for (int i = 0; i < num; ++i) s[i] -= sub;
    return num;
}

// --- The canonical primitives -------------------------------------------------
// Each finds its nearest crossing in (tMin, tMax) along a LOCAL ray, and says
// which part of itself it was (a box face, a cylinder cap) so the normal and uv
// can be worked out for the winner only.

struct LeafHit {
    float t   = kInf;
    int   part = 0;
};

inline void take(LeafHit& best, double t, int part, float tMin) {
    const float tf = static_cast<float>(t);
    if (tf > tMin && tf < best.t) { best.t = tf; best.part = part; }
}

void sphereHit(const glm::dvec3& o, const glm::dvec3& d, float tMin, LeafHit& best) {
    double r0, r1;
    const int n = quadratic(glm::dot(d, d), glm::dot(o, d), glm::dot(o, o) - 1.0, r0, r1);
    if (n >= 1) take(best, r0, 0, tMin);
    if (n >= 2) take(best, r1, 0, tMin);
}

void boxHit(const glm::vec3& o, const glm::vec3& d, float tMin, float tMax, LeafHit& best) {
    float t0, t1;
    int f0, f1;
    if (!slab(glm::vec3(-1.0f), glm::vec3(1.0f), o, d, tMin, tMax, t0, t1, f0, f1)) return;
    if (t0 > tMin && t0 < best.t) { best.t = t0; best.part = f0; }
    else if (t1 > tMin && t1 < best.t) { best.t = t1; best.part = f1; }
}

void planeHit(const glm::dvec3& o, const glm::dvec3& d, float tMin, LeafHit& best) {
    if (std::fabs(d.y) < 1e-14) return;
    take(best, -o.y / d.y, 0, tMin);
}

void discHit(const glm::dvec3& o, const glm::dvec3& d, double hole, float tMin, LeafHit& best) {
    if (std::fabs(d.y) < 1e-14) return;
    const double t = -o.y / d.y;
    const double x = o.x + d.x * t, z = o.z + d.z * t;
    const double r2 = x * x + z * z;
    if (r2 <= 1.0 && r2 >= hole * hole) take(best, t, 0, tMin);
}

// A cap of radius `r` in the plane y = `y`.
void capHit(const glm::dvec3& o, const glm::dvec3& d, double y, double r, int part, float tMin,
            LeafHit& best) {
    if (r <= 0.0 || std::fabs(d.y) < 1e-14) return;
    const double t = (y - o.y) / d.y;
    const double x = o.x + d.x * t, z = o.z + d.z * t;
    if (x * x + z * z <= r * r) take(best, t, part, tMin);
}

void cylinderHit(const glm::dvec3& o, const glm::dvec3& d, bool open, float tMin,
                 LeafHit& best) {
    double r0, r1;
    const int n = quadratic(d.x * d.x + d.z * d.z, o.x * d.x + o.z * d.z,
                            o.x * o.x + o.z * o.z - 1.0, r0, r1);
    for (int i = 0; i < n; ++i) {
        const double t = i == 0 ? r0 : r1;
        const double y = o.y + d.y * t;
        if (y >= 0.0 && y <= 1.0) take(best, t, 0, tMin);
    }
    if (!open) {
        capHit(o, d, 0.0, 1.0, 1, tMin, best);
        capHit(o, d, 1.0, 1.0, 2, tMin, best);
    }
}

void coneHit(const glm::dvec3& o, const glm::dvec3& d, double top, bool open, float tMin,
             LeafHit& best) {
    const double k  = 1.0 - top;
    const double R0 = 1.0 - k * o.y;
    double r0, r1;
    const int n = quadratic(d.x * d.x + d.z * d.z - k * k * d.y * d.y,
                            o.x * d.x + o.z * d.z + R0 * k * d.y,
                            o.x * o.x + o.z * o.z - R0 * R0, r0, r1);
    for (int i = 0; i < n; ++i) {
        const double t = i == 0 ? r0 : r1;
        const double y = o.y + d.y * t;
        if (y >= 0.0 && y <= 1.0) take(best, t, 0, tMin);
    }
    if (!open) {
        capHit(o, d, 0.0, 1.0, 1, tMin, best);
        capHit(o, d, 1.0, top, 2, tMin, best);
    }
}

void torusHit(const glm::dvec3& oIn, const glm::dvec3& dIn, double R, double r, float tMin,
              LeafHit& best) {
    // Conditioned before solving: a unit direction, and the origin moved to
    // the point of closest approach to the centre. Both shrink the
    // coefficients' range, which is what the closed form is sensitive to.
    const double dl = glm::length(dIn);
    if (dl < 1e-14) return;
    const glm::dvec3 d = dIn / dl;
    const double tc = -glm::dot(oIn, d);
    const glm::dvec3 o = oIn + d * tc;
    // Far enough out the ray cannot reach the torus at all.
    if (glm::dot(o, o) > (R + r) * (R + r) * 1.0001) return;

    const double R2 = R * R;
    const double f  = glm::dot(o, d);
    const double kk = glm::dot(o, o) + R2 - r * r;
    const double c[5] = {
        kk * kk - 4.0 * R2 * (o.x * o.x + o.z * o.z),
        4.0 * f * kk - 8.0 * R2 * (o.x * d.x + o.z * d.z),
        4.0 * f * f + 2.0 * kk - 4.0 * R2 * (d.x * d.x + d.z * d.z),
        4.0 * f,
        1.0,
    };
    double s[4];
    const int n = solveQuartic(c, s);
    for (int i = 0; i < n; ++i) {
        double x = s[i];
        for (int it = 0; it < 3; ++it) {
            const double fx  = (((c[4] * x + c[3]) * x + c[2]) * x + c[1]) * x + c[0];
            const double dfx = ((4.0 * c[4] * x + 3.0 * c[3]) * x + 2.0 * c[2]) * x + c[1];
            if (std::fabs(dfx) < 1e-14) break;
            x -= fx / dfx;
        }
        take(best, (x + tc) / dl, 0, tMin);
    }
}

// u around the y axis, [0, 1).
inline float aroundY(const glm::vec3& p) {
    return 0.5f + std::atan2(p.z, p.x) / (2.0f * kPi);
}

// The normal (local, unnormalised) and uv of a primitive at a local point.
void surfaceOf(const ShapeNode& s, const glm::vec3& p, int part, glm::vec3& n, glm::vec2& uv) {
    using K = ShapeNode::Kind;
    switch (s.kind) {
    case K::Sphere:
        n  = p;
        uv = glm::vec2(aroundY(p), 0.5f + std::asin(glm::clamp(p.y, -1.0f, 1.0f)) / kPi);
        break;
    case K::Box: {
        n = faceNormal(part);
        const int a = part & 3;
        const int u = a == 0 ? 2 : 0, v = a == 1 ? 2 : 1;
        uv = glm::vec2(p[u] * 0.5f + 0.5f, p[v] * 0.5f + 0.5f);
        break;
    }
    case K::Plane:
        n  = glm::vec3(0.0f, 1.0f, 0.0f);
        uv = glm::vec2(p.x, p.z);
        break;
    case K::Disc:
        n  = glm::vec3(0.0f, 1.0f, 0.0f);
        uv = glm::vec2(p.x * 0.5f + 0.5f, p.z * 0.5f + 0.5f);
        break;
    case K::Cylinder:
    case K::Cone:
        if (part == 1)      { n = glm::vec3(0.0f, -1.0f, 0.0f); uv = glm::vec2(p.x, p.z) * 0.5f + 0.5f; }
        else if (part == 2) { n = glm::vec3(0.0f,  1.0f, 0.0f); uv = glm::vec2(p.x, p.z) * 0.5f + 0.5f; }
        else {
            if (s.kind == K::Cylinder) n = glm::vec3(p.x, 0.0f, p.z);
            else {
                const float k = 1.0f - s.p0;
                n = glm::vec3(p.x, (1.0f - k * p.y) * k, p.z);
            }
            uv = glm::vec2(aroundY(p), p.y);
        }
        break;
    case K::Torus: {
        const float R = s.p0, r = s.p1;
        const float g = glm::dot(p, p) - R * R - r * r;
        n = p * g + glm::vec3(0.0f, 2.0f * R * R * p.y, 0.0f);
        // The tube's own angle: where around the minor circle the point is.
        const float rho = std::sqrt(p.x * p.x + p.z * p.z) - R;
        uv = glm::vec2(aroundY(p), 0.5f + std::atan2(p.y, rho) / (2.0f * kPi));
        break;
    }
    default:
        n  = p;
        uv = glm::vec2(0.0f);
        break;
    }
}

bool leafInside(const ShapeNode& s, const glm::vec3& p) {
    using K = ShapeNode::Kind;
    switch (s.kind) {
    case K::Sphere:   return glm::dot(p, p) < 1.0f;
    case K::Box:      return std::fabs(p.x) < 1.0f && std::fabs(p.y) < 1.0f && std::fabs(p.z) < 1.0f;
    case K::Plane:    return p.y < 0.0f;
    case K::Disc:     return false;
    case K::Cylinder: return p.y > 0.0f && p.y < 1.0f && p.x * p.x + p.z * p.z < 1.0f;
    case K::Cone: {
        if (p.y <= 0.0f || p.y >= 1.0f) return false;
        const float R = 1.0f - (1.0f - s.p0) * p.y;
        return p.x * p.x + p.z * p.z < R * R;
    }
    case K::Torus: {
        const float R = s.p0, r = s.p1;
        const float g = glm::dot(p, p) + R * R - r * r;
        return g * g - 4.0f * R * R * (p.x * p.x + p.z * p.z) < 0.0f;
    }
    case K::Iso:
        if (p.x <= s.boxLo.x || p.y <= s.boxLo.y || p.z <= s.boxLo.z ||
            p.x >= s.boxHi.x || p.y >= s.boxHi.y || p.z >= s.boxHi.z)
            return false;
        return s.func && s.func->scalar(p) - s.p0 < 0.0f;
    default:
        return false;
    }
}

// The field's gradient, by central differences: the isosurface's normal.
glm::vec3 isoGradient(const ShapeNode& s, const glm::vec3& p) {
    const float h = std::max(s.p2, 1e-5f);
    const Func& f = *s.func;
    return glm::vec3(f.scalar(p + glm::vec3(h, 0, 0)) - f.scalar(p - glm::vec3(h, 0, 0)),
                     f.scalar(p + glm::vec3(0, h, 0)) - f.scalar(p - glm::vec3(0, h, 0)),
                     f.scalar(p + glm::vec3(0, 0, h)) - f.scalar(p - glm::vec3(0, 0, h)));
}

// An isosurface: the surface where the field crosses the threshold, inside its
// box -- and, unless it is `open`, the box's own faces wherever the solid
// reaches them, so a field cut off by its box is a closed solid rather than a
// shell with its inside showing.
//
// Marched with steps the field's own value says are safe: a field that changes
// by at most `maxGradient` per unit cannot reach the threshold sooner than
// |f| / maxGradient away. A sign change between two steps is then narrowed down
// by bisection. Too small a maxGradient steps over thin parts (holes in the
// surface); the scene builder estimates it by sampling when none is given.
bool isoHit(const ShapeNode& s, const glm::vec3& o, const glm::vec3& d, float tMin, float tMax,
            float& tHit, glm::vec3& n) {
    if (!s.func) return false;
    float t0, t1;
    int f0, f1;
    if (!slab(s.boxLo, s.boxHi, o, d, tMin, tMax, t0, t1, f0, f1)) return false;
    const float a = std::max(t0, tMin), b = std::min(t1, tMax);
    if (a >= b) return false;
    const float dlen = glm::length(d);
    if (dlen < 1e-20f) return false;
    const Func& F = *s.func;
    const float th = s.p0;
    const float L  = std::max(s.p1, 1e-3f);
    const float acc = std::max(s.p2, 1e-5f);
    const auto field = [&](float t) { return F.scalar(o + d * t) - th; };

    float ft = field(a);
    if (!s.open && t0 > tMin && ft < 0.0f) {
        tHit = t0;
        n = faceNormal(f0);
        return true;
    }
    const float minStep = acc / dlen;
    float t = a;
    for (int step = 0; step < 20000 && t < b; ++step) {
        const float dt = std::max(std::fabs(ft) / (L * dlen), minStep);
        const float tn = std::min(t + dt, b);
        const float fn = field(tn);
        if ((ft < 0.0f) != (fn < 0.0f)) {
            float lo = t, hi = tn;
            const bool loNeg = ft < 0.0f;
            for (int i = 0; i < 32 && (hi - lo) * dlen > acc * 0.01f; ++i) {
                const float mid = 0.5f * (lo + hi);
                if ((field(mid) < 0.0f) == loNeg) lo = mid;
                else                              hi = mid;
            }
            tHit = 0.5f * (lo + hi);
            if (!(tHit > tMin)) tHit = hi;
            n = isoGradient(s, o + d * tHit);
            if (glm::dot(n, n) < 1e-30f) n = -d;
            return true;
        }
        t  = tn;
        ft = fn;
    }
    if (!s.open && t1 < tMax && ft < 0.0f && t >= b) {
        tHit = t1;
        n = faceNormal(f1);
        return true;
    }
    return false;
}

bool leafClosest(const ShapeNode& s, int index, const glm::vec3& oW, const glm::vec3& dW,
                 float tMin, float tMax, ShapeHit& hit) {
    using K = ShapeNode::Kind;
    const glm::vec3 o = glm::vec3(s.toLocal * glm::vec4(oW, 1.0f));
    const glm::vec3 d = glm::mat3(s.toLocal) * dW;

    glm::vec3 nl;
    glm::vec2 uv(0.0f);
    float t;
    if (s.kind == K::Iso) {
        if (!isoHit(s, o, d, tMin, tMax, t, nl)) return false;
        const glm::vec3 p  = o + d * t;
        const glm::vec3 c  = 0.5f * (s.boxLo + s.boxHi);
        const glm::vec3 q  = p - c;
        const float     ql = glm::length(q);
        uv = ql > 0.0f ? glm::vec2(aroundY(q), 0.5f + std::asin(glm::clamp(q.y / ql, -1.0f, 1.0f)) / kPi)
                       : glm::vec2(0.0f);
    } else {
        LeafHit best;
        best.t = tMax;
        const glm::dvec3 od(o), dd(d);
        switch (s.kind) {
        case K::Sphere:   sphereHit(od, dd, tMin, best); break;
        case K::Box:      boxHit(o, d, tMin, tMax, best); break;
        case K::Plane:    planeHit(od, dd, tMin, best); break;
        case K::Disc:     discHit(od, dd, s.p0, tMin, best); break;
        case K::Cylinder: cylinderHit(od, dd, s.open, tMin, best); break;
        case K::Cone:     coneHit(od, dd, s.p0, s.open, tMin, best); break;
        case K::Torus:    torusHit(od, dd, s.p0, s.p1, tMin, best); break;
        default: break;
        }
        if (!(best.t < tMax)) return false;
        t = best.t;
        surfaceOf(s, o + d * t, best.part, nl, uv);
    }
    glm::vec3 n = s.normalToWorld * nl;
    const float nn = glm::dot(n, n);
    n = nn > 1e-30f ? n / std::sqrt(nn) : -glm::normalize(dW);
    hit.t    = t;
    hit.n    = n;
    hit.uv   = uv;
    hit.leaf = index;
    return true;
}

// The canonical primitive's box, in its local space.
bool localBounds(const ShapeNode& s, glm::vec3& lo, glm::vec3& hi) {
    using K = ShapeNode::Kind;
    switch (s.kind) {
    case K::Sphere:
    case K::Box:      lo = glm::vec3(-1.0f); hi = glm::vec3(1.0f); return true;
    case K::Plane:    return false;
    case K::Disc:     lo = glm::vec3(-1.0f, -1e-4f, -1.0f); hi = glm::vec3(1.0f, 1e-4f, 1.0f); return true;
    case K::Cylinder: lo = glm::vec3(-1.0f, 0.0f, -1.0f); hi = glm::vec3(1.0f, 1.0f, 1.0f); return true;
    case K::Cone: {
        const float r = std::max(1.0f, s.p0);
        lo = glm::vec3(-r, 0.0f, -r);
        hi = glm::vec3(r, 1.0f, r);
        return true;
    }
    case K::Torus: {
        const float a = s.p0 + s.p1;
        lo = glm::vec3(-a, -s.p1, -a);
        hi = glm::vec3(a, s.p1, a);
        return true;
    }
    case K::Iso:      lo = s.boxLo; hi = s.boxHi; return true;
    default:          return false;
    }
}

} // namespace

void Shapes::finalize() {
    using K = ShapeNode::Kind;
    // Operands are always built before the node that combines them, so one
    // pass in order sees every child's bounds before its parent needs them.
    for (ShapeNode& s : nodes) {
        if (s.isLeaf()) {
            glm::vec3 lo, hi;
            s.bounded = localBounds(s, lo, hi);
            if (!s.bounded) continue;
            const glm::mat4 toWorld = glm::inverse(s.toLocal);
            s.lo = glm::vec3(kInf);
            s.hi = glm::vec3(-kInf);
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 p((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y,
                                  (c & 4) ? hi.z : lo.z);
                const glm::vec3 w = glm::vec3(toWorld * glm::vec4(p, 1.0f));
                s.lo = glm::min(s.lo, w);
                s.hi = glm::max(s.hi, w);
            }
            // A sliver of padding: a disc or a face-on plane piece is a box
            // of zero thickness, which the slab test treats inconsistently.
            const glm::vec3 pad = (s.hi - s.lo) * 1e-5f + 1e-6f;
            s.lo -= pad;
            s.hi += pad;
            continue;
        }
        const int* ch = childIndex.data() + s.first;
        if (s.count == 0) { s.bounded = true; s.lo = glm::vec3(0.0f); s.hi = glm::vec3(0.0f); continue; }
        switch (s.kind) {
        case K::Union:
        case K::Merge:
            s.bounded = true;
            s.lo = glm::vec3(kInf);
            s.hi = glm::vec3(-kInf);
            for (int i = 0; i < s.count; ++i) {
                const ShapeNode& c = nodes[static_cast<std::size_t>(ch[i])];
                if (!c.bounded) { s.bounded = false; break; }
                s.lo = glm::min(s.lo, c.lo);
                s.hi = glm::max(s.hi, c.hi);
            }
            break;
        case K::Intersection: {
            s.bounded = false;
            for (int i = 0; i < s.count; ++i) {
                const ShapeNode& c = nodes[static_cast<std::size_t>(ch[i])];
                if (!c.bounded) continue;
                if (!s.bounded) { s.lo = c.lo; s.hi = c.hi; s.bounded = true; }
                else { s.lo = glm::max(s.lo, c.lo); s.hi = glm::min(s.hi, c.hi); }
            }
            if (s.bounded) s.hi = glm::max(s.hi, s.lo);
            break;
        }
        case K::Difference: {
            const ShapeNode& c = nodes[static_cast<std::size_t>(ch[0])];
            s.bounded = c.bounded;
            s.lo = c.lo;
            s.hi = c.hi;
            break;
        }
        default: break;
        }
    }
}

bool Shapes::inside(int ni, const glm::vec3& p) const {
    using K = ShapeNode::Kind;
    const ShapeNode& s = nodes[static_cast<std::size_t>(ni)];
    if (s.bounded && (p.x < s.lo.x || p.y < s.lo.y || p.z < s.lo.z ||
                      p.x > s.hi.x || p.y > s.hi.y || p.z > s.hi.z))
        return false;
    if (s.isLeaf()) return leafInside(s, glm::vec3(s.toLocal * glm::vec4(p, 1.0f)));
    const int* ch = childIndex.data() + s.first;
    switch (s.kind) {
    case K::Union:
    case K::Merge:
        for (int i = 0; i < s.count; ++i) if (inside(ch[i], p)) return true;
        return false;
    case K::Intersection:
        for (int i = 0; i < s.count; ++i) if (!inside(ch[i], p)) return false;
        return s.count > 0;
    case K::Difference:
        if (s.count == 0 || !inside(ch[0], p)) return false;
        for (int i = 1; i < s.count; ++i) if (inside(ch[i], p)) return false;
        return true;
    default:
        return false;
    }
}

bool Shapes::closest(int ni, const glm::vec3& o, const glm::vec3& d, float tMin, float tMax,
                     ShapeHit& hit) const {
    using K = ShapeNode::Kind;
    const ShapeNode& s = nodes[static_cast<std::size_t>(ni)];
    if (s.bounded && !slabHit(s.lo, s.hi, o, d, tMin, tMax)) return false;
    if (s.isLeaf()) return leafClosest(s, ni, o, d, tMin, tMax, hit);

    const int* ch = childIndex.data() + s.first;
    if (s.kind == K::Union) {
        bool any = false;
        for (int i = 0; i < s.count; ++i) {
            ShapeHit h;
            if (closest(ch[i], o, d, tMin, any ? hit.t : tMax, h)) { hit = h; any = true; }
        }
        return any;
    }

    // Each operand's next crossing, kept between rounds: when a candidate is
    // rejected only the operand it came from has to look further, and an
    // isosurface operand is not marched again from scratch every time some
    // other operand's crossing turns out to be inside it.
    constexpr int kCache = 32;
    ShapeHit next[kCache];
    std::uint8_t state[kCache];   // 0 not asked yet, 1 next[i] valid, 2 nothing further
    const bool cached = s.count <= kCache;
    if (cached) for (int i = 0; i < s.count; ++i) state[i] = 0;

    float from = tMin;
    for (int iter = 0; iter < 1024; ++iter) {
        ShapeHit best;
        int which = -1;
        for (int i = 0; i < s.count; ++i) {
            if (cached) {
                if (state[i] == 1 && !(next[i].t > from)) state[i] = 0;
                if (state[i] == 0)
                    state[i] = closest(ch[i], o, d, from, tMax, next[i]) ? 1 : 2;
                if (state[i] == 1 && (which < 0 || next[i].t < best.t)) { best = next[i]; which = i; }
                continue;
            }
            ShapeHit h;
            if (closest(ch[i], o, d, from, which >= 0 ? best.t : tMax, h)) { best = h; which = i; }
        }
        if (which < 0) return false;
        if (cached) state[which] = 0;
        const glm::vec3 P = o + d * best.t;
        bool ok = true, flip = false;
        switch (s.kind) {
        case K::Merge:
            for (int j = 0; j < s.count && ok; ++j)
                if (j != which && inside(ch[j], P)) ok = false;
            break;
        case K::Intersection:
            for (int j = 0; j < s.count && ok; ++j)
                if (j != which && !inside(ch[j], P)) ok = false;
            break;
        case K::Difference:
            if (which == 0) {
                for (int j = 1; j < s.count && ok; ++j)
                    if (inside(ch[j], P)) ok = false;
            } else {
                ok = inside(ch[0], P);
                for (int j = 1; j < s.count && ok; ++j)
                    if (j != which && inside(ch[j], P)) ok = false;
                flip = true;
            }
            break;
        default: break;
        }
        if (ok) {
            hit = best;
            if (flip) hit.n = -hit.n;
            return true;
        }
        from = resumeAfter(best.t);
        if (!(from < tMax)) return false;
    }
    return false;
}

} // namespace pathtrace
