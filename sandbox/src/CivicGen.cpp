#include "CivicGen.hpp"

#include "CivicBuilder.hpp"
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


// Lettering on a facade in the street signs' face: capitals `h` tall (shrunk to
// fit `maxW`), centred on (x, y) in the plane z = `z`, read from -z.
void letter(Builder& b, const std::string& text, float h, float maxW, float x, float y, float z,
            AssetId m, bool capitals = true) {
    streetsign::Style st;
    st.frame    = streetsign::Frame::None;
    st.capitals = capitals;
    streetsign::Face f = streetsign::layout(text, st, h);
    const float inkW = f.width - 1.6f * h;   // layout() pads 0.8 h either side
    if (maxW > 0.0f && inkW > maxW) f = streetsign::layout(text, st, h * maxW / inkW);
    for (const streetsign::Poly& p : f.ink) {
        std::vector<glm::vec3> q;
        // Seen from -z the reader's right is -x.
        for (const glm::vec2& v : p.pts) q.push_back({x - v.x, y + v.y, z});
        b.face(q, {0.0f, 0.0f, -1.0f}, m);
    }
}

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


// --- The cultural buildings --------------------------------------------------------

// A flag in black, red and gold on a pole at (x, z).
void flag(Builder& b, float x, float z, const Palette& P) {
    b.cylinder(x, z, 0.07f, 0.0f, 9.0f, P.white, 6);
    const AssetId stripes[3] = {P.darkGrey, P.fireRed, P.gold};
    for (int i = 0; i < 3; ++i)
        b.box(x + 0.07f, x + 2.2f, 8.8f - (i + 1) * 0.4f, 8.8f - i * 0.4f, z - 0.02f, z + 0.02f,
              stripes[i]);
}

// Paving across a stretch of the plot, just above the ground.
void plaza(Builder& b, float x0, float x1, float z0, float z1, const Palette& P) {
    if (x1 - x0 > 0.5f && z1 - z0 > 0.5f) b.box(x0, x1, 0.0f, 0.06f, z0, z1, P.pavement);
}

Model townHall(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    const float bw = glm::clamp(W * 0.6f, 24.0f, 48.0f);
    const float bd = glm::clamp(D * 0.3f, 14.0f, 20.0f);
    if (W < bw + 4.0f || D < bd + 14.0f) return {};
    const float fh = 3.8f, H = 3.0f * fh;
    const float z0 = -0.5f * D + 11.0f, z1 = z0 + bd;
    const float x0 = -0.5f * bw, x1 = 0.5f * bw;
    b.box(x0, x1, 0.0f, H, z0, z1, P.hall, true);
    b.box(x0 - 0.2f, x1 + 0.2f, 0.0f, 1.0f, z0 - 0.2f, z1 + 0.2f, P.stone);     // plinth
    b.box(x0 - 0.3f, x1 + 0.3f, H - 0.5f, H, z0 - 0.3f, z1 + 0.3f, P.stone);    // cornice
    b.gable(x0, x1, z0, z1, H, H + 0.42f * bd, false, P.slate, P.hall, 0.6f);
    // The clock tower over the entrance, standing out of the front.
    const float th = H + 10.0f + 4.0f * unit(hashU(h ^ 0x71U));
    b.box(-3.2f, 3.2f, 0.0f, th, z0 - 1.5f, z0 + 5.0f, P.hall, true);
    b.box(-3.5f, 3.5f, th - 0.6f, th, z0 - 1.8f, z0 + 5.3f, P.stone);
    for (int s = 0; s < 2; ++s) {    // a clock face front and back
        const float z = s == 0 ? z0 - 1.56f : z0 + 5.06f;
        b.box(-1.2f, 1.2f, th - 4.6f, th - 2.2f, std::min(z, z + (s ? 0.04f : -0.04f)),
              std::max(z, z + (s ? 0.04f : -0.04f)), P.white);
        b.box(-0.08f, 0.08f, th - 3.4f, th - 2.4f, s ? z + 0.04f : z - 0.08f,
              s ? z + 0.08f : z - 0.04f, P.darkGrey);
    }
    b.pyramid(0.0f, z0 + 1.75f, 3.6f, 3.6f, th, th + 8.0f, P.copper);
    // Door and steps.
    b.box(-1.6f, 1.6f, 0.0f, 3.2f, z0 - 1.56f, z0 - 1.5f, P.wood);
    for (int i = 0; i < 3; ++i)
        b.box(-3.0f - i * 0.4f, 3.0f + i * 0.4f, 0.0f, 0.18f * (3 - i), z0 - 1.5f - (i + 1) * 0.35f,
              z0 - 1.5f, P.stone);
    // The square in front, and its flags.
    plaza(b, -0.5f * W + 2.0f, 0.5f * W - 2.0f, -0.5f * D + 1.0f, z0 - 2.6f, P);
    for (int i = -1; i <= 1; ++i) flag(b, i * 4.0f - 1.0f, -0.5f * D + 4.0f, P);
    b.top = std::max(b.top, th + 8.0f);
    return b.finish();
}

Model school(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    // Small blocks lose the gym and the pitch first; the L of classrooms stays.
    if (W < 44.0f || D < 38.0f) return {};
    const float fh = 3.5f, H = 3.0f * fh, wd = 12.0f;
    const float x0 = -0.5f * W + 3.0f;
    const float la = glm::clamp(W * 0.55f, 32.0f, 64.0f);
    const float z0 = -0.5f * D + 4.0f, z1 = z0 + wd;
    const AssetId facade = unit(hashU(h ^ 0x31U)) < 0.5f ? P.brickOffice : P.office;
    // The front wing, and a side wing going back from its left end: an L round
    // the schoolyard.
    b.box(x0, x0 + la, 0.0f, H, z0, z1, facade, true);
    b.box(x0 - 0.2f, x0 + la + 0.2f, H, H + 0.8f, z0 - 0.2f, z1 + 0.2f, P.concrete);
    const float lb = glm::clamp(D * 0.45f, 14.0f, 34.0f);
    b.box(x0, x0 + wd, 0.0f, H, z1, z1 + lb, facade, true);
    b.box(x0 - 0.2f, x0 + wd + 0.2f, H, H + 0.8f, z1, z1 + lb + 0.2f, P.concrete);
    // Entrance canopy and door on the street side.
    const float ex = x0 + 0.5f * la;
    b.box(ex - 4.0f, ex + 4.0f, 3.0f, 3.3f, z0 - 3.0f, z0, P.concrete);
    b.box(ex - 1.8f, ex + 1.8f, 0.0f, 2.8f, z0 - 0.06f, z0, P.darkGlass);
    // The schoolyard in the corner of the L.
    plaza(b, x0 + wd, x0 + la, z1, std::min(z1 + lb, 0.5f * D - 2.0f), P);
    // The gym at the back right: a big shed with a shallow roof.
    const float gx1 = 0.5f * W - 2.0f, gx0 = std::max(gx1 - 30.0f, x0 + la - 6.0f);
    const float gz1 = 0.5f * D - 2.0f, gz0 = std::max(gz1 - 18.0f, z1 + 4.0f);
    if (gx1 - gx0 > 14.0f && gz1 - gz0 > 10.0f) {
        b.box(gx0, gx1, 0.0f, 7.5f, gz0, gz1, P.metal, true);
        b.gable(gx0, gx1, gz0, gz1, 7.5f, 9.0f, false, P.metalRoof, P.metal, 0.3f);
    }
    // A sports pitch where there is still room: grass and white lines.
    const float px0 = x0 + wd + 6.0f, px1 = std::min(x0 + la, gx0 - 4.0f);
    const float pz0 = z1 + 6.0f, pz1 = 0.5f * D - 3.0f;
    if (px1 - px0 > 16.0f && pz1 - pz0 > 12.0f) {
        b.box(px0, px1, 0.07f, 0.1f, pz0, pz1, P.lawn);
        const float l = 0.12f, y0 = 0.1f, y1 = 0.12f;
        b.box(px0 + 1, px1 - 1, y0, y1, pz0 + 1, pz0 + 1 + l, P.white);
        b.box(px0 + 1, px1 - 1, y0, y1, pz1 - 1 - l, pz1 - 1, P.white);
        b.box(px0 + 1, px0 + 1 + l, y0, y1, pz0 + 1, pz1 - 1, P.white);
        b.box(px1 - 1 - l, px1 - 1, y0, y1, pz0 + 1, pz1 - 1, P.white);
        const float mx = 0.5f * (px0 + px1);
        b.box(mx - 0.5f * l, mx + 0.5f * l, y0, y1, pz0 + 1, pz1 - 1, P.white);
        for (float gx : {px0 + 1.0f, px1 - 1.0f}) {   // goals
            const float mz = 0.5f * (pz0 + pz1);
            b.box(gx - 0.05f, gx + 0.05f, 0.0f, 2.0f, mz - 2.5f, mz - 2.4f, P.white);
            b.box(gx - 0.05f, gx + 0.05f, 0.0f, 2.0f, mz + 2.4f, mz + 2.5f, P.white);
            b.box(gx - 0.05f, gx + 0.05f, 1.9f, 2.0f, mz - 2.5f, mz + 2.5f, P.white);
        }
    }
    return b.finish();
}

