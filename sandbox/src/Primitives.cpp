#include "Primitives.hpp"

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

using namespace fitzel;

// A small flower: a green stem (crossed quads, tint 0), a ring of cupped petals
// tinted per-instance (tint 1) and a yellow centre disc (tint 2). Petals tilt up
// and outward so the bloom reads as a flower from across the meadow, not a disc.
std::vector<float> makeFlowerMesh() {
    std::vector<float> v;
    auto push = [&](glm::vec3 p, glm::vec3 n, float t) {
        v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z, t});
    };
    auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, float t) {
        const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        push(a, n, t); push(b, n, t); push(c, n, t);
    };
    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n, float t) {
        push(a, n, t); push(b, n, t); push(c, n, t);
        push(a, n, t); push(c, n, t); push(d, n, t);
    };
    const float sh = 0.55f, hw = 0.018f;
    quad({-hw, 0, 0}, {hw, 0, 0}, {hw, sh, 0}, {-hw, sh, 0}, {0, 0, 1}, 0.0f);
    quad({0, 0, -hw}, {0, 0, hw}, {0, sh, hw}, {0, sh, -hw}, {1, 0, 0}, 0.0f);

    // Petals: rounded lobes, nearly flat and wide (buttercup/daisy look) so the
    // open bloom reads as a soft colour speck rather than a cupped star.
    const int   P = 6, ARC = 5;
    const float rb = 0.04f, rt = 0.30f, lift = 0.05f;
    const float PI = 3.14159265f;
    for (int i = 0; i < P; ++i) {
        const float am = static_cast<float>(i) / P * 6.2831853f;
        const float d  = (PI / P) * 1.2f; // wide, overlapping petals -> full bloom
        const glm::vec3 baseC(std::cos(am) * rb, sh, std::sin(am) * rb);
        glm::vec3 prev(0.0f);
        for (int k = 0; k <= ARC; ++k) {
            const float f = static_cast<float>(k) / ARC;   // 0..1 across the petal
            const float a = am - d + 2.0f * d * f;
            const float shape = std::sin(f * PI);          // round: 0 edges, 1 middle
            const float r = rb + (rt - rb) * shape;
            const float y = sh + lift * shape;
            const glm::vec3 p(std::cos(a) * r, y, std::sin(a) * r);
            if (k > 0) tri(baseC, prev, p, 1.0f);
            prev = p;
        }
    }
    // Small yellow centre disc, almost flush with the petals.
    const int   C = 10;
    const float rc = 0.08f;
    const glm::vec3 cc(0.0f, sh + 0.012f, 0.0f);
    for (int i = 0; i < C; ++i) {
        const float a0 = static_cast<float>(i) / C * 6.2831853f;
        const float a1 = static_cast<float>(i + 1) / C * 6.2831853f;
        const glm::vec3 p0(std::cos(a0) * rc, sh + 0.008f, std::sin(a0) * rc);
        const glm::vec3 p1(std::cos(a1) * rc, sh + 0.008f, std::sin(a1) * rc);
        tri(cc, p0, p1, 2.0f);
    }
    return v;
}

