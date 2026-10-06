#include "StreetProps.hpp"

#include <algorithm>
#include <cmath>

#include "CivicBuilder.hpp"
#include "StreetSign.hpp"   // the lettering

namespace props {

namespace {

using fitzel::AssetId;
using civic::Builder;
using civic::Model;

constexpr float kPi = 3.14159265358979323846f;

std::uint32_t hashU(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

AssetId ensure(std::vector<MaterialDef>& mats, const char* part, glm::vec3 albedo, float refl,
               float rough, glm::vec3 emission = glm::vec3(0.0f), float emissionStrength = 1.0f) {
    const std::string name = std::string("City Prop ") + part;
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

// --- Flat shapes on a face ----------------------------------------------------------
// A sign is drawn in its own plane: x to the reader's right, y up. `Plane` puts
// that plane in the model -- a face looking down -z is read with +x to the
// reader's LEFT, so its `right` is -x.
struct Plane {
    glm::vec3 origin{0.0f}, right{-1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f}, normal{0.0f, 0.0f, -1.0f};
    // The same plane `d` metres further out along its normal.
    Plane out(float d) const { Plane p = *this; p.origin += normal * d; return p; }
};
Plane front(glm::vec3 at) { return {at, {-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}; }
Plane back(glm::vec3 at)  { return {at, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; }

void flat(Builder& b, const Plane& pl, const std::vector<glm::vec2>& pts, AssetId m) {
    std::vector<glm::vec3> q;
    q.reserve(pts.size());
    for (const glm::vec2& v : pts) q.push_back(pl.origin + pl.right * v.x + pl.up * v.y);
    b.face(std::move(q), pl.normal, m);
}

// A convex outline and the band `w` wide inside it, as quads -- the rim of a
// sign -- and what is left in the middle. Both in the same plane, so they never
// fight for a pixel. The inner outline is the outer one pulled in by `w`
// along each edge's normal (exact for the regular shapes signs are).
std::vector<glm::vec2> inset(const std::vector<glm::vec2>& o, float w) {
    const std::size_t n = o.size();
    float area = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec2 a = o[i], c = o[(i + 1) % n];
        area += a.x * c.y - a.y * c.x;
    }
    const float s = area >= 0.0f ? 1.0f : -1.0f;   // CCW: the inside is to the left
    std::vector<glm::vec2> in(n);
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec2 p = o[(i + n - 1) % n], c = o[i], q = o[(i + 1) % n];
        const glm::vec2 d0 = glm::normalize(c - p), d1 = glm::normalize(q - c);
        const glm::vec2 n0 = s * glm::vec2(-d0.y, d0.x), n1 = s * glm::vec2(-d1.y, d1.x);
        // Where the two edges, each moved in by w, meet.
        const glm::vec2 a = p + n0 * w, b = c + n1 * w;
        const float den = d0.x * d1.y - d0.y * d1.x;
        if (std::abs(den) < 1e-6f) { in[i] = c + n0 * w; continue; }
        const float t = ((b.x - a.x) * d1.y - (b.y - a.y) * d1.x) / den;
        in[i] = a + d0 * t;
    }
    return in;
}
void rimmed(Builder& b, const Plane& pl, const std::vector<glm::vec2>& outline, float w,
            AssetId rim, AssetId fill) {
    const std::vector<glm::vec2> in = inset(outline, w);
    const std::size_t n = outline.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        flat(b, pl, {outline[i], outline[j], in[j], in[i]}, rim);
    }
    flat(b, pl, in, fill);
}
std::vector<glm::vec2> regular(int n, float circumR, float startDeg, glm::vec2 c = glm::vec2(0.0f)) {
    std::vector<glm::vec2> p;
    for (int i = 0; i < n; ++i) {
        const float a = glm::radians(startDeg) + 2.0f * kPi * i / n;
        p.push_back(c + circumR * glm::vec2(std::cos(a), std::sin(a)));
    }
    return p;
}
std::vector<glm::vec2> rect(float x0, float y0, float x1, float y1) {
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

// Lettering in the street signs' face, `h` the capitals' height, centred on
// `c` in the plane. Fitted: narrower than `maxW` (it shrinks to fit).
streetsign::Face letters(const std::string& text, float h, float maxW, bool capitals = false) {
    streetsign::Style st;
    st.frame    = streetsign::Frame::None;
    st.capitals = capitals;
    streetsign::Face f = streetsign::layout(text, st, h);
    // layout() pads the plate by 0.8 h either side; the ink is what must fit.
    const float inkW = f.width - 1.6f * h;
    if (maxW > 0.0f && inkW > maxW) f = streetsign::layout(text, st, h * maxW / inkW);
    return f;
}
void ink(Builder& b, const Plane& pl, const streetsign::Face& f, glm::vec2 c, AssetId m) {
    for (const streetsign::Poly& p : f.ink) {
        std::vector<glm::vec2> q;
        q.reserve(p.pts.size());
        for (const glm::vec2& v : p.pts) q.push_back(v + c);
        flat(b, pl, q, m);
    }
}

// --- Posters --------------------------------------------------------------------
// The colour slots a poster or a shop board picks from.
AssetId colour(const Palette& P, int i) {
    switch (((i % 6) + 6) % 6) {
        case 0:  return P.red;
        case 1:  return P.white;
        case 2:  return P.blue;
        case 3:  return P.yellow;
        case 4:  return P.black;
        default: return P.green;
    }
}
struct Poster { int bg, ink; const char* head; const char* sub; };
// Invented, and the kind of thing a German town has on its columns.
const Poster kPosters[] = {
    {3, 4, "ZIRKUS", "Gastspiel am Festplatz"},
    {2, 1, "SOMMERFEST", "Samstag am Markt"},
    {0, 1, "FITZEL BR\xC3\x84U", "Das Helle seit 1887"},
    {1, 0, "FLOHMARKT", "Sonntag ab 8 Uhr"},
    {4, 3, "KONZERT", "Stadthalle 20 Uhr"},
    {5, 1, "WOCHENMARKT", "Frisch vom Land"},
    {0, 3, "ALLES MUSS RAUS", "Nur noch diese Woche"},
    {2, 3, "STADTLAUF", "10 km durch die Altstadt"},
    {1, 2, "THEATER", "Faust - Premiere"},
    {3, 0, "KIRMES", "Riesenrad und Autoscooter"},
};
constexpr int kPosterCount = static_cast<int>(sizeof(kPosters) / sizeof(kPosters[0]));

} // namespace

int posterCount() { return kPosterCount; }

Palette ensurePalette(std::vector<MaterialDef>& mats) {
    Palette p;
    p.red    = ensure(mats, "Sign Red",    {0.62f, 0.03f, 0.04f}, 0.05f, 0.45f);
    p.white  = ensure(mats, "Sign White",  {0.88f, 0.88f, 0.86f}, 0.05f, 0.45f);
    p.blue   = ensure(mats, "Sign Blue",   {0.02f, 0.15f, 0.45f}, 0.05f, 0.45f);
    p.yellow = ensure(mats, "Sign Yellow", {0.92f, 0.68f, 0.02f}, 0.05f, 0.45f);
    p.black  = ensure(mats, "Sign Black",  {0.03f, 0.03f, 0.035f}, 0.05f, 0.50f);
    p.green  = ensure(mats, "Sign Green",  {0.03f, 0.30f, 0.14f}, 0.05f, 0.45f);
    p.back   = ensure(mats, "Sign Back",   {0.50f, 0.51f, 0.52f}, 0.20f, 0.50f);
    p.pole   = ensure(mats, "Pole",        {0.58f, 0.60f, 0.61f}, 0.35f, 0.40f);
    p.iron   = ensure(mats, "Cast Iron",   {0.09f, 0.09f, 0.09f}, 0.15f, 0.55f);
    p.bin    = ensure(mats, "Bin",         {0.07f, 0.20f, 0.13f}, 0.05f, 0.55f);
    p.column = ensure(mats, "Column",      {0.08f, 0.19f, 0.14f}, 0.05f, 0.60f);
    p.glowWhite = ensure(mats, "Glow White", {0.90f, 0.90f, 0.88f}, 0.00f, 0.40f,
                         {1.00f, 0.97f, 0.90f}, 2.5f);
    p.glowWarm  = ensure(mats, "Glow Warm",  {0.95f, 0.72f, 0.22f}, 0.00f, 0.40f,
                         {1.00f, 0.70f, 0.25f}, 2.5f);
    return p;
}

const char* signName(Sign s) {
    switch (s) {
        case Sign::GiveWay:      return "Give way";
        case Sign::Stop:         return "Stop";
        case Sign::PriorityRoad: return "Priority road";
        case Sign::Crossing:     return "Pedestrian crossing";
        case Sign::Parking:      return "Parking";
        default:                 return "?";
    }
}

// --- Traffic signs ----------------------------------------------------------------

Model trafficSign(Sign s, const Palette& P) {
    Builder b;
    const float cy = 2.45f;              // the face's centre
    const float zf = -0.06f;             // its plane, in front of the pole's clamp
    b.cylinder(0.0f, 0.0f, 0.03f, 0.0f, cy + 0.2f, P.pole, 8, true, false);
    b.box(-0.04f, 0.04f, cy - 0.1f, cy + 0.1f, zf + 0.005f, -0.025f, P.pole);   // the clamp
    const Plane F = front({0.0f, cy, zf});
    const Plane B = back({0.0f, cy, zf + 0.008f});
    const Plane F2 = F.out(0.004f);      // what lies on the face: symbols, letters
    std::vector<glm::vec2> outline;
    switch (s) {
        case Sign::GiveWay: {
            // Point down; side 0.75 m, the centroid on the centre.
            const float side = 0.75f, h = side * 0.8660254f;
            outline = {{0.0f, -2.0f * h / 3.0f}, {0.5f * side, h / 3.0f}, {-0.5f * side, h / 3.0f}};
            rimmed(b, F, outline, 0.085f, P.red, P.white);
            break;
        }
        case Sign::Stop: {
            outline = regular(8, 0.375f / std::cos(kPi / 8.0f), 22.5f);
            rimmed(b, F, outline, 0.02f, P.white, P.red);
            ink(b, F2, letters("STOP", 0.15f, 0.56f, true), {0.0f, 0.0f}, P.white);
            break;
        }
        case Sign::PriorityRoad: {
            outline = regular(4, 0.42f, 90.0f);
            rimmed(b, F, outline, 0.07f, P.white, P.yellow);
            break;
        }
        case Sign::Crossing: {
            outline = rect(-0.3f, -0.3f, 0.3f, 0.3f);
            rimmed(b, F, outline, 0.018f, P.white, P.blue);
            flat(b, F2, {{-0.25f, -0.2f}, {0.25f, -0.2f}, {0.0f, 0.24f}}, P.white);
            // The figure on the zebra, as plain as the sign itself draws it.
            const Plane F3 = F2.out(0.003f);
            flat(b, F3, regular(8, 0.032f, 0.0f, {0.01f, 0.085f}), P.black);
            flat(b, F3, {{-0.02f, 0.045f}, {0.035f, 0.045f}, {0.03f, -0.06f}, {-0.025f, -0.06f}}, P.black);
            flat(b, F3, {{-0.025f, -0.06f}, {0.0f, -0.06f}, {-0.045f, -0.14f}, {-0.07f, -0.14f}}, P.black);
            flat(b, F3, {{0.005f, -0.06f}, {0.03f, -0.06f}, {0.075f, -0.14f}, {0.05f, -0.14f}}, P.black);
            for (int i = -2; i <= 2; ++i)
                flat(b, F3, rect(i * 0.05f - 0.016f, -0.175f, i * 0.05f + 0.016f, -0.155f), P.black);
            break;
        }
        case Sign::Parking:
        default: {
            outline = rect(-0.3f, -0.3f, 0.3f, 0.3f);
            rimmed(b, F, outline, 0.018f, P.white, P.blue);
            ink(b, F2, letters("P", 0.40f, 0.5f, true), {0.0f, 0.0f}, P.white);
            break;
        }
    }
    flat(b, B, outline, P.back);
    b.solid(-0.05f, 0.05f, 0.0f, 2.2f, -0.05f, 0.05f);
    return b.finish();
}

Model townSign(const std::string& name, const Palette& P) {
    Builder b;
    // About the 1250 x 800 of a real one, wider for a long name.
    const streetsign::Face f = letters(name, 0.2f, 3.0f);
    const float W  = std::max(f.width + 0.25f, 1.3f), H = 0.8f;
    const float cy = 1.55f + 0.5f * H;
    const float zf = -0.07f;
    for (float x : {-0.5f * W + 0.18f, 0.5f * W - 0.18f}) {
        b.cylinder(x, 0.0f, 0.035f, 0.0f, cy + 0.5f * H - 0.05f, P.pole, 8, true, false);
        b.solid(x - 0.05f, x + 0.05f, 0.0f, cy, -0.05f, 0.05f);
    }
    const std::vector<glm::vec2> outline = rect(-0.5f * W, -0.5f * H, 0.5f * W, 0.5f * H);
    const glm::vec2 lineC(0.0f, 0.04f);
    // Front: the place you come into.
    {
        const Plane F = front({0.0f, cy, zf});
        flat(b, F, outline, P.yellow);
        const Plane F2 = F.out(0.004f);
        // The black rule, a few centimetres in from the edge.
        const std::vector<glm::vec2> r0 = inset(outline, 0.03f), r1 = inset(outline, 0.05f);
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t j = (i + 1) % 4;
            flat(b, F2, {r0[i], r0[j], r1[j], r1[i]}, P.black);
        }
        ink(b, F2, f, lineC, P.black);
    }
    // Back: the end of the town -- the same name, struck through in red.
    {
        const Plane B = back({0.0f, cy, zf + 0.01f});
        flat(b, B, outline, P.yellow);
        const Plane B2 = B.out(0.004f);
        ink(b, B2, f, lineC, P.black);
        const float hw = 0.5f * W - 0.08f, hh = 0.5f * H - 0.08f;
        const glm::vec2 d = glm::normalize(glm::vec2(2.0f * hw, 2.0f * hh));
        const glm::vec2 n(-d.y * 0.045f, d.x * 0.045f);
        flat(b, B2.out(0.003f), {glm::vec2(-hw, -hh) - n, glm::vec2(hw, hh) - n,
                                 glm::vec2(hw, hh) + n, glm::vec2(-hw, -hh) + n}, P.red);
    }
    return b.finish();
}

// --- On the pavement --------------------------------------------------------------

Model bin(const Palette& P) {
    Builder b;
    b.cylinder(0.0f, 0.0f, 0.035f, 0.0f, 0.95f, P.pole, 6, true, false);
    const float zc = -0.27f, r = 0.22f;
    b.cylinder(0.0f, zc, r, 0.36f, 0.92f, P.bin, 10, false, false);
    b.cylinder(0.0f, zc, r + 0.015f, 0.88f, 0.94f, P.bin, 10, false, false);    // the rim
    b.cylinder(0.0f, zc, r - 0.01f, 0.86f, 0.87f, P.black, 10, true, false);    // what is in it
    {
        std::vector<glm::vec3> bottom;   // seen from the kerb, low down
        for (int i = 0; i < 10; ++i) {
            const float a = 2.0f * kPi * i / 10;
            bottom.push_back({r * std::cos(a), 0.36f, zc + r * std::sin(a)});
        }
        b.face(std::move(bottom), {0.0f, -1.0f, 0.0f}, P.bin);
    }
    b.box(-0.05f, 0.05f, 0.55f, 0.75f, zc + r - 0.02f, -0.03f, P.pole);         // the bracket
    b.solid(-0.2f, 0.2f, 0.0f, 0.9f, zc - 0.2f, 0.05f);
    return b.finish();
}

Model advertColumn(std::uint32_t seed, const Palette& P) {
    Builder b;
    const float r = 0.6f, y0 = 0.25f, y1 = 2.6f;
    b.cylinder(0.0f, 0.0f, r + 0.07f, 0.0f, y0, P.column, 16, true, false);
    // Four posters round the drum, a quarter each.
    const int seg = 16;
    int design[4];
    for (int q = 0; q < 4; ++q)
        design[q] = static_cast<int>(hashU(seed ^ (0x9e37U * (static_cast<std::uint32_t>(q) + 1U))) %
                                     static_cast<std::uint32_t>(kPosterCount));
    for (int i = 0; i < seg; ++i) {
        const float a0 = 2.0f * kPi * i / seg, a1 = 2.0f * kPi * (i + 1) / seg;
        const glm::vec3 p(r * std::cos(a0), 0.0f, r * std::sin(a0));
        const glm::vec3 q(r * std::cos(a1), 0.0f, r * std::sin(a1));
        const glm::vec3 mid = 0.5f * (p + q);
        const Poster& ps = kPosters[design[i * 4 / seg]];
        b.face({{p.x, y0, p.z}, {q.x, y0, q.z}, {q.x, y1, q.z}, {p.x, y1, p.z}}, mid,
               colour(P, ps.bg));
    }
    // The lettering, bent round the drum: a letter's corner at x along the
    // reader's right goes x/r radians round (right is the way the angle falls).
    for (int q = 0; q < 4; ++q) {
        const Poster& ps = kPosters[design[q]];
        const float ac = 2.0f * kPi * (q + 0.5f) / 4.0f;
        const float rr = r + 0.004f;
        auto wrap = [&](const streetsign::Face& f, float yc) {
            for (const streetsign::Poly& poly : f.ink) {
                std::vector<glm::vec3> pts;
                glm::vec2 c(0.0f);
                for (const glm::vec2& v : poly.pts) {
                    const float a = ac - v.x / rr;
                    pts.push_back({rr * std::cos(a), yc + v.y, rr * std::sin(a)});
                    c += v;
                }
                c /= static_cast<float>(poly.pts.size());
                const float a = ac - c.x / rr;
                b.face(std::move(pts), {std::cos(a), 0.0f, std::sin(a)}, colour(P, ps.ink));
            }
        };
        // The headline only: the small print is not read from the kerb, and it
        // would cost the column twice its lettering.
        wrap(letters(ps.head, 0.16f, 0.72f, true), 1.85f);
    }
    // The ring and the dome on top.
    b.cylinder(0.0f, 0.0f, r + 0.06f, y1, y1 + 0.16f, P.column, 16, true, false);
    b.dome(0.0f, 0.0f, r + 0.02f, y1 + 0.16f, P.column, 16, 4);
    b.cylinder(0.0f, 0.0f, 0.04f, y1 + 0.16f + r, y1 + 0.36f + r, P.column, 6, true, false);
    b.solid(-0.45f, 0.45f, 0.0f, y1, -0.45f, 0.45f);
    return b.finish();
}

Model billboard(std::uint32_t seed, const Palette& P) {
    Builder b;
    const Poster& ps = kPosters[hashU(seed ^ 0xb111U) % static_cast<std::uint32_t>(kPosterCount)];
    const float y0 = 1.8f, W = 3.8f, H = 2.8f;
    for (float x : {-1.2f, 1.2f}) {
        b.box(x - 0.07f, x + 0.07f, 0.0f, y0 + 0.4f, 0.03f, 0.17f, P.back, false);
        b.solid(x - 0.07f, x + 0.07f, 0.0f, y0, 0.03f, 0.17f);
    }
    b.box(-0.5f * W, 0.5f * W, y0, y0 + H, -0.06f, 0.03f, P.back);                // the frame
    const Plane F = front({0.0f, y0 + 0.5f * H, -0.062f});
    flat(b, F, rect(-0.5f * W + 0.08f, -0.5f * H + 0.08f, 0.5f * W - 0.08f, 0.5f * H - 0.08f),
         colour(P, ps.bg));
    const Plane F2 = F.out(0.004f);
    ink(b, F2, letters(ps.head, 0.42f, W - 0.6f, true), {0.0f, 0.3f}, colour(P, ps.ink));
    ink(b, F2, letters(ps.sub, 0.17f, W - 0.8f), {0.0f, -0.55f}, colour(P, ps.ink));
    return b.finish();
}

// --- In the street ----------------------------------------------------------------

Model manhole(const Palette& P, float rise) {
    Builder b;
    b.cylinder(0.0f, 0.0f, 0.325f, -0.03f, rise, P.iron, 10, true, false);
    return b.finish();
}

Model gully(const Palette& P, float rise) {
    Builder b;
    b.box(-0.25f, 0.25f, -0.03f, rise, -0.16f, 0.16f, P.iron);
    const float y = rise + 0.002f;
    for (int i = -3; i <= 3; ++i) {   // the slots, flat on the grate
        const float x0 = i * 0.06f - 0.012f, x1 = i * 0.06f + 0.012f;
        b.face({{x0, y, -0.12f}, {x1, y, -0.12f}, {x1, y, 0.12f}, {x0, y, 0.12f}}, {0, 1, 0}, P.black);
    }
    return b.finish();
}

Model zebra(float width, const Palette& P, float rise) {
    Builder b;
    const int n = std::max(1, static_cast<int>(std::floor((width - 0.4f) / 1.0f)));
    const float x0 = -0.5f * (n * 1.0f - 0.5f);
    for (int i = 0; i < n; ++i) {
        const float x = x0 + i * 1.0f;
        b.box(x, x + 0.5f, -0.03f, rise, -1.5f, 1.5f, P.white);
    }
    return b.finish();
}

// --- On the buildings ---------------------------------------------------------------

Model houseNumber(const std::string& number, const Palette& P) {
    Builder b;
    streetsign::Style st;
    st.frame = streetsign::Frame::Line;
    const streetsign::Face f = streetsign::layout(number, st, 0.085f);
    const Plane F = front({0.0f, 0.0f, -0.012f});
    flat(b, F, f.outline, P.blue);
    // The plate's edge, so it does not look painted on from the side.
    for (std::size_t i = 0; i < f.outline.size(); ++i) {
        const glm::vec2 a = f.outline[i], c = f.outline[(i + 1) % f.outline.size()];
        const glm::vec2 m = 0.5f * (a + c);
        b.face({{-a.x, a.y, -0.012f}, {-c.x, c.y, -0.012f}, {-c.x, c.y, 0.0f}, {-a.x, a.y, 0.0f}},
               {-m.x, m.y, 0.0f}, P.blue);
    }
    ink(b, F.out(0.003f), f, {0.0f, 0.0f}, P.white);
    return b.finish();
}

Model shopSign(const std::string& name, int colourIndex, float maxWidth, const Palette& P) {
    Builder b;
    static const int kBoards[4] = {0, 2, 5, 4};   // red, blue, green, black
    const int board = kBoards[((colourIndex % 4) + 4) % 4];
    const AssetId lettering = board == 4 ? P.glowWarm : P.glowWhite;
    const streetsign::Face f = letters(name, 0.34f, std::max(maxWidth - 0.5f, 0.6f));
    const float W = std::min(f.width + 0.1f, maxWidth), H = std::max(f.height * 0.9f, 0.5f);
    b.box(-0.5f * W, 0.5f * W, -0.5f * H, 0.5f * H, -0.14f, 0.0f, colour(P, board));
    ink(b, front({0.0f, 0.0f, -0.143f}), f, {0.0f, 0.0f}, lettering);
    return b.finish();
}

std::string shopName(std::uint32_t seed) {
    // What a German high street sells, and the names over the windows.
    static const char* const kTrade[] = {
        "B\xC3\xA4" "ckerei", "Apotheke", "Caf\xC3\xA9", "Friseur", "Blumen", "Metzgerei",
        "Buchhandlung", "Kiosk", "Optik", "Eiscaf\xC3\xA9", "D\xC3\xB6" "ner", "Pizzeria",
        "Reiseb\xC3\xBC" "ro", "Schuhhaus", "Stadtbank", "Weinhandlung", "Spielwaren",
        "Drogerie", "Bistro", "Uhren", "Fahrr\xC3\xA4" "der", "Getr\xC3\xA4" "nke",
    };
    static const char* const kOwner[] = {
        "M\xC3\xBC" "ller", "Schmidt", "Schneider", "Fischer", "Weber", "Meyer", "Wagner",
        "Becker", "Schulz", "Hoffmann", "Koch", "Richter", "Klein", "Wolf", "Neumann",
        "Schwarz", "Zimmermann", "Braun", "Kr\xC3\xBC" "ger", "Hartmann", "Lange", "Werner",
    };
    constexpr std::uint32_t nT = sizeof(kTrade) / sizeof(kTrade[0]);
    constexpr std::uint32_t nO = sizeof(kOwner) / sizeof(kOwner[0]);
    const std::uint32_t t = hashU(seed) % nT;
    std::string s = kTrade[t];
    // Some are only what they sell; most carry the family's name.
    if (hashU(seed ^ 0x51U) % 2U != 0U && t != 1U && t != 7U)
        s += std::string(" ") + kOwner[hashU(seed ^ 0x77U) % nO];
    return s;
}

} // namespace props