Model kindergarten(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 30.0f || D < 28.0f) return {};
    const float bw = std::min(24.0f, W - 6.0f), bd = 11.0f, H = 3.4f;
    const float x0 = -0.5f * W + 3.0f, z0 = -0.5f * D + 5.0f;
    // Grass over the whole plot, the playground on it.
    b.box(-0.5f * W + 1.0f, 0.5f * W - 1.0f, 0.0f, 0.04f, -0.5f * D + 1.0f, 0.5f * D - 1.0f, P.lawn);
    b.box(x0, x0 + bw, 0.0f, H, z0, z0 + bd, P.kinder, true);
    b.gable(x0, x0 + bw, z0, z0 + bd, H, H + 3.0f, false, P.tile, P.kinder, 0.6f);
    for (int i = 0; i < 5; ++i) {   // big low windows on the garden side and the front
        const float wx = x0 + 2.0f + i * (bw - 4.0f) / 4.0f;
        b.box(wx - 1.0f, wx + 1.0f, 0.5f, 2.4f, z0 - 0.06f, z0, P.darkGlass);
        b.box(wx - 1.0f, wx + 1.0f, 0.5f, 2.4f, z0 + bd, z0 + bd + 0.06f, P.darkGlass);
    }
    b.box(x0 + 0.5f * bw - 1.0f, x0 + 0.5f * bw + 1.0f, 0.0f, 2.4f, z0 - 0.07f, z0 - 0.01f, P.fireRed);
    // Behind it: a sandpit, a swing, a climbing frame.
    const float gz = z0 + bd + 4.0f;
    const float sx = x0 + 3.0f;
    if (gz + 6.0f < 0.5f * D - 1.0f) {
        b.box(sx, sx + 5.0f, 0.0f, 0.3f, gz, gz + 5.0f, P.wood, true);
        b.box(sx + 0.2f, sx + 4.8f, 0.3f, 0.32f, gz + 0.2f, gz + 4.8f, P.sand);
        const float wx = sx + 9.0f;
        for (float dx : {0.0f, 3.2f}) {
            b.cylinder(wx + dx, gz + 1.0f, 0.08f, 0.0f, 2.6f, P.metal, 6);
            b.cylinder(wx + dx, gz + 3.0f, 0.08f, 0.0f, 2.6f, P.metal, 6);
        }
        b.box(wx - 0.1f, wx + 3.3f, 2.5f, 2.65f, gz + 0.95f, gz + 1.05f, P.metal);
        b.box(wx - 0.1f, wx + 3.3f, 2.5f, 2.65f, gz + 2.95f, gz + 3.05f, P.metal);
        for (float dx : {0.9f, 2.3f}) b.box(wx + dx - 0.25f, wx + dx + 0.25f, 0.45f, 0.5f, gz + 1.8f, gz + 2.2f, P.fireRed);
        const float cx = wx + 7.0f;
        if (cx + 3.0f < 0.5f * W - 1.0f) {
            for (float dx : {0.0f, 3.0f})
                for (float dz : {0.0f, 3.0f}) b.cylinder(cx + dx, gz + dz, 0.08f, 0.0f, 2.2f, P.fireRed, 6);
            b.box(cx - 0.1f, cx + 3.1f, 1.2f, 1.3f, gz - 0.1f, gz + 3.1f, P.wood);
            b.box(cx - 0.1f, cx + 3.1f, 2.1f, 2.2f, gz - 0.1f, gz + 3.1f, P.policeBlue);
        }
    }
    return b.finish();
}

Model library(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    const float bw = glm::clamp(W * 0.55f, 26.0f, 44.0f), bd = glm::clamp(D * 0.45f, 18.0f, 30.0f);
    if (W < bw + 6.0f || D < bd + 12.0f) return {};
    const int floors = 2 + static_cast<int>(hashU(h ^ 0x41U) % 2U);
    const float H = floors * 4.2f;
    const float x0 = -0.5f * bw, x1 = 0.5f * bw;
    const float z0 = -0.5f * D + 10.0f, z1 = z0 + bd;
    b.box(x0, x1, 0.0f, H, z0, z1, P.white, true);
    // A glass front the full width and height, set in the white box.
    b.box(x0 + 1.0f, x1 - 1.0f, 0.4f, H - 0.6f, z0 - 0.08f, z0, P.darkGlass);
    for (int f = 1; f < floors; ++f)   // the floor slabs showing through it
        b.box(x0 + 1.0f, x1 - 1.0f, f * 4.2f - 0.3f, f * 4.2f, z0 - 0.12f, z0 - 0.04f, P.white);
    // The roof slab reaches out over the entrance.
    b.box(x0 - 1.0f, x1 + 1.0f, H, H + 0.6f, z0 - 4.0f, z1 + 1.0f, P.concrete);
    for (float x : {x0 + 1.5f, x1 - 1.5f}) b.cylinder(x, z0 - 3.4f, 0.18f, 0.0f, H, P.concrete, 8);
    plaza(b, -0.5f * W + 2.0f, 0.5f * W - 2.0f, -0.5f * D + 1.0f, z0 - 0.1f, P);
    for (int i = 0; i < 3; ++i) {      // benches on the square
        const float bx = x0 + 4.0f + i * (bw - 8.0f) / 2.0f;
        b.box(bx - 1.0f, bx + 1.0f, 0.4f, 0.5f, z0 - 7.0f, z0 - 6.5f, P.wood);
    }
    return b.finish();
}

Model museum(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    const float bw = glm::clamp(W * 0.55f, 28.0f, 46.0f), bd = glm::clamp(D * 0.5f, 22.0f, 36.0f);
    if (W < bw + 6.0f || D < bd + 14.0f) return {};
    const float x0 = -0.5f * bw, x1 = 0.5f * bw;
    const float z0 = -0.5f * D + 12.0f, z1 = z0 + bd;
    const float base = 1.5f, H = base + 11.0f;
    // The stylobate: three broad steps up to the hall.
    for (int i = 0; i < 3; ++i) {
        const float g = 1.2f * (3 - i);
        b.box(x0 - g, x1 + g, 0.5f * i, 0.5f * (i + 1), z0 - 7.0f + 1.2f * i, z1 + g, P.stone);
    }
    b.box(x0, x1, base, H, z0, z1, P.stone, true);
    // The portico: a row of columns carrying the entablature and the pediment.
    const float pz0 = z0 - 6.0f;
    const int cols = std::max(4, static_cast<int>(bw / 5.0f) & ~1);
    for (int i = 0; i < cols; ++i) {
        const float cx = x0 + 1.5f + i * (bw - 3.0f) / (cols - 1);
        b.cylinder(cx, pz0 + 1.0f, 0.65f, base, H - 1.2f, P.white, 12);
    }
    b.box(x0, x1, H - 1.2f, H, pz0, z0, P.stone);
    b.gable(x0, x1, pz0, z1, H, H + 0.22f * bw, true, P.copper, P.stone, 0.3f);
    // A dome over the middle of the hall.
    const float dr = std::min(bw, bd) * 0.22f;
    b.cylinder(0.0f, 0.5f * (z0 + z1) + 2.0f, dr, H, H + 0.22f * bw + 1.0f, P.stone, 16);
    b.dome(0.0f, 0.5f * (z0 + z1) + 2.0f, dr, H + 0.22f * bw + 1.0f, P.copper, 16, 5);
    b.box(-1.8f, 1.8f, base, base + 4.5f, z0 - 0.06f, z0, P.wood);
    plaza(b, -0.5f * W + 2.0f, 0.5f * W - 2.0f, -0.5f * D + 1.0f, z0 - 7.2f, P);
    (void)h;
    return b.finish();
}

