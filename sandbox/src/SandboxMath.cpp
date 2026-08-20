#include "SandboxMath.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>   // glm::rotate
#include <glm/gtc/quaternion.hpp>

#include "CameraPath.hpp" // catmull() -- the uniform fallback

float vhash2(float x, float z) {
    const float h = std::sin(x * 127.1f + z * 311.7f) * 43758.5453f;
    return h - std::floor(h);
}

float valNoise2(float x, float z) {
    const float xi = std::floor(x), zi = std::floor(z);
    const float xf = x - xi, zf = z - zi;
    const float u = xf * xf * (3.0f - 2.0f * xf);
    const float v = zf * zf * (3.0f - 2.0f * zf);
    const float a = vhash2(xi, zi),     b = vhash2(xi + 1.0f, zi);
    const float c = vhash2(xi, zi + 1.0f), d = vhash2(xi + 1.0f, zi + 1.0f);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

float roadDistanceSq(const std::vector<glm::vec2>& line, float x, float z) {
    if (line.size() < 2) return 1e30f;
    const glm::vec2 p(x, z);
    float best = 1e30f;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const glm::vec2 a = line[i], b = line[i + 1];
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        float t = len2 > 1e-8f ? glm::dot(p - a, ab) / len2 : 0.0f;
        t = glm::clamp(t, 0.0f, 1.0f);
        const glm::vec2 d = p - (a + ab * t);
        best = std::min(best, glm::dot(d, d));
    }
    return best;
}

float rayAABB(const glm::vec3& ro, const glm::vec3& rd,
              const glm::vec3& bmin, const glm::vec3& bmax) {
    float tmin = 0.0f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        const float inv = 1.0f / rd[i];
        float t1 = (bmin[i] - ro[i]) * inv;
        float t2 = (bmax[i] - ro[i]) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmax < tmin) return -1.0f;
    }
    return tmin;
}

float rayTriangle(const glm::vec3& ro, const glm::vec3& rd,
                  const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    // Moller-Trumbore, without the backface cull: a face is pickable from either
    // side, because a mesh being modelled is routinely open and often looked into.
    const glm::vec3 e1 = b - a, e2 = c - a;
    const glm::vec3 p  = glm::cross(rd, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-8f) return -1.0f;   // ray parallel to the triangle
    const float inv = 1.0f / det;
    const glm::vec3 tv = ro - a;
    const float u = glm::dot(tv, p) * inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const glm::vec3 q = glm::cross(tv, e1);
    const float v = glm::dot(rd, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    const float t = glm::dot(e2, q) * inv;
    return t > 1e-5f ? t : -1.0f;              // behind the eye: not a hit
}

// --- Splines -----------------------------------------------------------------
// Sampling density: aim for a sample every kSampleStep metres so a long sweeping
// span gets as many samples as it needs to read as a curve instead of a chain of
// facets, while a short one stays cheap. Clamped at both ends.
namespace {
constexpr float kSampleStep = 2.0f;
constexpr int   kMinSub     = 6;
constexpr int   kMaxSub     = 128;
} // namespace

glm::vec2 catmullCentripetal(const glm::vec2& p0, const glm::vec2& p1,
                             const glm::vec2& p2, const glm::vec2& p3, float t) {
    auto next = [](float ti, const glm::vec2& a, const glm::vec2& b) {
        return ti + std::sqrt(glm::length(b - a));
    };
    const float t0 = 0.0f;
    const float t1 = next(t0, p0, p1);
    const float t2 = next(t1, p1, p2);
    const float t3 = next(t2, p2, p3);
    if (t1 - t0 < 1e-5f || t2 - t1 < 1e-5f || t3 - t2 < 1e-5f)
        return catmull(p0, p1, p2, p3, t);
    const float tt = t1 + t * (t2 - t1);
    const glm::vec2 a1 = ((t1 - tt) * p0 + (tt - t0) * p1) / (t1 - t0);
    const glm::vec2 a2 = ((t2 - tt) * p1 + (tt - t1) * p2) / (t2 - t1);
    const glm::vec2 a3 = ((t3 - tt) * p2 + (tt - t2) * p3) / (t3 - t2);
    const glm::vec2 b1 = ((t2 - tt) * a1 + (tt - t0) * a2) / (t2 - t0);
    const glm::vec2 b2 = ((t3 - tt) * a2 + (tt - t1) * a3) / (t3 - t1);
    return ((t2 - tt) * b1 + (tt - t1) * b2) / (t2 - t1);
}

std::vector<glm::vec2> sampleSpline(const std::vector<glm::vec2>& pts, bool closed,
                                    std::vector<int>* ptSample) {
    std::vector<glm::vec2> line;
    if (ptSample) ptSample->clear();
    const int n = static_cast<int>(pts.size());
    if (n < 2) return line;
    // A closed loop needs >= 3 points to be more than a back-and-forth. When
    // looping, control points wrap around (modulo n) so the tangents are
    // continuous across the seam; the extra segment n-1 -> 0 closes the ring.
    const bool loop = closed && n >= 3;
    auto pt = [&](int i) -> glm::vec2 {
        if (loop) return pts[((i % n) + n) % n];
        // Open ends: mirror a phantom point through the endpoint instead of
        // repeating it. A repeated point has zero knot spacing (degenerate for
        // the centripetal form) and flattens the first/last span's tangent;
        // mirroring lets the line leave its end point along the curve it is on.
        if (i < 0)     return 2.0f * pts[0] - pts[1];
        if (i > n - 1) return 2.0f * pts[n - 1] - pts[n - 2];
        return pts[i];
    };
    const int segs = loop ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const glm::vec2 p0 = pt(i - 1);
        const glm::vec2 p1 = pt(i);
        const glm::vec2 p2 = pt(i + 1);
        const glm::vec2 p3 = pt(i + 2);
        // Sample count from this span's own length, so curvature is resolved the
        // same everywhere regardless of how far apart the user set the points.
        const int sub = std::clamp(
            static_cast<int>(std::lround(glm::length(p2 - p1) / kSampleStep)),
            kMinSub, kMaxSub);
        // Control point i is the span's first sample -- recorded for anything that
        // names a stretch by control-point index (a road's bridges, say).
        if (ptSample) ptSample->push_back(static_cast<int>(line.size()));
        // Each span drops its final sample (it repeats the next span's first);
        // only the very last segment keeps it, to terminate the open line or to
        // land back on the start point and close the loop.
        const int last = (i == segs - 1) ? sub : sub - 1;
        for (int s = 0; s <= last; ++s)
            line.push_back(catmullCentripetal(p0, p1, p2, p3,
                                              static_cast<float>(s) / sub));
    }
    // The line's final sample closes out the last control point (the loop's start
    // point, or the open line's end point).
    if (ptSample) ptSample->push_back(static_cast<int>(line.size()) - 1);
    return line;
}

