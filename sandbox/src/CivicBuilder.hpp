#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>

#include "CityGen.hpp"    // city::Piece (the colliders)
#include "CivicGen.hpp"   // civic::Model

// The geometry kit the town's generated things are built with: CivicGen's
// public buildings, and the small things of a street (StreetProps). Faces in
// per material, in the object's own frame, flat-shaded; finish() hands over a
// civic::Model.
namespace civic {

constexpr float kBuildPi = 3.14159265358979323846f;

// --- Geometry --------------------------------------------------------------------
// Faces go in per material, in the plot's frame. Every face is a convex polygon
// wound to face `outward` (CCW seen from outside is the engine's front face),
// flat-shaded, with a planar UV off its dominant axis -- the civic materials are
// colours, and a texture put on one later tiles at four metres a repeat.
struct Builder {
    std::map<fitzel::AssetId, fitzel::MeshData> parts;
    std::vector<city::Piece> solids;
    float top = 0.0f;

    void face(std::vector<glm::vec3> p, glm::vec3 outward, fitzel::AssetId m) {
        if (p.size() < 3 || !m.valid()) return;
        glm::vec3 n(0.0f);
        for (std::size_t i = 0; i < p.size(); ++i) {
            const glm::vec3& a = p[i];
            const glm::vec3& b = p[(i + 1) % p.size()];
            n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x),
                           (a.x - b.x) * (a.y + b.y));
        }
        if (glm::dot(n, n) < 1e-12f) return;
        if (glm::dot(n, outward) < 0.0f) { std::reverse(p.begin(), p.end()); n = -n; }
        n = glm::normalize(n);
        const glm::vec3 an = glm::abs(n);
        fitzel::MeshData& md = parts[m];
        const auto base = static_cast<std::uint32_t>(md.vertices.size());
        for (const glm::vec3& v : p) {
            fitzel::Vertex vx{};
            vx.position = v;
            vx.normal   = n;
            vx.uv = (an.y >= an.x && an.y >= an.z) ? glm::vec2(v.x, v.z) * 0.25f
                  : (an.x >= an.z)                 ? glm::vec2(v.z, v.y) * 0.25f
                                                   : glm::vec2(v.x, v.y) * 0.25f;
            md.vertices.push_back(vx);
            top = std::max(top, v.y);
        }
        for (std::uint32_t i = 1; i + 1 < p.size(); ++i) {
            md.indices.push_back(base);
            md.indices.push_back(base + i);
            md.indices.push_back(base + i + 1);
        }
    }

    // An axis-aligned box (no bottom face: everything here stands on something).
    void box(float x0, float x1, float y0, float y1, float z0, float z1, fitzel::AssetId m,
             bool solid = false) {
        if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
        face({{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}, {1, 0, 0}, m);
        face({{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}}, {-1, 0, 0}, m);
        face({{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}}, {0, 0, 1}, m);
        face({{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}, {0, 0, -1}, m);
        face({{x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}}, {0, 1, 0}, m);
        if (solid) this->solid(x0, x1, y0, y1, z0, z1);
    }
    void solid(float x0, float x1, float y0, float y1, float z0, float z1) {
        city::Piece pc;
        pc.center  = {0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1)};
        pc.half    = {0.5f * (x1 - x0), 0.5f * (y1 - y0), 0.5f * (z1 - z0)};
        pc.collide = true;   // no material: it collides and draws nothing
        solids.push_back(pc);
    }

    // A pitched roof over [x0,x1] x [z0,z1] from the eaves at y0 to the ridge at
    // y1, the ridge along Z (alongZ) or along X, `over` metres of overhang. The
    // two gable triangles are walls (`wall`); the slopes get an underside too, so
    // the overhang has a soffit when you stand under it.
    void gable(float x0, float x1, float z0, float z1, float y0, float y1, bool alongZ,
               fitzel::AssetId roof, fitzel::AssetId wall, float over = 0.4f) {
        if (alongZ) {
            const float xm = 0.5f * (x0 + x1);
            const float drop = over * (y1 - y0) / std::max(0.5f * (x1 - x0), 0.1f);
            const float za = z0 - over, zb = z1 + over;
            const glm::vec3 lA{x0 - over, y0 - drop, za}, lB{x0 - over, y0 - drop, zb};
            const glm::vec3 rA{x1 + over, y0 - drop, za}, rB{x1 + over, y0 - drop, zb};
            const glm::vec3 tA{xm, y1, za}, tB{xm, y1, zb};
            const glm::vec3 nl = glm::normalize(glm::vec3(-(y1 - y0), 0.5f * (x1 - x0), 0.0f));
            const glm::vec3 nr = glm::vec3(-nl.x, nl.y, 0.0f);
            face({lA, lB, tB, tA}, nl, roof);
            face({rA, tA, tB, rB}, nr, roof);
            face({lA, tA, tB, lB}, -nl, roof);
            face({rA, rB, tB, tA}, -nr, roof);
            face({{x0, y0, z0}, {xm, y1, z0}, {x1, y0, z0}}, {0, 0, -1}, wall);
            face({{x0, y0, z1}, {x1, y0, z1}, {xm, y1, z1}}, {0, 0, 1}, wall);
        } else {
            const float zm = 0.5f * (z0 + z1);
            const float drop = over * (y1 - y0) / std::max(0.5f * (z1 - z0), 0.1f);
            const float xa = x0 - over, xb = x1 + over;
            const glm::vec3 fA{xa, y0 - drop, z0 - over}, fB{xb, y0 - drop, z0 - over};
            const glm::vec3 bA{xa, y0 - drop, z1 + over}, bB{xb, y0 - drop, z1 + over};
            const glm::vec3 tA{xa, y1, zm}, tB{xb, y1, zm};
            const glm::vec3 nf = glm::normalize(glm::vec3(0.0f, 0.5f * (z1 - z0), -(y1 - y0)));
            const glm::vec3 nb = glm::vec3(0.0f, nf.y, -nf.z);
            face({fA, fB, tB, tA}, nf, roof);
            face({bA, tA, tB, bB}, nb, roof);
            face({fA, tA, tB, fB}, -nf, roof);
            face({bA, bB, tB, tA}, -nb, roof);
            face({{x0, y0, z0}, {x0, y1, zm}, {x0, y0, z1}}, {-1, 0, 0}, wall);
            face({{x1, y0, z0}, {x1, y0, z1}, {x1, y1, zm}}, {1, 0, 0}, wall);
        }
    }

    void pyramid(float cx, float cz, float hx, float hz, float y0, float y1, fitzel::AssetId m) {
        const glm::vec3 a{cx - hx, y0, cz - hz}, b{cx + hx, y0, cz - hz};
        const glm::vec3 c{cx + hx, y0, cz + hz}, d{cx - hx, y0, cz + hz};
        const glm::vec3 t{cx, y1, cz};
        face({a, b, t}, {0, hz, -(y1 - y0)}, m);
        face({b, c, t}, {(y1 - y0), hz, 0}, m);
        face({c, d, t}, {0, hz, (y1 - y0)}, m);
        face({d, a, t}, {-(y1 - y0), hz, 0}, m);
    }

    void cylinder(float cx, float cz, float r, float y0, float y1, fitzel::AssetId m,
                  int seg = 16, bool cap = true, bool solid = false) {
        std::vector<glm::vec3> ring;
        for (int i = 0; i < seg; ++i) {
            const float a = 2.0f * kBuildPi * i / seg;
            ring.push_back({cx + r * std::cos(a), 0.0f, cz + r * std::sin(a)});
        }
        for (int i = 0; i < seg; ++i) {
            const glm::vec3 p = ring[i], q = ring[(i + 1) % seg];
            const glm::vec3 mid = 0.5f * (p + q) - glm::vec3(cx, 0.0f, cz);
            face({{p.x, y0, p.z}, {q.x, y0, q.z}, {q.x, y1, q.z}, {p.x, y1, p.z}},
                 glm::vec3(mid.x, 0.0f, mid.z), m);
        }
        if (cap) {
            std::vector<glm::vec3> c;
            for (const glm::vec3& p : ring) c.push_back({p.x, y1, p.z});
            face(c, {0, 1, 0}, m);
        }
        if (solid) this->solid(cx - r * 0.8f, cx + r * 0.8f, y0, y1, cz - r * 0.8f, cz + r * 0.8f);
    }

    void cone(float cx, float cz, float r, float y0, float y1, fitzel::AssetId m, int seg = 16) {
        for (int i = 0; i < seg; ++i) {
            const float a = 2.0f * kBuildPi * i / seg, b = 2.0f * kBuildPi * (i + 1) / seg;
            const glm::vec3 p{cx + r * std::cos(a), y0, cz + r * std::sin(a)};
            const glm::vec3 q{cx + r * std::cos(b), y0, cz + r * std::sin(b)};
            const float am = 0.5f * (a + b);
            face({p, q, {cx, y1, cz}}, {std::cos(am) * (y1 - y0), r, std::sin(am) * (y1 - y0)}, m);
        }
    }

    void dome(float cx, float cz, float r, float y0, fitzel::AssetId m, int seg = 16, int rings = 5) {
        for (int j = 0; j < rings; ++j) {
            const float t0 = 0.5f * kBuildPi * j / rings, t1 = 0.5f * kBuildPi * (j + 1) / rings;
            for (int i = 0; i < seg; ++i) {
                const float a = 2.0f * kBuildPi * i / seg, b = 2.0f * kBuildPi * (i + 1) / seg;
                auto P = [&](float ang, float t) {
                    return glm::vec3(cx + r * std::cos(t) * std::cos(ang), y0 + r * std::sin(t),
                                     cz + r * std::cos(t) * std::sin(ang));
                };
                const glm::vec3 c = 0.25f * (P(a, t0) + P(b, t0) + P(a, t1) + P(b, t1));
                const glm::vec3 out = c - glm::vec3(cx, y0, cz);
                if (j + 1 == rings) face({P(a, t0), P(b, t0), {cx, y0 + r, cz}}, out, m);
                else                face({P(a, t0), P(b, t0), P(b, t1), P(a, t1)}, out, m);
            }
        }
    }

    // A parked vehicle: body, cabin, and a stripe down both flanks. Along X.
    void vehicle(float cx, float cz, float len, float wid, float h, fitzel::AssetId body,
                 fitzel::AssetId stripe, fitzel::AssetId glass, bool along = true) {
        const float hl = 0.5f * len, hw = 0.5f * wid;
        auto B = [&](float a0, float a1, float y0, float y1, float b0, float b1, fitzel::AssetId m) {
            if (along) box(cx + a0, cx + a1, y0, y1, cz + b0, cz + b1, m);
            else       box(cx + b0, cx + b1, y0, y1, cz + a0, cz + a1, m);
        };
        B(-hl, hl, 0.35f, h * 0.62f, -hw, hw, body);
        B(-hl * 0.45f, hl * 0.55f, h * 0.62f, h, -hw * 0.9f, hw * 0.9f, glass);
        B(-hl + 0.02f, hl - 0.02f, h * 0.36f, h * 0.48f, -hw - 0.03f, hw + 0.03f, stripe);
        B(-hl * 0.7f, -hl * 0.4f, 0.0f, 0.4f, -hw + 0.05f, hw - 0.05f, glass);   // wheels
        B(hl * 0.4f, hl * 0.7f, 0.0f, 0.4f, -hw + 0.05f, hw - 0.05f, glass);
    }

    // A round frustum, open at both ends (a cooling tower's shell is built
    // from a stack of these).
    void frustum(float cx, float cz, float r0, float r1, float y0, float y1, fitzel::AssetId m, int seg = 20) {
        for (int i = 0; i < seg; ++i) {
            const float a0 = 2.0f * kBuildPi * i / seg, a1 = 2.0f * kBuildPi * (i + 1) / seg;
            const glm::vec3 d0(std::cos(a0), 0.0f, std::sin(a0)), d1(std::cos(a1), 0.0f, std::sin(a1));
            const glm::vec3 c(cx, 0.0f, cz);
            const glm::vec3 p00 = c + d0 * r0 + glm::vec3(0, y0, 0), p10 = c + d1 * r0 + glm::vec3(0, y0, 0);
            const glm::vec3 p01 = c + d0 * r1 + glm::vec3(0, y1, 0), p11 = c + d1 * r1 + glm::vec3(0, y1, 0);
            const glm::vec3 out = glm::normalize(d0 + d1);
            face({p00, p10, p11, p01}, out, m);
            face({p00, p01, p11, p10}, -out, m);   // the inside, seen from the top
        }
    }
    // A truncated pyramid: a rectangle at y0 shrinking by `inset` to its top at y1.
    void mound(float x0, float x1, float z0, float z1, float inset, float y0, float y1, fitzel::AssetId side,
               fitzel::AssetId top) {
        const glm::vec3 b[4] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}};
        const glm::vec3 t[4] = {{x0 + inset, y1, z0 + inset}, {x1 - inset, y1, z0 + inset},
                                {x1 - inset, y1, z1 - inset}, {x0 + inset, y1, z1 - inset}};
        const glm::vec3 c(0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1));
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            const glm::vec3 mid = 0.25f * (b[i] + b[j] + t[i] + t[j]);
            face({b[i], b[j], t[j], t[i]}, mid - c, side);
        }
        face({t[0], t[1], t[2], t[3]}, {0, 1, 0}, top);
    }
    // A square beam of width `w` from a to b (lattice members, insulators).
    void beam(glm::vec3 a, glm::vec3 b, float w, fitzel::AssetId m) {
        glm::vec3 d = b - a;
        const float len = glm::length(d);
        if (len < 1e-4f) return;
        d /= len;
        const glm::vec3 ref = std::abs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 u = glm::normalize(glm::cross(d, ref)) * (0.5f * w);
        const glm::vec3 v = glm::normalize(glm::cross(d, u)) * (0.5f * w);
        const glm::vec3 q[4] = {u + v, -u + v, -u - v, u - v};
        for (int i = 0; i < 4; ++i) {
            const glm::vec3 e0 = q[i], e1 = q[(i + 1) % 4];
            face({a + e0, a + e1, b + e1, b + e0}, e0 + e1, m);
        }
    }

    // Every face went in with corners of its own; the ones that share a corner
    // exactly (position, normal, UV) are welded into one vertex -- lettering cut
    // into triangles is most of a sign, and two thirds of its corners are shared.
    static void weld(fitzel::MeshData& md) {
        struct Key {
            glm::vec3 p, n; glm::vec2 uv;
            bool operator<(const Key& o) const {
                if (p.x != o.p.x) return p.x < o.p.x;
                if (p.y != o.p.y) return p.y < o.p.y;
                if (p.z != o.p.z) return p.z < o.p.z;
                if (n.x != o.n.x) return n.x < o.n.x;
                if (n.y != o.n.y) return n.y < o.n.y;
                if (n.z != o.n.z) return n.z < o.n.z;
                if (uv.x != o.uv.x) return uv.x < o.uv.x;
                return uv.y < o.uv.y;
            }
        };
        std::map<Key, std::uint32_t> seen;
        std::vector<fitzel::Vertex> verts;
        std::vector<std::uint32_t> remap(md.vertices.size());
        for (std::size_t i = 0; i < md.vertices.size(); ++i) {
            const fitzel::Vertex& v = md.vertices[i];
            const auto it = seen.emplace(Key{v.position, v.normal, v.uv},
                                         static_cast<std::uint32_t>(verts.size()));
            if (it.second) verts.push_back(v);
            remap[i] = it.first->second;
        }
        for (std::uint32_t& idx : md.indices) idx = remap[idx];
        md.vertices = std::move(verts);
    }

    Model finish() {
        Model m;
        for (auto& [id, md] : parts)
            if (!md.vertices.empty()) {
                weld(md);
                m.parts.emplace_back(id, std::move(md));
            }
        m.solids = std::move(solids);
        m.height = top;
        return m;
    }
};

} // namespace civic