Model theatre(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 40.0f || D < 44.0f) return {};
    const float hw = glm::clamp(W * 0.55f, 28.0f, 44.0f);
    const float x0 = -0.5f * hw, x1 = 0.5f * hw;
    const float z0 = -0.5f * D + 9.0f;
    // Foyer: low and glazed along the street.
    const float fz1 = z0 + 9.0f;
    b.box(x0, x1, 0.0f, 7.0f, z0, fz1, P.hall, true);
    b.box(x0 + 1.0f, x1 - 1.0f, 0.3f, 6.2f, z0 - 0.08f, z0, P.darkGlass);
    b.box(x0 - 0.3f, x1 + 0.3f, 7.0f, 7.6f, z0 - 0.3f, fz1, P.stone);
    // The canopy and its name band.
    b.box(x0 + 3.0f, x1 - 3.0f, 3.8f, 4.2f, z0 - 4.0f, z0, P.fireRed);
    b.box(x0 + 3.0f, x1 - 3.0f, 4.2f, 5.0f, z0 - 4.05f, z0 - 3.9f, P.gold);
    // Auditorium behind, and the fly tower over the stage behind that.
    const float az1 = std::min(fz1 + 26.0f, 0.5f * D - 16.0f);
    b.box(x0, x1, 0.0f, 15.0f, fz1, az1, P.hall, true);
    b.gable(x0, x1, fz1, az1, 15.0f, 18.0f, true, P.slate, P.hall, 0.3f);
    const float tz1 = std::min(az1 + 14.0f, 0.5f * D - 2.0f);
    b.box(x0 + 3.0f, x1 - 3.0f, 0.0f, 24.0f + 3.0f * unit(hashU(h ^ 0x61U)), az1, tz1, P.stone, true);
    plaza(b, -0.5f * W + 2.0f, 0.5f * W - 2.0f, -0.5f * D + 1.0f, z0 - 4.2f, P);
    return b.finish();
}

Model pool(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    // A small block keeps the big pool and loses the children's one.
    if (W < 40.0f || D < 38.0f) return {};
    // The lawn, then the building by the street: entrance and changing rooms.
    b.box(-0.5f * W + 1.0f, 0.5f * W - 1.0f, 0.0f, 0.04f, -0.5f * D + 1.0f, 0.5f * D - 1.0f, P.lawn);
    const float bz0 = -0.5f * D + 3.0f, bz1 = bz0 + 8.0f;
    const float bw = std::min(26.0f, W - 8.0f);
    b.box(-0.5f * bw, 0.5f * bw, 0.0f, 3.6f, bz0, bz1, P.white, true);
    b.box(-0.5f * bw - 0.4f, 0.5f * bw + 0.4f, 3.6f, 3.9f, bz0 - 0.4f, bz1 + 0.4f, P.policeBlue);
    b.box(-1.8f, 1.8f, 0.0f, 2.6f, bz0 - 0.06f, bz0, P.darkGlass);
    // A basin: a white rim, the water a hand below it.
    auto basin = [&](float x0, float x1, float z0, float z1) {
        const float r = 1.5f;
        b.box(x0 - r, x1 + r, 0.04f, 0.3f, z0 - r, z0, P.white);
        b.box(x0 - r, x1 + r, 0.04f, 0.3f, z1, z1 + r, P.white);
        b.box(x0 - r, x0, 0.04f, 0.3f, z0, z1, P.white);
        b.box(x1, x1 + r, 0.04f, 0.3f, z0, z1, P.white);
        b.box(x0, x1, 0.04f, 0.15f, z0, z1, P.poolWater);
    };
    const float lw = std::min(25.0f, W - 14.0f), lz0 = bz1 + 6.0f;
    const float lz1 = std::min(lz0 + 12.5f, 0.5f * D - 6.0f);
    const float lx0 = -0.5f * W + 5.0f;
    basin(lx0, lx0 + lw, lz0, lz1);
    // Lanes on the bottom, seen through the water.
    for (int i = 1; i < 5; ++i) {
        const float z = lz0 + i * (lz1 - lz0) / 5.0f;
        b.box(lx0 + 1.0f, lx0 + lw - 1.0f, 0.151f, 0.155f, z - 0.1f, z + 0.1f, P.policeBlue);
    }
    // A diving tower at the far end, and a children's pool beside the big one.
    const float tx = lx0 + lw + 0.2f, tz = 0.5f * (lz0 + lz1);
    if (tx + 3.0f < 0.5f * W - 1.0f) {
        b.box(tx, tx + 2.5f, 0.0f, 5.0f, tz - 1.2f, tz + 1.2f, P.concrete, true);
        b.box(tx - 2.5f, tx + 2.5f, 4.8f, 5.0f, tz - 1.0f, tz + 1.0f, P.concrete);
        b.box(tx - 1.5f, tx + 2.5f, 2.8f, 3.0f, tz - 0.9f, tz + 0.9f, P.concrete);
    }
    const float kx0 = lx0 + lw + 6.0f, kz0 = lz1 + 5.0f;
    if (kx0 + 7.0f < 0.5f * W - 3.0f && kz0 + 7.0f < 0.5f * D - 3.0f)
        basin(kx0, kx0 + 7.0f, kz0, kz0 + 7.0f);
    // Sunshades on the lawn.
    for (int i = 0; i < 3; ++i) {
        const float sx = lx0 + 4.0f + i * 7.0f, sz = std::min(lz1 + 6.0f, 0.5f * D - 4.0f);
        b.cylinder(sx, sz, 0.05f, 0.0f, 2.3f, P.white, 6);
        b.cone(sx, sz, 1.4f, 2.1f, 2.6f, i % 2 ? P.fireRed : P.kinder, 8);
    }
    (void)h;
    return b.finish();
}


// --- Supply and transport -----------------------------------------------------------