MeshData makeCylinderX(float r, float ht, int seg) {
    MeshData m;
    const float TAU = 6.28318530718f;
    for (int i = 0; i < seg; ++i) {
        const float a0 = static_cast<float>(i) / seg * TAU;
        const float a1 = static_cast<float>(i + 1) / seg * TAU;
        const glm::vec3 n0(0.0f, std::cos(a0), std::sin(a0));
        const glm::vec3 n1(0.0f, std::cos(a1), std::sin(a1));
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        m.vertices.push_back({{-ht, r * n0.y, r * n0.z}, n0, {0.0f, 0.0f}});
        m.vertices.push_back({{ ht, r * n0.y, r * n0.z}, n0, {0.0f, 1.0f}});
        m.vertices.push_back({{ ht, r * n1.y, r * n1.z}, n1, {1.0f, 1.0f}});
        m.vertices.push_back({{-ht, r * n1.y, r * n1.z}, n1, {1.0f, 0.0f}});
        m.indices.insert(m.indices.end(),
                         {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    for (int side = 0; side < 2; ++side) {
        const float x = side ? ht : -ht;
        const glm::vec3 nc(side ? 1.0f : -1.0f, 0.0f, 0.0f);
        const auto c = static_cast<std::uint32_t>(m.vertices.size());
        m.vertices.push_back({{x, 0.0f, 0.0f}, nc, {0.5f, 0.5f}});
        for (int i = 0; i <= seg; ++i) {
            const float a = static_cast<float>(i) / seg * TAU;
            m.vertices.push_back({{x, r * std::cos(a), r * std::sin(a)}, nc, {0.0f, 0.0f}});
        }
        for (int i = 0; i < seg; ++i) {
            const std::uint32_t a = c + 1 + i, b = c + 2 + i;
            if (side) m.indices.insert(m.indices.end(), {c, a, b});
            else      m.indices.insert(m.indices.end(), {c, b, a});
        }
    }
    return m;
}

std::vector<Vertex> makeRampVerts() {
    std::vector<Vertex> v;
    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n) {
        for (glm::vec3 p : {a, b, c, a, c, d}) v.push_back({p, n, {0, 0}});
        for (glm::vec3 p : {a, c, b, a, d, c}) v.push_back({p, -n, {0, 0}}); // back side
    };
    const float lo = -0.5f, hi = 0.5f;
    const glm::vec3 Al(lo, lo, lo), Bl(lo, lo, hi), Cl(lo, hi, hi);
    const glm::vec3 Ar(hi, lo, lo), Br(hi, lo, hi), Cr(hi, hi, hi);
    quad(Al, Ar, Br, Bl, {0, -1, 0});                                  // bottom
    quad(Bl, Br, Cr, Cl, {0, 0, 1});                                   // back wall
    quad(Al, Cl, Cr, Ar, glm::normalize(glm::vec3(0, 1, -1)));         // slope
    // triangular sides
    for (glm::vec3 p : {Al, Bl, Cl}) v.push_back({p, {-1, 0, 0}, {0, 0}});
    for (glm::vec3 p : {Al, Cl, Bl}) v.push_back({p, { 1, 0, 0}, {0, 0}});
    for (glm::vec3 p : {Ar, Cr, Br}) v.push_back({p, { 1, 0, 0}, {0, 0}});
    for (glm::vec3 p : {Ar, Br, Cr}) v.push_back({p, {-1, 0, 0}, {0, 0}});
    return v;
}

std::vector<Vertex> makeCylinderYVerts(int seg) {
    std::vector<Vertex> v;
    const float TAU = 6.28318530718f, r = 0.5f, hy = 0.5f;
    auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 n) {
        v.push_back({a, n, {0, 0}}); v.push_back({b, n, {0, 0}}); v.push_back({c, n, {0, 0}});
        v.push_back({a, -n, {0, 0}}); v.push_back({c, -n, {0, 0}}); v.push_back({b, -n, {0, 0}});
    };
    for (int i = 0; i < seg; ++i) {
        const float a0 = static_cast<float>(i) / seg * TAU;
        const float a1 = static_cast<float>(i + 1) / seg * TAU;
        const glm::vec3 n0(std::cos(a0), 0, std::sin(a0)), n1(std::cos(a1), 0, std::sin(a1));
        const glm::vec3 b0(r * n0.x, -hy, r * n0.z), t0(r * n0.x, hy, r * n0.z);
        const glm::vec3 b1(r * n1.x, -hy, r * n1.z), t1(r * n1.x, hy, r * n1.z);
        tri(b0, b1, t1, n0); tri(b0, t1, t0, n0);        // side
        tri(glm::vec3(0, hy, 0), t0, t1, {0, 1, 0});     // top cap
        tri(glm::vec3(0, -hy, 0), b1, b0, {0, -1, 0});   // bottom cap
    }
    return v;
}

std::vector<Vertex> makeSphereVerts(int stacks, int slices) {
    std::vector<Vertex> v;
    const float PI = 3.14159265358979f, TAU = 6.28318530718f, r = 0.5f;
    auto pt = [&](int st, int sl) -> Vertex {
        const float phi   = PI  * static_cast<float>(st) / stacks; // 0..PI, +Y down
        const float theta = TAU * static_cast<float>(sl) / slices;
        const glm::vec3 n(std::sin(phi) * std::cos(theta),
                          std::cos(phi),
                          std::sin(phi) * std::sin(theta));
        return { r * n, n, { static_cast<float>(sl) / slices,
                             static_cast<float>(st) / stacks } };
    };
    for (int st = 0; st < stacks; ++st) {
        for (int sl = 0; sl < slices; ++sl) {
            const Vertex a = pt(st,     sl),     d = pt(st,     sl + 1);
            const Vertex b = pt(st + 1, sl),     c = pt(st + 1, sl + 1);
            v.push_back(a); v.push_back(d); v.push_back(c);
            v.push_back(a); v.push_back(c); v.push_back(b);
        }
    }
    return v;
}
