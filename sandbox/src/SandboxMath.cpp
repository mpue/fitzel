#include "SandboxMath.hpp"

#include <algorithm>
#include <cmath>

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