Model petrolStation(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 40.0f || D < 34.0f) return {};
    const float x0 = -0.5f * W + 2.0f, z0 = -0.5f * D + 2.0f;
    // Grass round it; the forecourt in asphalt.
    b.box(-0.5f * W + 1.0f, 0.5f * W - 1.0f, 0.0f, 0.04f, -0.5f * D + 1.0f, 0.5f * D - 1.0f, P.lawn);
    const float fw = std::min(38.0f, W - 4.0f), fd = std::min(30.0f, D - 4.0f);
    b.box(x0, x0 + fw, 0.04f, 0.08f, z0, z0 + fd, P.asphalt);
    // The canopy on its pillars, the brand's colour round its edge.
    const float cx0 = x0 + 4.0f, cx1 = x0 + fw - 12.0f, cz0 = z0 + 4.0f, cz1 = z0 + 16.0f;
    b.box(cx0, cx1, 5.0f, 5.9f, cz0, cz1, P.white);
    b.box(cx0 - 0.05f, cx1 + 0.05f, 5.0f, 5.5f, cz0 - 0.05f, cz1 + 0.05f, P.fuel);
    for (float px : {cx0 + 2.0f, 0.5f * (cx0 + cx1), cx1 - 2.0f})
        for (float pz : {cz0 + 3.0f, cz1 - 3.0f})
            b.box(px - 0.25f, px + 0.25f, 0.0f, 5.0f, pz - 0.25f, pz + 0.25f, P.white, true);
    // Three pump islands across it, a car at some of them.
    for (int i = 0; i < 3; ++i) {
        const float ix = cx0 + 3.0f + i * (cx1 - cx0 - 6.0f) / 2.0f, iz = 0.5f * (cz0 + cz1);
        b.box(ix - 0.6f, ix + 0.6f, 0.0f, 0.2f, iz - 2.5f, iz + 2.5f, P.concrete);
        for (float dz : {-1.2f, 1.2f})
            b.box(ix - 0.3f, ix + 0.3f, 0.2f, 1.9f, iz + dz - 0.4f, iz + dz + 0.4f, P.fuel);
        if (hashU(h ^ (0x51U + i)) % 2U)
            b.vehicle(ix + 2.2f, iz, 4.3f, 1.8f, 1.5f, i % 2 ? P.policeBlue : P.fireRed, P.white,
                      P.darkGlass, false);
    }
    // The shop at the back of the forecourt, glass to the pumps.
    const float sx0 = x0 + fw - 10.0f, sx1 = x0 + fw, sz0 = z0 + 6.0f, sz1 = z0 + 20.0f;
    b.box(sx0, sx1, 0.0f, 4.0f, sz0, sz1, P.white, true);
    b.box(sx0 - 0.08f, sx0, 0.3f, 3.2f, sz0 + 1.0f, sz1 - 1.0f, P.darkGlass);
    b.box(sx0 - 0.1f, sx1 + 0.1f, 4.0f, 4.6f, sz0 - 0.1f, sz1 + 0.1f, P.fuel);
    // The price pole at the street, three prices lettered in the street signs' face.
    const float px = x0 + 1.5f, pz = z0 + 0.8f;
    b.box(px - 1.0f, px + 1.0f, 0.0f, 7.0f, pz - 0.25f, pz + 0.25f, P.fuel, true);
    streetsign::Style st;
    st.frame = streetsign::Frame::None;
    const char* prices[3] = {"1,79", "1,69", "1,89"};
    for (int i = 0; i < 3; ++i) {
        const streetsign::Face f = streetsign::layout(prices[i], st, 0.42f);
        const float y = 5.8f - i * 1.3f;
        for (int side = -1; side <= 1; side += 2) {
            const float z = pz + side * 0.26f;
            b.box(px - 0.85f, px + 0.85f, y - 0.5f, y + 0.5f, std::min(z, z + side * 0.01f),
                  std::max(z, z + side * 0.01f), P.white);
            for (const streetsign::Poly& p : f.ink) {
                std::vector<glm::vec3> q;
                for (const glm::vec2& v : p.pts)
                    q.push_back({px + (side < 0 ? -v.x : v.x), y + v.y, z + side * 0.015f});
                b.face(q, {0.0f, 0.0f, static_cast<float>(side)}, P.darkGrey);
            }
        }
    }
    return b.finish();
}

Model powerPlant(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 60.0f || D < 50.0f) return {};
    const float x0 = -0.5f * W + 3.0f, x1 = 0.5f * W - 3.0f;
    const float z0 = -0.5f * D + 4.0f, z1 = 0.5f * D - 3.0f;
    // Turbine hall along the street, the boiler house rising behind one end.
    const float hw = std::min(55.0f, (x1 - x0) * 0.6f), hz1 = z0 + std::min(22.0f, (z1 - z0) * 0.4f);
    b.box(x0, x0 + hw, 0.0f, 24.0f, z0, hz1, P.metal, true);
    b.gable(x0, x0 + hw, z0, hz1, 24.0f, 26.5f, false, P.metalRoof, P.metal, 0.3f);
    const float bx1 = x0 + std::min(24.0f, hw * 0.45f);
    b.box(x0, bx1, 0.0f, 46.0f, hz1, std::min(hz1 + 20.0f, z1), P.brick, true);
    // Two cooling towers behind: a hyperboloid shell as a stack of frustums.
    const float r0 = glm::clamp(std::min(W, D) * 0.14f, 9.0f, 26.0f), tH = 2.4f * r0;
    const float tz = z1 - r0 - 1.0f;
    for (int t = 0; t < 2; ++t) {
        const float tx = x1 - r0 - 1.0f - t * (2.0f * r0 + 4.0f);
        if (tx - r0 < bx1 + 2.0f) break;
        const int n = 6;
        for (int i = 0; i < n; ++i) {
            auto rad = [&](float u) {   // waist at 70 % height
                const float k = (u - 0.7f) / 0.7f;
                return r0 * (0.62f + 0.38f * k * k);
            };
            const float u0 = static_cast<float>(i) / n, u1 = static_cast<float>(i + 1) / n;
            b.frustum(tx, tz, rad(u0), rad(u1), u0 * tH, u1 * tH, P.concrete, 20);
        }
        b.solid(tx - 0.7f * r0, tx + 0.7f * r0, 0.0f, tH, tz - 0.7f * r0, tz + 0.7f * r0);
        b.top = std::max(b.top, tH);
    }
    // The chimney, red and white.
    const float chx = bx1 + 6.0f, chz = hz1 + 6.0f;
    const float ch = 70.0f + 30.0f * unit(hashU(h ^ 0x91U));
    for (int k = 0; k < 10; ++k)
        b.cylinder(chx, chz, 2.6f - 0.12f * k, ch * k / 10.0f, ch * (k + 1) / 10.0f,
                   k % 2 == 0 ? P.chimneyRed : P.white, 12, k == 9, false);
    b.solid(chx - 2.0f, chx + 2.0f, 0.0f, ch, chz - 2.0f, chz + 2.0f);
    // The switchyard by the street: transformers under a gantry.
    const float yx0 = x0 + hw + 4.0f, yx1 = std::min(yx0 + 26.0f, x1);
    if (yx1 - yx0 > 12.0f) {
        for (float x = yx0 + 3.0f; x < yx1 - 2.0f; x += 6.0f) {
            b.box(x - 1.5f, x + 1.5f, 0.0f, 3.0f, z0 + 3.0f, z0 + 6.0f, P.darkGrey, true);
            b.beam({x, 0.0f, z0 + 8.0f}, {x, 12.0f, z0 + 8.0f}, 0.4f, P.steel);
        }
        b.beam({yx0 + 3.0f, 12.0f, z0 + 8.0f}, {yx1 - 2.0f, 12.0f, z0 + 8.0f}, 0.5f, P.steel);
    }
    return b.finish();
}

Model landfill(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 50.0f || D < 45.0f) return {};
    const float x0 = -0.5f * W + 3.0f, x1 = 0.5f * W - 3.0f;
    const float z0 = -0.5f * D + 10.0f, z1 = 0.5f * D - 3.0f;
    // The heap in terraces, the lower ones grassed over, the working top raw.
    const int levels = 3;
    const float lh = 5.0f + 2.0f * unit(hashU(h ^ 0x81U));
    for (int i = 0; i < levels; ++i) {
        const float g = 6.0f * i;
        const bool top = i == levels - 1;
        b.mound(x0 + g, x1 - g, z0 + g, z1 - g, 4.0f, i * lh, (i + 1) * lh, P.earth,
                top ? P.rubbish : P.lawn);
    }
    b.solid(x0 + 4.0f, x1 - 4.0f, 0.0f, levels * lh * 0.8f, z0 + 4.0f, z1 - 4.0f);
    // The ramp up the front, a truck on it and one on top.
    b.face({{x0 + 6.0f, 0.05f, z0 - 0.5f}, {x0 + 12.0f, 0.05f, z0 - 0.5f},
            {x0 + 12.0f, lh + 0.05f, z0 + 4.0f}, {x0 + 6.0f, lh + 0.05f, z0 + 4.0f}},
           {0.0f, 1.0f, -0.6f}, P.gravel);
    b.vehicle(0.5f * (x0 + x1), 0.5f * (z0 + z1), 8.0f, 2.5f, 3.0f, P.kinder, P.darkGrey, P.darkGlass);
    b.vehicle(x0 + 16.0f, z0 - 4.0f, 8.0f, 2.5f, 3.0f, P.kinder, P.darkGrey, P.darkGlass);
    // The weighbridge office by the gate, and the fence along the street.
    b.box(x1 - 10.0f, x1 - 4.0f, 0.0f, 2.8f, z0 - 8.0f, z0 - 5.0f, P.white, true);
    b.box(x0 - 1.0f, x1 + 1.0f, 0.0f, 2.0f, -0.5f * D + 1.5f, -0.5f * D + 1.55f, P.steel);
    b.box(x0 + 5.0f, x0 + 13.0f, 0.0f, 0.02f, -0.5f * D + 1.0f, z0, P.gravel);
    return b.finish();
}