glm::vec3 attitudeEuler(float yawDeg, float pitchDeg, float rollDeg) {
    // Nothing to convert when the craft is level: with no roll the two
    // conventions agree exactly, so a car, a loop's pitch and every scene saved
    // before this come through untouched.
    if (rollDeg == 0.0f) return glm::vec3(pitchDeg, yawDeg, 0.0f);

    // What we want, in the order an aircraft turns: heading, then nose, then
    // wings. Same order Unity's Euler angles use, for what it is worth.
    const glm::mat3 want =
        glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(yawDeg),   glm::vec3(0, 1, 0)) *
                  glm::rotate(glm::mat4(1.0f), glm::radians(pitchDeg), glm::vec3(1, 0, 0)) *
                  glm::rotate(glm::mat4(1.0f), glm::radians(rollDeg),  glm::vec3(0, 0, 1)));

    // ...expressed in the order the scene composes: Rz(z) * Ry(y) * Rx(x).
    // glm is column-major, so element [row][col] reads want[col][row].
    const float r20 = want[0][2];
    const float ey  = std::asin(std::clamp(-r20, -1.0f, 1.0f));
    float ex, ez;
    if (std::abs(r20) < 0.99999f) {
        ex = std::atan2(want[1][2], want[2][2]);
        ez = std::atan2(want[0][1], want[0][0]);
    } else {
        // Straight up or down: only (roll -+ yaw) is determined, so pick the
        // solution that puts all of it in the craft's own axis.
        ez = 0.0f;
        ex = std::atan2(-want[2][1], want[1][1]);
    }
    return glm::vec3(glm::degrees(ex), glm::degrees(ey), glm::degrees(ez));
}

float sceneHeading(const glm::vec3& eulerDeg) {
    // glm's quaternion-from-Euler happens to compose in exactly the order the
    // scene does (Rz * Ry * Rx -- verified against ImGuizmo's Recompose), so this
    // is the orientation the renderer will actually use.
    const glm::quat q(glm::radians(eulerDeg));
    const glm::vec3 nose = q * glm::vec3(0.0f, 0.0f, 1.0f);
    if (glm::length(glm::vec2(nose.x, nose.z)) > 1e-3f)
        return std::atan2(nose.x, nose.z);
    // Pointing straight up or down: read the heading off the wings instead, which
    // are still level in exactly that case.
    const glm::vec3 right = q * glm::vec3(1.0f, 0.0f, 0.0f);
    return std::atan2(-right.z, right.x);
}
