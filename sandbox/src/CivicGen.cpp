#include "CivicGen.hpp"

#include "StreetSign.hpp"   // the lettering on a bus stop's sign

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace civic {

namespace {

using fitzel::AssetId;

constexpr float kPi = 3.14159265358979323846f;

// --- Deterministic variation ---------------------------------------------------
std::uint32_t hashU(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float unit(std::uint32_t h) { return static_cast<float>(h & 0xffffffU) / 16777215.0f; }

// --- Materials -----------------------------------------------------------------
AssetId ensure(std::vector<MaterialDef>& mats, const char* part, glm::vec3 albedo,
               float refl, float rough, glm::vec3 emission = glm::vec3(0.0f),
               float emissionStrength = 1.0f) {
    const std::string name = std::string("City Civic ") + part;
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = albedo; m.reflectivity = refl; m.roughness = rough;
        m.emission = emission; m.emissionStrength = emissionStrength;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId          = AssetId::generate();
    md.name             = name;
    md.albedo           = albedo;
    md.reflectivity     = refl;
    md.roughness        = rough;
    md.emission         = emission;
    md.emissionStrength = emissionStrength;
    mats.push_back(md);
    return md.assetId;
}

// The procedural window grid on an office facade (see MaterialDef::windowGrid):
// the rows land on the storeys the generator builds to.
void windows(std::vector<MaterialDef>& mats, AssetId id, glm::vec2 cell, float lit,
             float seed) {
    for (MaterialDef& m : mats)
        if (m.assetId == id) {
            m.windowGrid  = true;
            m.windowCell  = cell;
            m.windowLit   = lit;
            m.windowSeed  = seed;
            m.windowColor = {1.00f, 0.88f, 0.70f};
            m.windowGlow  = 2.2f;
        }
}

// --- Geometry --------------------------------------------------------------------
// Faces go in per material, in the plot's frame. Every face is a convex polygon
// wound to face `outward` (CCW seen from outside is the engine's front face),
// flat-shaded, with a planar UV off its dominant axis -- the civic materials are
// colours, and a texture put on one later tiles at four metres a repeat.
struct Builder {
    std::map<AssetId, fitzel::MeshData> parts;
    std::vector<city::Piece> solids;
    float top = 0.0f;

    void face(std::vector<glm::vec3> p, glm::vec3 outward, AssetId m) {
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
    void box(float x0, float x1, float y0, float y1, float z0, float z1, AssetId m,
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
               AssetId roof, AssetId wall, float over = 0.4f) {
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

    void pyramid(float cx, float cz, float hx, float hz, float y0, float y1, AssetId m) {
        const glm::vec3 a{cx - hx, y0, cz - hz}, b{cx + hx, y0, cz - hz};
        const glm::vec3 c{cx + hx, y0, cz + hz}, d{cx - hx, y0, cz + hz};
        const glm::vec3 t{cx, y1, cz};
        face({a, b, t}, {0, hz, -(y1 - y0)}, m);
        face({b, c, t}, {(y1 - y0), hz, 0}, m);
        face({c, d, t}, {0, hz, (y1 - y0)}, m);
        face({d, a, t}, {-(y1 - y0), hz, 0}, m);
    }

    void cylinder(float cx, float cz, float r, float y0, float y1, AssetId m,
                  int seg = 16, bool cap = true, bool solid = false) {
        std::vector<glm::vec3> ring;
        for (int i = 0; i < seg; ++i) {
            const float a = 2.0f * kPi * i / seg;
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

    void cone(float cx, float cz, float r, float y0, float y1, AssetId m, int seg = 16) {
        for (int i = 0; i < seg; ++i) {
            const float a = 2.0f * kPi * i / seg, b = 2.0f * kPi * (i + 1) / seg;
            const glm::vec3 p{cx + r * std::cos(a), y0, cz + r * std::sin(a)};
            const glm::vec3 q{cx + r * std::cos(b), y0, cz + r * std::sin(b)};
            const float am = 0.5f * (a + b);
            face({p, q, {cx, y1, cz}}, {std::cos(am) * (y1 - y0), r, std::sin(am) * (y1 - y0)}, m);
        }
    }

    void dome(float cx, float cz, float r, float y0, AssetId m, int seg = 16, int rings = 5) {
        for (int j = 0; j < rings; ++j) {
            const float t0 = 0.5f * kPi * j / rings, t1 = 0.5f * kPi * (j + 1) / rings;
            for (int i = 0; i < seg; ++i) {
                const float a = 2.0f * kPi * i / seg, b = 2.0f * kPi * (i + 1) / seg;
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
    void vehicle(float cx, float cz, float len, float wid, float h, AssetId body,
                 AssetId stripe, AssetId glass, bool along = true) {
        const float hl = 0.5f * len, hw = 0.5f * wid;
        auto B = [&](float a0, float a1, float y0, float y1, float b0, float b1, AssetId m) {
            if (along) box(cx + a0, cx + a1, y0, y1, cz + b0, cz + b1, m);
            else       box(cx + b0, cx + b1, y0, y1, cz + a0, cz + a1, m);
        };
        B(-hl, hl, 0.35f, h * 0.62f, -hw, hw, body);
        B(-hl * 0.45f, hl * 0.55f, h * 0.62f, h, -hw * 0.9f, hw * 0.9f, glass);
        B(-hl + 0.02f, hl - 0.02f, h * 0.36f, h * 0.48f, -hw - 0.03f, hw + 0.03f, stripe);
        B(-hl * 0.7f, -hl * 0.4f, 0.0f, 0.4f, -hw + 0.05f, hw - 0.05f, glass);   // wheels
        B(hl * 0.4f, hl * 0.7f, 0.0f, 0.4f, -hw + 0.05f, hw - 0.05f, glass);
    }

    Model finish() {
        Model m;
        for (auto& [id, md] : parts)
            if (!md.vertices.empty()) m.parts.emplace_back(id, std::move(md));
        m.solids = std::move(solids);
        m.height = top;
        return m;
    }
};

// --- The kinds -------------------------------------------------------------------
// Each lays out a W x D plot whose front edge (the street) is z = -D/2.

Model church(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    const float nw = glm::clamp(W * 0.32f, 9.0f, 17.0f);
    const float front = glm::clamp(D * 0.12f, 3.0f, 12.0f);   // the square before it
    const float tw = std::max(nw * 0.55f, 5.5f);
    const float z0 = -0.5f * D + front;
    const float nl = glm::clamp(D - front - tw * 0.6f - nw * 0.6f - 2.0f, 14.0f, 46.0f);
    if (W < nw + 4.0f || D < front + tw + 14.0f) return {};
    const float wallH = 9.5f + nw * 0.2f;
    const float ridge = wallH + nw * 0.55f;
    const AssetId roof = unit(hashU(h ^ 0x21U)) < 0.5f ? P.slate : P.tile;

    // Nave.
    const float nz0 = z0 + tw * 0.6f, nz1 = nz0 + nl;
    b.box(-0.5f * nw, 0.5f * nw, 0.0f, wallH, nz0, nz1, P.stone, true);
    b.gable(-0.5f * nw, 0.5f * nw, nz0, nz1, wallH, ridge, true, roof, P.stone);
    // Tall windows down both flanks, a dark pane standing just proud of the wall.
    for (float z = nz0 + 4.0f; z < nz1 - 3.0f; z += 4.6f)
        for (int s = -1; s <= 1; s += 2) {
            const float x = s * (0.5f * nw + 0.04f);
            b.box(std::min(x, x + s * 0.06f), std::max(x, x + s * 0.06f), 2.8f, wallH - 2.0f,
                  z - 0.7f, z + 0.7f, P.darkGlass);
            b.pyramid(x + s * 0.03f, z, 0.03f, 0.7f, wallH - 2.0f, wallH - 1.2f, P.darkGlass);
        }
    // Transept on a bigger church: the cross you see from the air.
    if (nl > 26.0f && unit(hashU(h ^ 0x33U)) < 0.7f) {
        const float tz = nz0 + nl * 0.62f, td = nw * 0.9f, tx = 0.5f * nw + nw * 0.55f;
        b.box(-tx, tx, 0.0f, wallH, tz - 0.5f * td, tz + 0.5f * td, P.stone, true);
        b.gable(-tx, tx, tz - 0.5f * td, tz + 0.5f * td, wallH, ridge, false, roof, P.stone);
    }
    // Apse: the rounded east end.
    const float ar = nw * 0.38f;
    b.cylinder(0.0f, nz1, ar, 0.0f, wallH * 0.85f, P.stone, 14, false, true);
    b.cone(0.0f, nz1, ar + 0.3f, wallH * 0.85f, wallH * 0.85f + nw * 0.35f, roof, 14);

    // Tower on the front, with the door, belfry openings, clocks and the spire.
    const float towerH = ridge + 6.0f + 10.0f * unit(hashU(h ^ 0x44U));
    const float tx0 = -0.5f * tw, tx1 = 0.5f * tw, tz1 = z0 + tw;
    b.box(tx0, tx1, 0.0f, towerH, z0, tz1, P.stone, true);
    b.box(-1.3f, 1.3f, 0.0f, 4.2f, z0 - 0.08f, z0, P.wood);
    b.pyramid(0.0f, z0 - 0.04f, 1.3f, 0.04f, 4.2f, 5.2f, P.wood);
    const float bel = towerH - 4.5f;
    const float cz = 0.5f * (z0 + tz1);
    b.box(-0.6f, 0.6f, bel, bel + 2.8f, z0 - 0.05f, z0, P.darkGlass);
    b.box(-0.6f, 0.6f, bel, bel + 2.8f, tz1, tz1 + 0.05f, P.darkGlass);
    b.box(tx0 - 0.05f, tx0, bel, bel + 2.8f, cz - 0.6f, cz + 0.6f, P.darkGlass);
    b.box(tx1, tx1 + 0.05f, bel, bel + 2.8f, cz - 0.6f, cz + 0.6f, P.darkGlass);
    b.box(-1.0f, 1.0f, bel - 3.6f, bel - 1.6f, z0 - 0.07f, z0, P.white);   // clock face
    b.box(-0.08f, 0.08f, bel - 2.7f, bel - 1.8f, z0 - 0.1f, z0 - 0.07f, P.gold);
    b.box(-0.08f, 0.6f, bel - 2.7f, bel - 2.55f, z0 - 0.1f, z0 - 0.07f, P.gold);
    const float spire = towerH + tw * (1.8f + 1.0f * unit(hashU(h ^ 0x55U)));
    b.pyramid(0.0f, cz, 0.5f * tw + 0.2f, 0.5f * tw + 0.2f, towerH, spire, P.copper);
    b.box(-0.07f, 0.07f, spire, spire + 2.2f, cz - 0.07f, cz + 0.07f, P.gold);
    b.box(-0.5f, 0.5f, spire + 1.3f, spire + 1.45f, cz - 0.07f, cz + 0.07f, P.gold);
    return b.finish();
}

Model police(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    const float bw = glm::clamp(W * 0.62f, 20.0f, 46.0f);
    const float bd = glm::clamp(D * 0.28f, 11.0f, 15.0f);
    if (W < bw + 2.0f || D < bd + 8.0f) return {};
    const int   floors = 3 + static_cast<int>(hashU(h ^ 0x10U) % 2U);
    const float fh = 3.3f, H = floors * fh;
    const float z0 = -0.5f * D + 5.0f, z1 = z0 + bd;
    const float x0 = -0.5f * W + 2.0f, x1 = x0 + bw;
    b.box(x0, x1, 0.0f, H, z0, z1, P.office, true);
    b.box(x0 - 0.15f, x1 + 0.15f, 0.0f, 0.8f, z0 - 0.15f, z1 + 0.15f, P.concrete);  // plinth
    b.box(x0 - 0.2f, x1 + 0.2f, H, H + 0.9f, z0 - 0.2f, z1 + 0.2f, P.concrete);     // parapet
    // The blue band at the first floor -- the colour that says "police" at 200 m.
    b.box(x0 - 0.12f, x1 + 0.12f, fh - 0.55f, fh, z0 - 0.12f, z1 + 0.12f, P.policeBlue);
    // A rear wing for the cells and the garage.
    const float wx0 = x1 - std::min(14.0f, bw * 0.4f);
    const float wz1 = std::min(z1 + 13.0f, 0.5f * D - 2.0f);
    if (wz1 > z1 + 5.0f) {
        b.box(wx0, x1, 0.0f, 2.0f * fh, z1, wz1, P.office, true);
        b.box(wx0 - 0.2f, x1 + 0.2f, 2.0f * fh, 2.0f * fh + 0.7f, z1, wz1 + 0.2f, P.concrete);
    }
    // Entrance: canopy, and a blue lamp either side of it.
    const float ex = 0.5f * (x0 + x1);
    b.box(ex - 3.5f, ex + 3.5f, 3.0f, 3.35f, z0 - 3.0f, z0, P.concrete);
    b.box(ex - 1.2f, ex + 1.2f, 0.0f, 2.6f, z0 - 0.06f, z0, P.darkGlass);
    for (int s = -1; s <= 1; s += 2)
        b.box(ex + s * 3.1f - 0.25f, ex + s * 3.1f + 0.25f, 3.35f, 3.9f, z0 - 2.9f, z0 - 2.4f,
              P.blueLamp);
    // Flagpoles on the forecourt.
    for (int i = 0; i < 3; ++i) {
        const float fx = x0 + 2.0f + i * 1.8f;
        b.cylinder(fx, z0 - 3.5f, 0.07f, 0.0f, 9.0f, P.white, 6);
        b.box(fx + 0.07f, fx + 2.0f, 7.6f, 8.8f, z0 - 3.52f, z0 - 3.48f,
              i == 1 ? P.policeBlue : (i == 0 ? P.fireRed : P.gold));
    }
    // Carport with patrol cars beside the building, if there is room; else the
    // cars park on the forecourt.
    const float cx0 = x1 + 4.0f, cx1 = 0.5f * W - 2.0f;
    if (cx1 - cx0 > 9.0f) {
        const float cz0 = z0, cz1 = std::min(z0 + 16.0f, 0.5f * D - 2.0f);
        b.box(cx0, cx1, 3.0f, 3.3f, cz0, cz1, P.metalRoof);
        for (float px : {cx0 + 0.2f, cx1 - 0.4f})
            for (float pz : {cz0 + 0.2f, cz1 - 0.4f})
                b.box(px, px + 0.2f, 0.0f, 3.0f, pz, pz + 0.2f, P.metal);
        int n = 0;
        for (float z = cz0 + 2.0f; z + 2.0f < cz1 && n < 4; z += 3.0f, ++n)
            b.vehicle(0.5f * (cx0 + cx1), z, 4.6f, 1.9f, 1.55f, P.white, P.policeBlue,
                      P.darkGlass);
        b.solid(cx0, cx1, 0.0f, 3.3f, cz0, cz1);
    } else {
        for (int i = 0; i < 2; ++i)
            b.vehicle(x1 - 3.0f - i * 5.5f, z0 - 3.0f, 4.6f, 1.9f, 1.55f, P.white,
                      P.policeBlue, P.darkGlass);
    }
    return b.finish();
}

Model fireStation(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    const int   doors = glm::clamp(static_cast<int>((W * 0.55f) / 5.2f), 2, 5);
    const float hallW = doors * 5.2f + 1.6f, hallH = 7.5f;
    const float hallD = glm::clamp(D * 0.36f, 14.0f, 22.0f);
    const float apron = 10.0f;                       // where the engines stand
    if (W < hallW + 4.0f || D < hallD + apron + 2.0f) return {};
    const float x0 = -0.5f * W + 2.0f, x1 = x0 + hallW;
    const float z0 = -0.5f * D + apron, z1 = z0 + hallD;
    b.box(x0, x1, 0.0f, hallH, z0, z1, P.brick, true);
    b.box(x0 - 0.2f, x1 + 0.2f, hallH, hallH + 0.6f, z0 - 0.2f, z1 + 0.2f, P.concrete);
    // The doors: the whole identity of the building.
    for (int i = 0; i < doors; ++i) {
        const float dx = x0 + 1.0f + i * 5.2f;
        b.box(dx, dx + 4.2f, 0.0f, 4.6f, z0 - 0.1f, z0, P.fireRed);
        b.box(dx - 0.15f, dx + 4.35f, 4.6f, 4.85f, z0 - 0.14f, z0, P.white);
        for (float y = 1.2f; y < 4.4f; y += 1.15f)          // panel lines
            b.box(dx + 0.1f, dx + 4.1f, y, y + 0.06f, z0 - 0.14f, z0 - 0.1f, P.brick);
    }
    b.box(x0, x1, hallH - 1.4f, hallH - 0.6f, z0 - 0.12f, z0, P.fireRed);   // name band
    // Office wing beside the hall, two storeys with windows.
    const float ox1 = std::min(x1 + 16.0f, 0.5f * W - 2.0f);
    if (ox1 - x1 > 6.0f) {
        b.box(x1, ox1, 0.0f, 7.0f, z0, z0 + std::min(hallD, 13.0f), P.brickOffice, true);
        b.box(x1, ox1 + 0.2f, 7.0f, 7.6f, z0 - 0.2f, z0 + std::min(hallD, 13.0f) + 0.2f,
              P.concrete);
    }
    // Drill tower at the back corner -- the fire station's landmark.
    const float tH = 17.0f + 6.0f * unit(hashU(h ^ 0x61U));
    const float tx1 = x1, tx0 = x1 - 4.6f;
    const float tz0 = std::min(z1, 0.5f * D - 7.0f), tz1 = tz0 + 4.6f;
    b.box(tx0, tx1, 0.0f, tH, tz0, tz1, P.brick, true);
    for (float y = 3.0f; y < tH - 2.0f; y += 3.2f)
        b.box(tx0 + 1.4f, tx1 - 1.4f, y, y + 1.7f, tz0 - 0.06f, tz0, P.darkGlass);
    b.pyramid(0.5f * (tx0 + tx1), 0.5f * (tz0 + tz1), 2.6f, 2.6f, tH, tH + 1.6f, P.metalRoof);
    // Engines out on the apron.
    const int engines = 1 + static_cast<int>(hashU(h ^ 0x62U) % 2U);
    for (int i = 0; i < engines && i < doors; ++i)
        b.vehicle(x0 + 3.1f + i * 5.2f, z0 - 4.6f, 8.2f, 2.5f, 3.0f, P.fireRed, P.white,
                  P.darkGlass, false);
    return b.finish();
}

Model hospital(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    const float mw = glm::clamp(W * 0.78f, 30.0f, 72.0f);
    const float md = glm::clamp(D * 0.24f, 13.0f, 17.0f);
    const float front = 11.0f;                       // drop-off and entrance
    if (W < mw + 2.0f || D < md + front + 4.0f) return {};
    const int   floors = 5 + static_cast<int>(hashU(h ^ 0x71U) % 4U);
    const float fh = 3.5f, H = floors * fh;
    const float x0 = -0.5f * mw, x1 = 0.5f * mw;
    const float z0 = -0.5f * D + front, z1 = z0 + md;
    b.box(x0, x1, 0.0f, H, z0, z1, P.hospital, true);
    b.box(x0 - 0.2f, x1 + 0.2f, 0.0f, 1.0f, z0 - 0.2f, z1 + 0.2f, P.stone);
    b.box(x0 - 0.2f, x1 + 0.2f, H, H + 0.8f, z0 - 0.2f, z1 + 0.2f, P.concrete);
    // Two wings running back: an H in plan, the shape of every district hospital.
    const float ww = 13.0f, wz1 = std::min(z1 + D * 0.5f, 0.5f * D - 2.0f);
    const float wH = H - fh;
    if (wz1 > z1 + 8.0f)
        for (int s = -1; s <= 1; s += 2) {
            const float a = s < 0 ? x0 : x1 - ww, c = s < 0 ? x0 + ww : x1;
            b.box(a, c, 0.0f, wH, z1, wz1, P.hospital, true);
            b.box(a - 0.2f, c + 0.2f, wH, wH + 0.8f, z1, wz1 + 0.2f, P.concrete);
        }
    // The red cross, lit, high on the front.
    const float cy = H - 3.5f;
    b.box(-3.2f, 3.2f, cy - 3.2f, cy + 3.2f, z0 - 0.12f, z0, P.white);
    b.box(-0.75f, 0.75f, cy - 2.5f, cy + 2.5f, z0 - 0.2f, z0 - 0.12f, P.redCross);
    b.box(-2.5f, 2.5f, cy - 0.75f, cy + 0.75f, z0 - 0.2f, z0 - 0.12f, P.redCross);
    // Helipad on the roof.
    const float pr = std::min(0.4f * md, 7.0f);
    const float px = x1 - pr - 2.0f, pz = 0.5f * (z0 + z1), py = H + 0.8f;
    b.cylinder(px, pz, pr, py, py + 0.3f, P.darkGrey, 20);
    b.box(px - 1.6f, px - 1.0f, py + 0.3f, py + 0.34f, pz - 2.2f, pz + 2.2f, P.white);
    b.box(px + 1.0f, px + 1.6f, py + 0.3f, py + 0.34f, pz - 2.2f, pz + 2.2f, P.white);
    b.box(px - 1.0f, px + 1.0f, py + 0.3f, py + 0.34f, pz - 0.3f, pz + 0.3f, P.white);
    // Entrance canopy and the emergency bay with its ambulances.
    b.box(-8.0f, 8.0f, 4.0f, 4.4f, z0 - 6.0f, z0, P.concrete);
    for (float px2 : {-7.6f, 7.4f}) b.box(px2, px2 + 0.25f, 0.0f, 4.0f, z0 - 5.8f, z0 - 5.55f, P.metal);
    b.box(-2.0f, 2.0f, 0.0f, 3.0f, z0 - 0.06f, z0, P.darkGlass);
    b.box(-8.0f, -3.5f, 4.4f, 4.9f, z0 - 6.02f, z0 - 5.9f, P.redCross);
    for (int i = 0; i < 2; ++i)
        b.vehicle(-5.5f - i * 3.2f, z0 - 3.2f, 5.8f, 2.2f, 2.6f, P.white, P.fireRed,
                  P.darkGlass, false);
    return b.finish();
}

// One hall of an estate, filling [x0,x1] x [z0,z1].
void industryHall(Builder& b, float x0, float x1, float z0, float z1, std::uint32_t h,
                  const Palette& P) {
    const float hh = 8.0f + 4.0f * unit(hashU(h ^ 0x81U));
    const bool  shed = unit(hashU(h ^ 0x82U)) < 0.55f;
    b.box(x0, x1, 0.0f, hh, z0, z1, shed ? P.concrete : P.metal, true);
    if (shed) {
        // Sawtooth: glazed north lights facing away from the street, slopes up
        // towards them -- the factory roof everyone knows.
        const float tooth = 7.0f, th = 3.2f;
        for (float z = z0; z + 1.0f < z1; z += tooth) {
            const float za = z, zb = std::min(z + tooth, z1);
            b.face({{x0, hh, za}, {x1, hh, za}, {x1, hh + th, zb}, {x0, hh + th, zb}},
                   {0, zb - za, -th}, P.metalRoof);
            b.face({{x0, hh, zb}, {x0, hh + th, zb}, {x1, hh + th, zb}, {x1, hh, zb}},
                   {0, 0, 1}, P.darkGlass);
            b.face({{x0, hh, za}, {x0, hh + th, zb}, {x0, hh, zb}}, {-1, 0, 0}, P.concrete);
            b.face({{x1, hh, za}, {x1, hh, zb}, {x1, hh + th, zb}}, {1, 0, 0}, P.concrete);
        }
    } else {
        const bool alongZ = (z1 - z0) > (x1 - x0);
        b.gable(x0, x1, z0, z1, hh, hh + 2.6f, alongZ, P.metalRoof, P.metal, 0.3f);
    }
    // Loading doors on the street side.
    for (float x = x0 + 3.0f; x + 4.5f < x1 - 2.0f; x += 7.5f)
        b.box(x, x + 4.2f, 0.0f, 4.6f, z0 - 0.08f, z0, P.darkGrey);
}

Model industry(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 20.0f || D < 20.0f) return {};
    const int   n   = W > 110.0f ? 3 : (W > 60.0f ? 2 : 1);
    const float gap = 8.0f;
    // The chimney needs ground of its own: in the gap between the first two
    // slices, or -- on a one-slice estate -- a strip kept free at the left end.
    // Planted anywhere inside a slice it stood in the middle of a tank.
    const bool  chimney = unit(hashU(h ^ 0xa1U)) < 0.45f;
    const float lead = (chimney && n == 1) ? 6.0f : 0.0f;
    const float sw  = (W - 4.0f - lead - (n - 1) * gap) / n;
    const float z0  = -0.5f * D + 3.0f, z1 = 0.5f * D - 3.0f;
    const float xs  = -0.5f * W + 2.0f + lead;
    bool hadHall = false;
    for (int i = 0; i < n; ++i) {
        const float x0 = xs + i * (sw + gap), x1 = x0 + sw;
        const std::uint32_t hi = hashU(h ^ (0x9e37U * static_cast<std::uint32_t>(i + 1)));
        const float pick = unit(hi);
        // At least one hall per estate: silos and tanks alone read as a farm.
        const int kind = (!hadHall && i + 1 == n) ? 0 : (pick < 0.55f ? 0 : pick < 0.8f ? 1 : 2);
        if (kind == 0) {
            industryHall(b, x0, x1, z0, z1, hi, P);
            hadHall = true;
        } else if (kind == 1) {
            // Silos: a row of cylinders with cones, and a head house on top.
            const int   m = glm::clamp(static_cast<int>(sw / 9.0f), 2, 5);
            const float r = std::min(sw / (2.0f * m) - 0.4f, 5.0f);
            const float sh = 18.0f + 10.0f * unit(hashU(hi ^ 0x3U));
            const float zc = z0 + std::min(12.0f, 0.5f * (z1 - z0));
            for (int k = 0; k < m; ++k) {
                const float cx = x0 + (k + 0.5f) * (sw / m);
                b.cylinder(cx, zc, r, 0.0f, sh, P.concrete, 16, false, true);
                b.cone(cx, zc, r, sh, sh + r * 0.5f, P.concrete, 16);
            }
            b.box(x0 + (sw / m) * 0.5f, x1 - (sw / m) * 0.5f, sh + r * 0.5f, sh + r * 0.5f + 3.2f,
                  zc - 1.8f, zc + 1.8f, P.metal);
            if (z1 - zc > 20.0f)
                industryHall(b, x0, x1, zc + r + 6.0f, z1, hashU(hi ^ 0x5U), P);
        } else {
            // A tank farm.
            const float r = std::min(std::min(sw, z1 - z0) * 0.22f, 10.0f);
            for (int k = 0; k < 2; ++k)
                for (int l = 0; l < 2; ++l) {
                    const float cx = x0 + (k + 0.5f) * 0.5f * sw, cz = z0 + (l + 0.5f) * 0.5f * (z1 - z0);
                    const float th = 7.0f + 5.0f * unit(hashU(hi ^ (0x70U + k * 2U + l)));
                    b.cylinder(cx, cz, r, 0.0f, th, P.tank, 20, false, true);
                    b.dome(cx, cz, r, th, P.tank, 20, 3);
                }
        }
    }
    // A chimney, red and white, at the back of the estate.
    if (chimney) {
        const float cx = n > 1 ? xs + sw + 0.5f * gap : -0.5f * W + 2.0f + 0.5f * lead;
        const float cz = z1 - 3.0f;
        const float ch = 34.0f + 22.0f * unit(hashU(h ^ 0xa2U));
        const int   bands = 8;
        for (int k = 0; k < bands; ++k) {
            const float y0 = ch * k / bands, y1 = ch * (k + 1) / bands;
            const float r  = 2.0f - 0.7f * (static_cast<float>(k) / bands);
            b.cylinder(cx, cz, r, y0, y1, (k % 2 == 0 || k + 1 < bands / 2) ? P.chimneyRed : P.white,
                       12, k + 1 == bands, false);
        }
        b.solid(cx - 1.6f, cx + 1.6f, 0.0f, ch, cz - 1.6f, cz + 1.6f);
    }
    return b.finish();
}

} // namespace

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Church:      return "Church";
        case Kind::Police:      return "Police station";
        case Kind::FireStation: return "Fire station";
        case Kind::Hospital:    return "Hospital";
        case Kind::Industry:    return "Industry";
        default:                return "?";
    }
}

Palette ensurePalette(std::vector<MaterialDef>& mats, float windowLit) {
    Palette p;
    p.stone      = ensure(mats, "Stone",       {0.74f, 0.70f, 0.62f}, 0.00f, 0.85f);
    p.slate      = ensure(mats, "Slate",       {0.20f, 0.21f, 0.24f}, 0.05f, 0.60f);
    p.tile       = ensure(mats, "Roof Tile",   {0.50f, 0.20f, 0.13f}, 0.00f, 0.70f);
    p.copper     = ensure(mats, "Copper",      {0.33f, 0.56f, 0.47f}, 0.10f, 0.55f);
    p.darkGlass  = ensure(mats, "Dark Glass",  {0.05f, 0.06f, 0.08f}, 0.30f, 0.10f);
    p.wood       = ensure(mats, "Door",        {0.30f, 0.18f, 0.10f}, 0.00f, 0.60f);
    p.gold       = ensure(mats, "Gold",        {0.78f, 0.62f, 0.26f}, 0.60f, 0.30f);
    p.office     = ensure(mats, "Office",      {0.76f, 0.75f, 0.72f}, 0.00f, 0.80f);
    p.hospital   = ensure(mats, "Hospital",    {0.90f, 0.91f, 0.91f}, 0.00f, 0.75f);
    p.brick      = ensure(mats, "Brick",       {0.50f, 0.23f, 0.17f}, 0.00f, 0.85f);
    p.brickOffice = ensure(mats, "Brick Office", {0.50f, 0.23f, 0.17f}, 0.00f, 0.85f);
    p.fireRed    = ensure(mats, "Fire Red",    {0.72f, 0.05f, 0.04f}, 0.15f, 0.35f);
    p.policeBlue = ensure(mats, "Police Blue", {0.05f, 0.18f, 0.52f}, 0.05f, 0.45f);
    p.blueLamp   = ensure(mats, "Blue Lamp",   {0.05f, 0.10f, 0.35f}, 0.00f, 0.30f,
                          {0.25f, 0.45f, 1.00f}, 6.0f);
    p.redCross   = ensure(mats, "Red Cross",   {0.70f, 0.03f, 0.03f}, 0.00f, 0.40f,
                          {1.00f, 0.08f, 0.06f}, 3.0f);
    p.white      = ensure(mats, "White",       {0.93f, 0.93f, 0.92f}, 0.00f, 0.60f);
    p.concrete   = ensure(mats, "Concrete",    {0.56f, 0.55f, 0.52f}, 0.00f, 0.90f);
    p.metal      = ensure(mats, "Cladding",    {0.47f, 0.52f, 0.56f}, 0.15f, 0.55f);
    p.metalRoof  = ensure(mats, "Metal Roof",  {0.36f, 0.38f, 0.41f}, 0.15f, 0.50f);
    p.chimneyRed = ensure(mats, "Chimney Red", {0.62f, 0.15f, 0.11f}, 0.00f, 0.80f);
    p.darkGrey   = ensure(mats, "Dark Grey",   {0.14f, 0.14f, 0.15f}, 0.00f, 0.80f);
    // Not paper-white: a sunlit tank at 0.85 is the brightest thing in the
    // frame and blooms into a halo.
    p.tank       = ensure(mats, "Tank",        {0.66f, 0.67f, 0.66f}, 0.04f, 0.65f);
    p.pavement   = ensure(mats, "Pavement",    {0.34f, 0.33f, 0.32f}, 0.00f, 0.90f);
    // The lamps: a dark lens in the library (emission strength 0), lit per frame.
    for (int a = 0; a < 2; ++a) {
        const std::string ax = a == 0 ? "Signal X " : "Signal Z ";
        p.signalRed[a]   = ensure(mats, (ax + "Red").c_str(),   {0.22f, 0.03f, 0.02f}, 0.05f, 0.20f,
                                  {1.00f, 0.06f, 0.02f}, 0.0f);
        p.signalAmber[a] = ensure(mats, (ax + "Amber").c_str(), {0.24f, 0.13f, 0.02f}, 0.05f, 0.20f,
                                  {1.00f, 0.45f, 0.02f}, 0.0f);
        p.signalGreen[a] = ensure(mats, (ax + "Green").c_str(), {0.02f, 0.18f, 0.10f}, 0.05f, 0.20f,
                                  {0.10f, 1.00f, 0.55f}, 0.0f);
    }
    // Real glass reflects 4% head on; more and it turns into a dark mirror.
    p.shelterGlass = ensure(mats, "Shelter Glass", {0.80f, 0.86f, 0.88f}, 0.04f, 0.08f);
    p.busYellow  = ensure(mats, "Bus Stop Yellow", {0.95f, 0.76f, 0.05f}, 0.00f, 0.45f);
    p.busGreen   = ensure(mats, "Bus Stop Green",  {0.00f, 0.38f, 0.17f}, 0.00f, 0.45f);
    // A shelter you can see through: the one pane in the town that is not opaque
    // on purpose (the houses' glass is, see HouseGen). One blended draw per chunk.
    for (MaterialDef& m : mats)
        if (m.assetId == p.shelterGlass) m.opacity = 0.30f;
    windows(mats, p.office,      {2.4f, 3.3f}, windowLit, 71.0f);
    windows(mats, p.hospital,    {2.2f, 3.5f}, windowLit, 83.0f);
    windows(mats, p.brickOffice, {2.4f, 3.5f}, windowLit, 97.0f);
    return p;
}

Model build(Kind kind, float width, float depth, std::uint32_t seed, const Palette& pal) {
    switch (kind) {
        case Kind::Church:      return church(width, depth, seed, pal);
        case Kind::Police:      return police(width, depth, seed, pal);
        case Kind::FireStation: return fireStation(width, depth, seed, pal);
        case Kind::Hospital:    return hospital(width, depth, seed, pal);
        case Kind::Industry:    return industry(width, depth, seed, pal);
        default:                return {};
    }
}

// --- Street furniture ----------------------------------------------------------

void signalPhase(double t, SignalLamps& a, SignalLamps& b) {
    double s = std::fmod(t, static_cast<double>(kSignalCycle));
    if (s < 0.0) s += kSignalCycle;
    a = b = SignalLamps{};
    a.red = b.red = true;                              // the default: stop
    if (s < 12.0)      { a.red = false; a.green = true; }
    else if (s < 15.0) { a.red = false; a.amber = true; }
    else if (s < 16.0) { }                             // all red
    else if (s < 17.0) { b.amber = true; }             // B: red + amber
    else if (s < 29.0) { b.red = false; b.green = true; }
    else if (s < 32.0) { b.red = false; b.amber = true; }
    else if (s < 33.0) { }                             // all red
    else               { a.amber = true; }             // A: red + amber
}

Model trafficLight(int axis, const Palette& P) {
    axis = axis == 0 ? 0 : 1;
    Builder b;
    b.cylinder(0.0f, 0.0f, 0.06f, 0.0f, 3.3f, P.metal, 10, true, true);
    // The head in front of the pole, a contrast board behind its lamps.
    b.box(-0.17f, 0.17f, 2.25f, 3.15f, -0.30f, -0.07f, P.darkGrey);
    b.box(-0.28f, 0.28f, 2.15f, 3.25f, -0.07f, -0.05f, P.darkGrey);
    const float ys[3] = {2.95f, 2.70f, 2.45f};
    for (int i = 0; i < 3; ++i) {
        const AssetId lamp = i == 0 ? P.signalRed[axis] : i == 1 ? P.signalAmber[axis]
                                                                 : P.signalGreen[axis];
        std::vector<glm::vec3> disc;
        for (int k = 0; k < 12; ++k) {
            const float a = 2.0f * kPi * k / 12;
            disc.push_back({0.10f * std::cos(a), ys[i] + 0.10f * std::sin(a), -0.302f});
        }
        b.face(disc, {0, 0, -1}, lamp);
        // A hood over each lamp, so the sun does not light it from above.
        b.box(-0.12f, 0.12f, ys[i] + 0.10f, ys[i] + 0.12f, -0.42f, -0.30f, P.darkGrey);
    }
    return b.finish();
}

Model busShelter(const Palette& P) {
    Builder b;
    const float hw = 1.8f, z0 = -0.72f, z1 = 0.72f, h = 2.45f;
    for (float x : {-hw, hw})
        for (float z : {z0, z1}) b.box(x - 0.04f, x + 0.04f, 0.0f, h, z - 0.04f, z + 0.04f, P.metal);
    b.box(-hw - 0.1f, hw + 0.1f, h, h + 0.10f, z0 - 0.12f, z1 + 0.08f, P.metal);
    b.box(-hw + 0.04f, hw - 0.04f, 0.12f, h - 0.05f, z1 - 0.01f, z1 + 0.01f, P.shelterGlass);
    b.box(hw - 0.01f, hw + 0.01f, 0.12f, h - 0.05f, z0 + 0.12f, z1 - 0.04f, P.shelterGlass);
    // The bench against the back, on two legs.
    b.box(-1.0f, 1.0f, 0.42f, 0.47f, 0.18f, 0.60f, P.metal);
    for (float x : {-0.8f, 0.8f}) b.box(x - 0.03f, x + 0.03f, 0.0f, 0.42f, 0.35f, 0.45f, P.metal);
    // What a car hits: the back wall and the glass side.
    b.solid(-hw, hw, 0.0f, h, z1 - 0.06f, z1 + 0.06f);
    b.solid(hw - 0.06f, hw + 0.06f, 0.0f, h, z0, z1);
    return b.finish();
}

Model busStopSign(const std::string& name, const Palette& P) {
    Builder b;
    const float pole = 2.75f, cy = 2.52f, R = 0.22f, T = 0.012f;
    b.cylinder(0.0f, 0.0f, 0.04f, 0.0f, pole, P.metal, 8, true, true);
    // The disc, in front of the pole: yellow, a green ring, the green H.
    const float zc = -0.06f;
    const int   seg = 16;
    std::vector<glm::vec3> front, back;
    for (int k = 0; k < seg; ++k) {
        const float a = 2.0f * kPi * k / seg;
        const float x = R * std::cos(a), y = cy + R * std::sin(a);
        front.push_back({x, y, zc - T});
        back.push_back({x, y, zc + T});
        const float a1 = 2.0f * kPi * (k + 1) / seg;
        const float x1 = R * std::cos(a1), y1 = cy + R * std::sin(a1);
        b.face({{x, y, zc - T}, {x1, y1, zc - T}, {x1, y1, zc + T}, {x, y, zc + T}},
               {std::cos(0.5f * (a + a1)), std::sin(0.5f * (a + a1)), 0.0f}, P.busYellow);
    }
    b.face(front, {0, 0, -1}, P.busYellow);
    b.face(back, {0, 0, 1}, P.busYellow);
    for (int side = -1; side <= 1; side += 2) {
        const float z = zc + side * (T + 0.0015f);
        const glm::vec3 n(0.0f, 0.0f, static_cast<float>(side));
        for (int k = 0; k < seg; ++k) {
            const float a0 = 2.0f * kPi * k / seg, a1 = 2.0f * kPi * (k + 1) / seg;
            const float r0 = 0.17f, r1 = 0.20f;
            b.face({{r0 * std::cos(a0), cy + r0 * std::sin(a0), z},
                    {r1 * std::cos(a0), cy + r1 * std::sin(a0), z},
                    {r1 * std::cos(a1), cy + r1 * std::sin(a1), z},
                    {r0 * std::cos(a1), cy + r0 * std::sin(a1), z}},
                   n, P.busGreen);
        }
        // The H, in the street signs' typeface. Seen from -z a reader's right is
        // -x, so the front face runs its letters mirrored in x.
        streetsign::Style st = streetsign::presetStyle(0);
        st.frame = streetsign::Frame::None;
        st.capitals = true;
        const streetsign::Face h = streetsign::layout("H", st, 0.17f);
        for (const streetsign::Poly& p : h.ink) {
            std::vector<glm::vec3> q;
            for (const glm::vec2& v : p.pts)
                q.push_back({side < 0 ? -v.x : v.x, cy + v.y, z});
            b.face(q, n, P.busGreen);
        }
    }
    // The name plate under the disc, yellow with green letters, both faces.
    streetsign::Style st;
    st.plate = glm::vec3(0.95f, 0.76f, 0.05f);
    st.frame = streetsign::Frame::None;
    const streetsign::Face f = streetsign::layout(name, st, 0.05f);
    const float py = cy - R - 0.05f - 0.5f * f.height;
    std::vector<glm::vec3> pf, pb;
    for (const glm::vec2& v : f.outline) {
        pf.push_back({-v.x, py + v.y, zc - T});
        pb.push_back({v.x, py + v.y, zc + T});
    }
    b.face(pf, {0, 0, -1}, P.busYellow);
    b.face(pb, {0, 0, 1}, P.busYellow);
    {
        const int   side = 1;
        const float z = zc + side * (T + 0.0015f);
        for (const streetsign::Poly& p : f.ink) {
            std::vector<glm::vec3> q;
            for (const glm::vec2& v : p.pts) q.push_back({side < 0 ? -v.x : v.x, py + v.y, z});
            b.face(q, {0.0f, 0.0f, static_cast<float>(side)}, P.busGreen);
        }
    }
    return b.finish();
}

} // namespace civic