Model station(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 70.0f || D < 55.0f) return {};
    const float x0 = -0.5f * W + 2.0f, x1 = 0.5f * W - 2.0f;
    // The station building along the street: two wings, a taller hall in the
    // middle under a barrel roof, a clock tower at one end.
    const float z0 = -0.5f * D + 12.0f, z1 = z0 + 16.0f;
    const float hx0 = -12.0f, hx1 = 12.0f;
    b.box(x0 + 6.0f, hx0, 0.0f, 12.0f, z0, z1, P.hall, true);
    b.box(hx1, x1 - 6.0f, 0.0f, 12.0f, z0, z1, P.hall, true);
    b.gable(x0 + 6.0f, hx0, z0, z1, 12.0f, 16.0f, false, P.slate, P.hall, 0.4f);
    b.gable(hx1, x1 - 6.0f, z0, z1, 12.0f, 16.0f, false, P.slate, P.hall, 0.4f);
    b.box(hx0, hx1, 0.0f, 16.0f, z0, z1, P.stone, true);
    const int seg = 10;
    for (int i = 0; i < seg; ++i) {      // the barrel roof over the hall, along z
        const float a0 = kPi * i / seg, a1 = kPi * (i + 1) / seg;
        const float xa = -12.0f * std::cos(a0), ya = 16.0f + 7.0f * std::sin(a0);
        const float xb = -12.0f * std::cos(a1), yb = 16.0f + 7.0f * std::sin(a1);
        b.face({{xa, ya, z0}, {xb, yb, z0}, {xb, yb, z1}, {xa, ya, z1}},
               {-std::cos(0.5f * (a0 + a1)), std::sin(0.5f * (a0 + a1)), 0.0f}, P.copper);
    }
    // The hall's front: an arched window over the doors, and its gable end.
    b.box(-8.0f, 8.0f, 2.0f, 14.0f, z0 - 0.08f, z0, P.darkGlass);
    std::vector<glm::vec3> gable{{-12.0f, 16.0f, z0}};
    for (int i = 0; i <= seg; ++i) {
        const float a = kPi * i / seg;
        gable.push_back({-12.0f * std::cos(a), 16.0f + 7.0f * std::sin(a), z0});
    }
    b.face(gable, {0, 0, -1}, P.stone);
    const float tx = x0 + 3.0f;
    b.box(tx - 3.0f, tx + 3.0f, 0.0f, 30.0f, z0, z0 + 6.0f, P.hall, true);
    b.pyramid(tx, z0 + 3.0f, 3.3f, 3.3f, 30.0f, 36.0f, P.copper);
    b.box(tx - 1.3f, tx + 1.3f, 24.0f, 26.6f, z0 - 0.06f, z0, P.white);
    b.box(tx - 0.08f, tx + 0.08f, 24.6f, 25.8f, z0 - 0.1f, z0 - 0.06f, P.darkGrey);
    // The forecourt with taxis waiting.
    b.box(x0, x1, 0.0f, 0.06f, -0.5f * D + 1.0f, z0, P.pavement);
    for (int i = 0; i < 4; ++i)
        b.vehicle(hx1 + 6.0f + i * 5.5f, z0 - 4.0f, 4.4f, 1.8f, 1.5f, P.hall, P.darkGrey, P.darkGlass);
    // Behind: platforms and tracks across the whole block, under a train shed.
    const float tz0 = z1 + 3.0f, tz1 = 0.5f * D - 2.0f;
    const int tracks = std::max(2, std::min(6, static_cast<int>((tz1 - tz0) / 8.0f) * 2 - 1));
    const float pitch = (tz1 - tz0) / static_cast<float>(tracks);
    for (int i = 0; i < tracks; ++i) {
        const float cz = tz0 + (i + 0.5f) * pitch;
        if (i % 2 == 0) {                // a track: ballast and two rails
            b.box(x0, x1, 0.0f, 0.25f, cz - 1.6f, cz + 1.6f, P.gravel);
            for (float s : {-0.72f, 0.72f})
                b.box(x0, x1, 0.25f, 0.42f, cz + s - 0.04f, cz + s + 0.04f, P.rail);
        } else {                         // a platform
            b.box(x0 + 4.0f, x1 - 4.0f, 0.0f, 0.8f, cz - 0.5f * pitch + 1.8f, cz + 0.5f * pitch - 1.8f,
                  P.concrete, true);
        }
    }
    // A train standing at the first track.
    {
        const float cz = tz0 + 0.5f * pitch;
        for (int c = 0; c < 3; ++c) {
            const float cx = x0 + 10.0f + c * 21.0f;
            if (cx + 20.0f > x1) break;
            b.box(cx, cx + 20.0f, 0.5f, 4.2f, cz - 1.45f, cz + 1.45f, P.trainRed, true);
            b.box(cx + 0.5f, cx + 19.5f, 2.2f, 3.3f, cz - 1.47f, cz + 1.47f, P.darkGlass);
            b.box(cx, cx + 20.0f, 1.1f, 1.4f, cz - 1.47f, cz + 1.47f, P.white);
        }
    }
    // The shed: a curved roof on columns over all of it.
    const float span = tz1 - tz0, rise = std::min(9.0f, 0.3f * span);
    for (int i = 0; i < seg; ++i) {
        const float a0 = kPi * i / seg, a1 = kPi * (i + 1) / seg;
        auto pt = [&](float a) {
            return glm::vec2(0.5f * (tz0 + tz1) - 0.5f * span * std::cos(a), 9.0f + rise * std::sin(a));
        };
        const glm::vec2 p = pt(a0), q = pt(a1);
        const glm::vec3 n(0.0f, std::sin(0.5f * (a0 + a1)), -std::cos(0.5f * (a0 + a1)));
        const AssetId m = i == seg / 2 || i == seg / 2 - 1 ? P.darkGlass : P.metalRoof;
        b.face({{x0 + 4.0f, p.y, p.x}, {x1 - 4.0f, p.y, p.x}, {x1 - 4.0f, q.y, q.x}, {x0 + 4.0f, q.y, q.x}}, n, m);
        b.face({{x0 + 4.0f, p.y, p.x}, {x0 + 4.0f, q.y, q.x}, {x1 - 4.0f, q.y, q.x}, {x1 - 4.0f, p.y, p.x}}, -n, m);
    }
    for (float x = x0 + 6.0f; x < x1 - 4.0f; x += 16.0f)
        for (float z : {tz0 + 0.3f, tz1 - 0.3f}) b.cylinder(x, z, 0.3f, 0.0f, 9.0f, P.steel, 8, true, true);
    b.top = std::max(b.top, 9.0f + rise);
    (void)h;
    return b.finish();
}

// --- Shopping and going out ---------------------------------------------------------

// A car parked in a bay, its colour picked by `h`.
void parkedCar(Builder& b, float x, float z, std::uint32_t h, const Palette& P, bool alongZ = true) {
    const AssetId colours[6] = {P.fireRed, P.policeBlue, P.white, P.darkGrey, P.kinder, P.fuel};
    const AssetId c = colours[hashU(h) % 6U];
    b.vehicle(x, z, 4.3f, 1.8f, 1.45f, c, c, P.darkGlass, !alongZ);
}

Model supermarket(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 40.0f || D < 36.0f) return {};
    static const char* const kBrands[] = {"FRISCHMARKT", "KAUFGUT", "TAGESFRISCH", "VOLLKORB",
                                          "GUTE WAHL"};
    const char* brandName = kBrands[hashU(h ^ 0xa1U) % 5U];
    const AssetId brands[4] = {P.fireRed, P.policeBlue, P.kinder, P.fuel};
    const AssetId brand = brands[hashU(h ^ 0xa2U) % 4U];
    const AssetId onBrand = brand == P.kinder ? P.fireRed : P.neon;
    // The market at the back of the plot, the car park between it and the street.
    const float bw = std::min(W - 8.0f, 62.0f), bd = glm::clamp(D * 0.42f, 18.0f, 34.0f);
    const float H = 6.5f;
    const float z1 = 0.5f * D - 3.0f, z0 = z1 - bd;
    const float x0 = -0.5f * bw, x1 = 0.5f * bw;
    b.box(x0, x1, 0.0f, H, z0, z1, P.white, true);
    b.box(x0 - 0.3f, x1 + 0.3f, H, H + 0.4f, z0 - 0.3f, z1 + 0.3f, P.metalRoof);
    // The glazed entrance end, under a canopy on posts.
    const float ex0 = x0 + bw * 0.52f, ex1 = x1 - 2.0f;
    b.box(ex0, ex1, 0.1f, 3.6f, z0 - 0.06f, z0, P.darkGlass);
    b.box(ex0, ex1, 3.6f, 3.85f, z0 - 3.2f, z0, P.metalRoof);
    for (float x = ex0 + 0.5f; x < ex1; x += 6.0f) b.box(x - 0.12f, x + 0.12f, 0.0f, 3.6f, z0 - 3.1f, z0 - 2.86f, P.metal);
    // The fascia band in the brand's colour, the name on it.
    b.box(x0, x1, H - 1.9f, H + 0.4f, z0 - 0.25f, z0, brand);
    letter(b, brandName, 1.05f, bw * 0.48f, x0 + bw * 0.27f, H - 0.75f, z0 - 0.26f, onBrand);
    // The car park: asphalt, white bay lines, cars in some of the bays.
    const float pz0 = -0.5f * D + 1.0f, pz1 = z0 - 3.4f;
    b.box(-0.5f * W + 1.0f, 0.5f * W - 1.0f, 0.0f, 0.05f, pz0, z0, P.asphalt);
    const float bay = 2.6f, bayD = 5.0f;
    const int bays = static_cast<int>((W - 10.0f) / bay);
    const float bx0 = -0.5f * bay * static_cast<float>(bays);
    for (int row = 0; row < 2; ++row) {
        const float rz0 = row == 0 ? pz0 + 1.5f : pz1 - bayD;   // front row by the street, back row by the market
        if (row == 1 && rz0 < pz0 + 1.5f + bayD + 6.0f) break;  // no room for an aisle
        for (int i = 0; i <= bays; ++i) {
            const float x = bx0 + i * bay;
            b.box(x - 0.06f, x + 0.06f, 0.05f, 0.06f, rz0, rz0 + bayD, P.white);
            if (i < bays && hashU(h ^ (0x3000U + static_cast<std::uint32_t>(row * 97 + i))) % 5U < 2U)
                parkedCar(b, x + 0.5f * bay, rz0 + 0.5f * bayD, h ^ static_cast<std::uint32_t>(i * 31 + row), P);
        }
    }
    // A trolley shelter beside the entrance.
    {
        const float tx0 = x1 - 9.0f, tx1 = x1 - 4.0f, tz0 = z0 - 9.0f, tz1 = z0 - 6.8f;
        b.box(tx0, tx1, 2.2f, 2.32f, tz0, tz1, P.metal);
        for (float x : {tx0 + 0.1f, tx1 - 0.1f}) b.box(x - 0.05f, x + 0.05f, 0.0f, 2.2f, tz1 - 0.15f, tz1 - 0.05f, P.metal);
        for (int i = 0; i < 2; ++i)
            b.box(tx0 + 0.5f, tx1 - 0.5f, 0.15f, 1.0f, tz0 + 0.3f + i * 1.0f, tz0 + 0.9f + i * 1.0f, P.steel);
    }
    // The totem by the street: the name at the top, the P under it.
    {
        const float tx = -0.5f * W + 3.2f, tz = -0.5f * D + 1.8f;
        b.box(tx - 0.8f, tx + 0.8f, 0.0f, 6.0f, tz - 0.25f, tz + 0.25f, brand, true);
        letter(b, brandName, 0.26f, 1.4f, tx, 5.4f, tz - 0.26f, onBrand);
        b.box(tx - 0.55f, tx + 0.55f, 3.2f, 4.3f, tz - 0.27f, tz - 0.25f, P.policeBlue);
        letter(b, "P", 0.75f, 0.0f, tx, 3.75f, tz - 0.28f, P.white);
    }
    return b.finish();
}

Model cinema(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 36.0f || D < 34.0f) return {};
    static const char* const kNames[] = {"CAPITOL", "APOLLO", "ODEON", "FILMPALAST", "UNION",
                                         "LICHTSPIELE", "ROXY"};
    const char* name = kNames[hashU(h ^ 0xc1U) % 7U];
    const float bw = glm::clamp(W * 0.75f, 30.0f, 64.0f), bd = glm::clamp(D * 0.62f, 24.0f, 48.0f);
    const float H = 15.0f + 3.0f * unit(hashU(h ^ 0xc2U));
    const float z0 = -0.5f * D + 10.0f, z1 = std::min(z0 + bd, 0.5f * D - 2.0f);
    const float x0 = -0.5f * bw, x1 = 0.5f * bw;
    // The dark box of the halls, the glazed foyer along the street.
    b.box(x0, x1, 0.0f, H, z0, z1, P.darkGrey, true);
    b.box(x0 - 0.2f, x1 + 0.2f, H, H + 0.5f, z0 - 0.2f, z1 + 0.2f, P.concrete);
    b.box(x0 + 2.0f, x1 - 2.0f, 0.2f, 6.8f, z0 - 0.08f, z0, P.darkGlass);
    // The marquee over the doors: a red band carrying what is on.
    b.box(x0 + 4.0f, x1 - 4.0f, 4.2f, 4.5f, z0 - 4.5f, z0, P.white);
    b.box(x0 + 4.0f, x1 - 4.0f, 4.5f, 5.5f, z0 - 4.6f, z0 - 4.4f, P.fireRed);
    letter(b, "KINO 1 - 8", 0.55f, bw - 12.0f, 0.0f, 5.0f, z0 - 4.61f, P.neon);
    // The name across the top, big enough to read from the end of the street.
    letter(b, name, 3.0f, bw * 0.8f, 0.0f, H - 3.4f, z0 - 0.02f, P.neon);
    // Poster cases either side of the doors.
    const AssetId posters[4] = {P.kinder, P.policeBlue, P.fireRed, P.fuel};
    for (int i = 0; i < 4; ++i) {
        const float x = (i < 2 ? x0 + 2.5f + i * 2.2f : x1 - 4.5f - (i - 2) * 2.2f);
        b.box(x - 0.1f, x + 1.5f, 0.6f, 2.9f, z0 - 0.2f, z0 - 0.08f, P.darkGrey);
        b.box(x, x + 1.4f, 0.7f, 2.8f, z0 - 0.21f, z0 - 0.2f, posters[(i + hashU(h)) % 4U]);
    }
    plaza(b, -0.5f * W + 2.0f, 0.5f * W - 2.0f, -0.5f * D + 1.0f, z0 - 0.1f, P);
    return b.finish();
}

Model departmentStore(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 36.0f || D < 30.0f) return {};
    static const char* const kNames[] = {"M\xC3\x9C" "LLER", "SCHNEIDER", "BECKER", "HOFFMANN",
                                         "WEBER", "HERTEL"};
    const std::string name = std::string("KAUFHAUS ") + kNames[hashU(h ^ 0xd1U) % 6U];
    const int floors = 4 + static_cast<int>(hashU(h ^ 0xd2U) % 3U);
    const float fh = 4.2f, H = floors * fh;
    const float bw = glm::clamp(W * 0.86f, 30.0f, 84.0f), bd = glm::clamp(D * 0.78f, 24.0f, 64.0f);
    const float z0 = -0.5f * D + 4.0f, z1 = std::min(z0 + bd, 0.5f * D - 2.0f);
    const float x0 = -0.5f * bw, x1 = 0.5f * bw;
    b.box(x0, x1, 0.0f, H, z0, z1, P.office, true);
    // Shop windows all along the ground floor, a canopy over them.
    b.box(x0 + 1.0f, x1 - 1.0f, 0.3f, 3.6f, z0 - 0.08f, z0, P.darkGlass);
    b.box(x0, x1, 3.9f, 4.15f, z0 - 2.2f, z0, P.metalRoof);
    // A cornice, and the name on a dark band under it.
    b.box(x0 - 0.3f, x1 + 0.3f, H, H + 0.7f, z0 - 0.3f, z1 + 0.3f, P.stone);
    b.box(x0 + 2.0f, x1 - 2.0f, H - 3.3f, H - 0.6f, z0 - 0.12f, z0, P.darkGrey);
    letter(b, name, 1.5f, bw - 6.0f, 0.0f, H - 1.95f, z0 - 0.13f, P.neon);
    plaza(b, -0.5f * W + 1.0f, 0.5f * W - 1.0f, -0.5f * D + 1.0f, z0 - 0.1f, P);
    return b.finish();
}

Model parkingGarage(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 34.0f || D < 32.0f) return {};
    const int levels = 3 + static_cast<int>(hashU(h ^ 0xe1U) % 3U);
    const float lh = 2.9f, top = levels * lh;
    const float bw = glm::clamp(W * 0.88f, 30.0f, 72.0f), bd = glm::clamp(D * 0.82f, 28.0f, 60.0f);
    const float z0 = -0.5f * D + 4.0f, z1 = std::min(z0 + bd, 0.5f * D - 2.0f);
    const float x0 = -0.5f * bw, x1 = 0.5f * bw;
    // Open decks on columns, a parapet round each.
    b.box(x0, x1, 0.0f, 0.05f, z0, z1, P.asphalt);
    for (int L = 1; L <= levels; ++L) {
        const float y = L * lh;
        b.box(x0, x1, y - 0.3f, y, z0, z1, P.concrete);
        b.box(x0, x1, y, y + 1.0f, z0, z0 + 0.2f, P.concrete);
        b.box(x0, x1, y, y + 1.0f, z1 - 0.2f, z1, P.concrete);
        b.box(x0, x0 + 0.2f, y, y + 1.0f, z0 + 0.2f, z1 - 0.2f, P.concrete);
        b.box(x1 - 0.2f, x1, y, y + 1.0f, z0 + 0.2f, z1 - 0.2f, P.concrete);
    }
    for (float x = x0 + 0.3f; x <= x1 - 0.3f; x += 8.0f)
        for (float z = z0 + 0.3f; z <= z1 - 0.3f; z += 8.0f)
            b.box(x - 0.25f, x + 0.25f, 0.0f, top - 0.3f, z - 0.25f, z + 0.25f, P.concrete);
    // Cars on every deck, along the front where you can see them.
    for (int L = 0; L < levels; ++L) {
        const float y = L == 0 ? 0.05f : L * lh;
        for (int i = 0; i < static_cast<int>((bw - 4.0f) / 2.6f); ++i)
            if (hashU(h ^ (0x4400U + static_cast<std::uint32_t>(L * 131 + i))) % 3U == 0U) {
                Builder car;
                parkedCar(car, x0 + 2.0f + i * 2.6f + 1.3f, z0 + 3.2f, h ^ static_cast<std::uint32_t>(L * 7 + i), P);
                for (auto& [m, md] : car.parts) {
                    fitzel::MeshData& to = b.parts[m];
                    const auto base = static_cast<std::uint32_t>(to.vertices.size());
                    for (fitzel::Vertex v : md.vertices) { v.position.y += y; to.vertices.push_back(v); }
                    for (std::uint32_t idx : md.indices) to.indices.push_back(base + idx);
                }
            }
    }
    // The stair tower on the corner, with the blue P on it.
    const float sx0 = x1 - 5.0f;
    b.box(sx0, x1 + 0.5f, 0.0f, top + 3.0f, z0 - 0.5f, z0 + 5.0f, P.concrete, true);
    b.box(sx0 + 0.8f, x1 - 0.3f, top - 1.0f, top + 2.6f, z0 - 0.53f, z0 - 0.5f, P.policeBlue);
    letter(b, "P", 2.6f, 0.0f, 0.5f * (sx0 + 0.8f + x1 - 0.3f), top + 0.8f, z0 - 0.54f, P.white);
    letter(b, "PARKHAUS", 0.9f, bw - 12.0f, 0.5f * (x0 + sx0), lh + 0.5f, z0 - 0.01f, P.white);
    b.solid(x0, x1, 0.0f, top + 1.0f, z0, z1);
    b.top = std::max(b.top, top + 3.0f);
    return b.finish();
}

Model hotel(float W, float D, std::uint32_t h, const Palette& P) {
    Builder b;
    if (W < 30.0f || D < 26.0f) return {};
    static const char* const kNames[] = {"AM MARKT", "ZUR POST", "KAISERHOF", "ZUM ADLER",
                                         "GOLDENER HIRSCH", "STADTHOTEL", "AM PARK"};
    const char* suffix = kNames[hashU(h ^ 0xf1U) % 7U];
    const int floors = 6 + static_cast<int>(hashU(h ^ 0xf2U) % 4U);
    const float fh = 3.2f, H = floors * fh;
    const float bw = glm::clamp(W * 0.7f, 24.0f, 52.0f), bd = glm::clamp(D * 0.42f, 13.0f, 18.0f);
    const float z0 = -0.5f * D + 8.0f, z1 = z0 + bd;
    const float x0 = -0.5f * bw, x1 = 0.5f * bw;
    b.box(x0, x1, 0.0f, H, z0, z1, P.hall, true);
    b.box(x0 - 0.1f, x1 + 0.1f, 0.0f, fh, z0 - 0.1f, z1 + 0.1f, P.stone);
    b.box(x0 - 0.3f, x1 + 0.3f, H, H + 0.5f, z0 - 0.3f, z1 + 0.3f, P.stone);
    // The canopy out to the kerb, the name on its front, a revolving door under it.
    b.box(-4.0f, 4.0f, 3.3f, 3.6f, z0 - 5.5f, z0, P.darkGrey);
    b.box(-4.0f, 4.0f, 3.6f, 4.1f, z0 - 5.55f, z0 - 5.45f, P.darkGrey);
    letter(b, std::string("HOTEL ") + suffix, 0.3f, 7.4f, 0.0f, 3.85f, z0 - 5.56f, P.gold);
    for (float x : {-3.7f, 3.7f}) b.box(x - 0.08f, x + 0.08f, 0.0f, 3.3f, z0 - 5.4f, z0 - 5.24f, P.gold);
    b.box(-1.6f, 1.6f, 0.1f, 2.8f, z0 - 0.06f, z0, P.darkGlass);
    // HOTEL in big letters on a frame on the roof.
    b.box(x0 + 3.0f, x1 - 3.0f, H + 0.5f, H + 0.8f, z0 + 0.6f, z0 + 0.9f, P.steel);
    letter(b, "HOTEL", 2.2f, bw - 6.0f, 0.0f, H + 2.1f, z0 + 0.58f, P.neon);
    b.top = std::max(b.top, H + 3.4f);
    for (int i = -1; i <= 1; ++i) flag(b, i * 3.0f, -0.5f * D + 3.0f, P);
    plaza(b, -0.5f * W + 2.0f, 0.5f * W - 2.0f, -0.5f * D + 1.0f, z0 - 0.1f, P);
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
        case Kind::TownHall:    return "Town hall";
        case Kind::School:      return "School";
        case Kind::Kindergarten: return "Kindergarten";
        case Kind::Library:     return "Library";
        case Kind::Museum:      return "Museum";
        case Kind::Theatre:     return "Theatre";
        case Kind::Pool:        return "Swimming pool";
        case Kind::PetrolStation: return "Petrol station";
        case Kind::PowerPlant:  return "Power station";
        case Kind::Landfill:    return "Landfill";
        case Kind::Station:     return "Main station";
        case Kind::Supermarket: return "Supermarket";
        case Kind::Cinema:      return "Cinema";
        case Kind::DepartmentStore: return "Department store";
        case Kind::ParkingGarage:   return "Multi-storey car park";
        case Kind::Hotel:       return "Hotel";
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
    // Granite kerb stones: lighter than the slabs, so the edge reads as one.
    p.kerb       = ensure(mats, "Kerb Stone",  {0.52f, 0.51f, 0.49f}, 0.00f, 0.75f);
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
    p.hall       = ensure(mats, "Hall",        {0.86f, 0.81f, 0.70f}, 0.00f, 0.80f);
    p.lawn       = ensure(mats, "Lawn",        {0.10f, 0.19f, 0.06f}, 0.00f, 0.95f);
    p.sand       = ensure(mats, "Sand",        {0.72f, 0.62f, 0.44f}, 0.00f, 0.95f);
    p.poolWater  = ensure(mats, "Pool Water",  {0.05f, 0.42f, 0.55f}, 0.20f, 0.05f);
    p.kinder     = ensure(mats, "Kindergarten", {0.93f, 0.72f, 0.20f}, 0.00f, 0.75f);
    windows(mats, p.hall, {2.8f, 3.8f}, windowLit, 113.0f);
    p.asphalt    = ensure(mats, "Asphalt",     {0.07f, 0.07f, 0.075f}, 0.00f, 0.85f);
    p.fuel       = ensure(mats, "Fuel Brand",  {0.02f, 0.40f, 0.36f}, 0.10f, 0.40f);
    p.earth      = ensure(mats, "Earth",       {0.30f, 0.24f, 0.17f}, 0.00f, 0.95f);
    p.rubbish    = ensure(mats, "Rubbish",     {0.33f, 0.31f, 0.28f}, 0.00f, 0.90f);
    p.gravel     = ensure(mats, "Gravel",      {0.27f, 0.25f, 0.23f}, 0.00f, 0.95f);
    p.rail       = ensure(mats, "Rail",        {0.36f, 0.33f, 0.30f}, 0.60f, 0.35f);
    p.trainRed   = ensure(mats, "Train Red",   {0.58f, 0.05f, 0.05f}, 0.20f, 0.35f);
    p.steel      = ensure(mats, "Pylon Steel", {0.45f, 0.47f, 0.47f}, 0.40f, 0.50f);
    p.neon       = ensure(mats, "Neon",        {0.90f, 0.90f, 0.88f}, 0.00f, 0.40f,
                          {1.00f, 0.95f, 0.85f}, 3.0f);
    // The pavement's slabs: a baked texture (sandbox/tools/pavingtex.py), found
    // by the GUID its committed sidecar carries; CitySystem loads the pixels.
    for (MaterialDef& m : mats)
        if (m.assetId == p.pavement) {
            m.texId       = AssetId::fromString("5f2a9c1e7b3d4e60a1c8f09d2b6e7a31");
            m.normalTexId = AssetId::fromString("8d41e6b2c9a74f15b3e02c7d9a6f1e84");
            m.albedo      = glm::vec3(1.0f);
            m.tint        = glm::vec3(0.72f);   // concrete, not chalk, seen from above
        }
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
        case Kind::TownHall:    return townHall(width, depth, seed, pal);
        case Kind::School:      return school(width, depth, seed, pal);
        case Kind::Kindergarten: return kindergarten(width, depth, seed, pal);
        case Kind::Library:     return library(width, depth, seed, pal);
        case Kind::Museum:      return museum(width, depth, seed, pal);
        case Kind::Theatre:     return theatre(width, depth, seed, pal);
        case Kind::Pool:        return pool(width, depth, seed, pal);
        case Kind::PetrolStation: return petrolStation(width, depth, seed, pal);
        case Kind::PowerPlant:  return powerPlant(width, depth, seed, pal);
        case Kind::Landfill:    return landfill(width, depth, seed, pal);
        case Kind::Station:     return station(width, depth, seed, pal);
        case Kind::Supermarket: return supermarket(width, depth, seed, pal);
        case Kind::Cinema:      return cinema(width, depth, seed, pal);
        case Kind::DepartmentStore: return departmentStore(width, depth, seed, pal);
        case Kind::ParkingGarage:   return parkingGarage(width, depth, seed, pal);
        case Kind::Hotel:       return hotel(width, depth, seed, pal);
        default:                return {};
    }
}

// --- Street furniture ----------------------------------------------------------

Model pylon(const Palette& P, std::vector<glm::vec3>* attach) {
    Builder b;
    const float H = 38.0f, waist = 28.0f;
    // Four legs tapering from an 8 m base to the waist, a slim peak above it.
    auto leg = [&](float y, int sx, int sz) {
        const float t = y / waist;
        const float half = 4.0f + (1.1f - 4.0f) * t;
        return glm::vec3(sx * half, y, sz * half);
    };
    const int sx[4] = {-1, 1, 1, -1}, sz[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        b.beam(leg(0.0f, sx[i], sz[i]), leg(waist, sx[i], sz[i]), 0.3f, P.steel);
        b.beam(leg(waist, sx[i], sz[i]), {sx[i] * 0.6f, H, sz[i] * 0.6f}, 0.2f, P.steel);
    }
    // Cross bracing on every face, in five bays.
    const float bays[6] = {0.0f, 7.0f, 13.0f, 18.5f, 23.5f, waist};
    for (int f = 0; f < 4; ++f) {
        const int g = (f + 1) % 4;
        for (int k = 0; k < 5; ++k) {
            b.beam(leg(bays[k], sx[f], sz[f]), leg(bays[k + 1], sx[g], sz[g]), 0.12f, P.steel);
            b.beam(leg(bays[k], sx[g], sz[g]), leg(bays[k + 1], sx[f], sz[f]), 0.12f, P.steel);
            b.beam(leg(bays[k + 1], sx[f], sz[f]), leg(bays[k + 1], sx[g], sz[g]), 0.12f, P.steel);
        }
    }
    // The arms: a wide one at 26 m, a narrower one at 32 m, and the insulator
    // strings hanging from their ends.
    const float armY[2] = {26.0f, 32.0f}, armX[2] = {11.0f, 8.0f};
    std::vector<glm::vec3> points;
    for (int a = 0; a < 2; ++a) {
        const float y = armY[a];
        for (int s = -1; s <= 1; s += 2) {
            b.beam({0.0f, y, -0.8f}, {s * armX[a], y, -0.3f}, 0.25f, P.steel);
            b.beam({0.0f, y, 0.8f}, {s * armX[a], y, 0.3f}, 0.25f, P.steel);
            b.beam({s * 1.2f, y + 3.0f, 0.0f}, {s * armX[a], y, 0.0f}, 0.15f, P.steel);
            const std::vector<float> xs = a == 0 ? std::vector<float>{6.0f, 10.5f} : std::vector<float>{7.5f};
            for (float x : xs) {
                b.beam({s * x, y, 0.0f}, {s * x, y - 3.0f, 0.0f}, 0.18f, P.white);
                points.push_back({s * x, y - 3.0f, 0.0f});
            }
        }
    }
    points.push_back({0.0f, H, 0.0f});   // the earth wire, over the top
    if (attach) *attach = points;
    b.solid(-4.0f, 4.0f, 0.0f, 12.0f, -4.0f, 4.0f);
    b.top = H;
    return b.finish();
}

void cable(fitzel::MeshData& md, glm::vec3 a, glm::vec3 b, float sag, float radius) {
    const int segs = 16;
    auto at = [&](float t) { return glm::mix(a, b, t) - glm::vec3(0.0f, 4.0f * sag * t * (1.0f - t), 0.0f); };
    const glm::vec3 along = glm::normalize(b - a);
    glm::vec3 side = glm::cross(along, glm::vec3(0, 1, 0));
    side = glm::length(side) > 1e-4f ? glm::normalize(side) : glm::vec3(1, 0, 0);
    const glm::vec3 up = glm::normalize(glm::cross(side, along));
    const glm::vec3 ring[3] = {up * radius, (-0.5f * up + 0.866f * side) * radius,
                               (-0.5f * up - 0.866f * side) * radius};
    for (int i = 0; i < segs; ++i) {
        const glm::vec3 p = at(static_cast<float>(i) / segs), q = at(static_cast<float>(i + 1) / segs);
        for (int k = 0; k < 3; ++k) {
            const glm::vec3 r0 = ring[k], r1 = ring[(k + 1) % 3];
            const glm::vec3 n = glm::normalize(r0 + r1);
            const auto base = static_cast<std::uint32_t>(md.vertices.size());
            for (const glm::vec3& v : {p + r0, q + r0, q + r1, p + r1}) {
                fitzel::Vertex vx{};
                vx.position = v;
                vx.normal   = n;
                md.vertices.push_back(vx);
            }
            // Wound so the outside is the front, whichever way the wire runs.
            const glm::vec3 fn = glm::cross((q + r0) - (p + r0), (p + r1) - (p + r0));
            if (glm::dot(fn, n) >= 0.0f)
                md.indices.insert(md.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            else
                md.indices.insert(md.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
        }
    }
}

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
