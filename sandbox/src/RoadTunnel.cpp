#include "RoadTunnel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#ifndef FITZEL_PLAYER
#include <imgui.h>
#endif

#include <fitzel/graphics/Mesh.hpp>

namespace roadtunnel {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// World metres per texture tile on the concrete. Matches RoadBridge's, so a bore
// and a deck on the same road read as the same material at the same scale.
constexpr float kTile = 4.0f;

// Segments across the arched crown. Ten is smooth at the speeds this engine is
// driven at and keeps a long bore's triangle count in the thousands, not the
// hundreds of thousands.
constexpr int kArcSegs = 10;

// The frame of the road at one centreline sample. Deliberately the same
// construction as RoadBridge's -- the two are built against the same lofted
// ribbon, and a bore whose section rolled differently from the deck's would sit
// crooked on the very same carriageway.
struct Frame {
    glm::vec3 pos;
    glm::vec3 side;  // across the bore, ROLLED by the road's cross-fall
    glm::vec3 fwd;
    glm::vec3 up;    // section's up, rolled with `side`
};

Frame frameAt(const std::vector<glm::vec2>& center, const std::vector<float>& prof,
              const std::vector<float>& bank, std::size_t i) {
    glm::vec2 f = (i == 0)                 ? center[1] - center[0]
                : (i + 1 == center.size()) ? center[i] - center[i - 1]
                                           : center[i + 1] - center[i - 1];
    if (glm::length(f) < 1e-4f) f = glm::vec2(0.0f, 1.0f);
    f = glm::normalize(f);
    const glm::vec2 sd(f.y, -f.x);
    const float br = glm::radians((i < bank.size()) ? bank[i] : 0.0f);
    const float cb = std::cos(br), sb = std::sin(br);
    Frame fr;
    fr.pos  = glm::vec3(center[i].x, prof[i], center[i].y);
    fr.side = glm::vec3(sd.x * cb, -sb, sd.y * cb);
    fr.up   = glm::vec3(sd.x * sb,  cb, sd.y * sb);
    fr.fwd  = glm::vec3(f.x, 0.0f, f.y);
    return fr;
}

// A point of the cross-section placed in the world: `oh.x` metres sideways of the
// centreline, `oh.y` metres above the road surface.
glm::vec3 place(const Frame& fr, glm::vec2 oh) {
    return fr.pos + fr.side * oh.x + fr.up * oh.y;
}

// Append a quad with its normal turned to face `toward`.
//
// A bore is a HOLE, and that inverts the rule every other sweep in this engine
// follows: all of it is seen from the inside, so its walls face the driver rather
// than the world. Rather than hand-wind eleven section edges and two rings and
// get one of them backwards -- a wall you look straight through into the hill --
// the winding is decided by where the surface should be seen FROM.
void facedQuad(fitzel::MeshData& md, glm::vec3 a, glm::vec3 b, glm::vec3 c,
               glm::vec3 d, glm::vec2 ua, glm::vec2 ub, glm::vec2 uc, glm::vec2 ud,
               const glm::vec3& toward) {
    glm::vec3 n = glm::cross(b - a, d - a);
    if (glm::length(n) < 1e-9f) return; // degenerate (zero-length step or edge)
    const glm::vec3 mid = 0.25f * (a + b + c + d);
    if (glm::dot(n, toward - mid) < 0.0f) { // wound away from the viewer: flip
        std::swap(b, d);
        std::swap(ub, ud);
        n = -n;
    }
    const glm::vec3 nn = glm::normalize(n);
    const auto base = static_cast<std::uint32_t>(md.vertices.size());
    md.vertices.push_back({a, nn, ua});
    md.vertices.push_back({b, nn, ub});
    md.vertices.push_back({c, nn, uc});
    md.vertices.push_back({d, nn, ud});
    md.indices.insert(md.indices.end(),
                      {base, base + 1, base + 2, base, base + 2, base + 3});
}

// The bore's cross-section as an OPEN polyline in (sideways, height above the
// road): up the left wall, over the arched crown, down the right wall. Open at
// the bottom on purpose -- the road ribbon is the floor, and a slab under it
// would only z-fight the asphalt.
//
// The crown is a true semicircle of radius `half` centred on the springline, and
// that is what makes the portal ring concentric for free: growing `half` and
// `crown` by the same offset leaves the arc's centre exactly where it was and
// only grows its radius, so the two outlines never cross.
std::vector<glm::vec2> boreOutline(float half, float crown, float skirt) {
    const float spring = std::max(crown - half, 0.3f);
    std::vector<glm::vec2> o;
    o.reserve(static_cast<std::size_t>(kArcSegs) + 3);
    o.push_back({-half, -skirt});
    o.push_back({-half, spring});
    for (int k = 1; k <= kArcSegs; ++k) {
        const float a = kPi * (1.0f - static_cast<float>(k) /
                                          static_cast<float>(kArcSegs));
        o.push_back({half * std::cos(a), spring + half * std::sin(a)});
    }
    o.push_back({half, -skirt});
    return o;
}

// The flat concrete ring around a mouth, filling the gap between the bore and the
// portal outline. `dir` is +1 at the far end of a span and -1 at the near one, and
// only decides which way the ring is seen from -- a portal is the one part of a
// tunnel you look at from OUTSIDE.
void addPortal(fitzel::MeshData& md, const Frame& fr,
               const std::vector<glm::vec2>& in, const std::vector<glm::vec2>& out,
               float dir, float crown) {
    // Somewhere out in front of the mouth, up at eye height rather than at the
    // road: the ring must face the approaching driver.
    const glm::vec3 look = fr.pos + fr.fwd * (dir * 8.0f) + fr.up * (crown * 0.5f);
    for (std::size_t k = 0; k + 1 < in.size() && k + 1 < out.size(); ++k)
        facedQuad(md, place(fr, in[k]), place(fr, in[k + 1]),
                  place(fr, out[k + 1]), place(fr, out[k]),
                  in[k] / kTile, in[k + 1] / kTile,
                  out[k + 1] / kTile, out[k] / kTile, look);
}

// Prefix arc length along the centreline.
std::vector<float> arcLengths(const std::vector<glm::vec2>& center) {
    std::vector<float> s(center.size(), 0.0f);
    for (std::size_t i = 1; i < center.size(); ++i)
        s[i] = s[i - 1] + glm::distance(center[i], center[i - 1]);
    return s;
}

} // namespace

std::vector<Span> plan(const std::vector<glm::vec2>& center, std::vector<float>& prof,
                       const std::vector<Span>& cores, const Params& p,
                       std::vector<float>& gradeW) {
    const int n = static_cast<int>(center.size());
    gradeW.assign(static_cast<std::size_t>(std::max(n, 0)), 1.0f);
    std::vector<Span> spans;
    if (cores.empty() || n < 3) return spans;

    const std::vector<float> s = arcLengths(center);

    // Bore: hold the road DOWN onto the straight chord between the two points the
    // user picked. min() rather than a plain assignment, and that single character
    // is the whole difference from a bridge: ground rising between the ends is
    // then something the road goes THROUGH instead of over, while a dip that was
    // already below the chord keeps its own profile rather than being filled up to
    // it. A taut string pinned at both ends, pulled from the other side.
    for (const Span& c : cores) {
        const float len = s[c.end] - s[c.begin];
        if (len < 1e-5f) continue;
        const float h0 = prof[c.begin], h1 = prof[c.end];
        for (int i = c.begin + 1; i < c.end; ++i)
            prof[i] = std::min(prof[i], h0 + (h1 - h0) * (s[i] - s[c.begin]) / len);
    }

    std::vector<float> dist(n, s.back() + 1.0f); // "nowhere near a bore"
    for (const Span& c : cores)
        for (int i = c.begin; i <= c.end; ++i) dist[i] = 0.0f;

    // Distance along the road to the nearest bore, by the usual two-pass 1-D
    // transform. A closed loop wraps, so run it twice with the seam stitched
    // between passes and a distance can travel the whole way around.
    const bool loop = glm::distance(center.front(), center.back()) < 1e-3f;
    for (int pass = 0; pass < (loop ? 2 : 1); ++pass) {
        for (int i = 1; i < n; ++i)
            dist[i] = std::min(dist[i], dist[i - 1] + (s[i] - s[i - 1]));
        for (int i = n - 2; i >= 0; --i)
            dist[i] = std::min(dist[i], dist[i + 1] + (s[i + 1] - s[i]));
        if (loop) dist.front() = dist.back() = std::min(dist.front(), dist.back());
    }

    const float ab = std::max(p.abutment, 0.01f);
    for (int i = 0; i < n; ++i) {
        const float e = glm::clamp(dist[i] / ab, 0.0f, 1.0f);
        gradeW[i] = e * e * (3.0f - 2.0f * e); // 0 over the bore -> 1 on grade
    }

    // The tube reaches as far as the falloff does -- the same rule as a deck, and
    // for the mirrored reason. Over the abutment the ground ramps from hill height
    // back down to the road, so the tube there is progressively less buried and
    // ends standing clear: that exposed last stretch is the portal structure, and
    // it is what a mountain road's tunnel mouth actually looks like.
    for (int i = 0; i < n;) {
        if (dist[i] > ab) { ++i; continue; }
        int j = i;
        while (j + 1 < n && dist[j + 1] <= ab) ++j;
        if (j > i) spans.push_back({i, j});
        i = j + 1;
    }
    return spans;
}

void build(const std::vector<glm::vec2>& center, const std::vector<float>& prof,
           const std::vector<float>& bank, const std::vector<Span>& spans,
           float roadWidth, const Params& p, fitzel::MeshData& md) {
    if (spans.empty() || center.size() < 2) return;

    const std::vector<float> s = arcLengths(center);

    const float half  = roadWidth * 0.5f + std::max(p.sideClear, 0.0f);
    // Keep the springline off the road however the sliders are set: a clear height
    // below the bore's half width would put the arc's centre underground and turn
    // the section inside out.
    const float crown = std::max(p.clearHeight, half + 0.3f);
    const float skirt = std::max(p.skirt, 0.05f);
    const float por   = std::max(p.portal, 0.0f);

    const std::vector<glm::vec2> in  = boreOutline(half, crown, skirt);
    const std::vector<glm::vec2> out = boreOutline(half + por, crown + por, skirt);

    // Distance around the section, so the concrete's u runs continuously up one
    // wall, over the crown and down the other instead of restarting per face.
    std::vector<float> ou(in.size(), 0.0f);
    for (std::size_t k = 1; k < in.size(); ++k)
        ou[k] = ou[k - 1] + glm::distance(in[k], in[k - 1]);

    for (const Span& sp : spans) {
        for (int i = sp.begin; i < sp.end; ++i) {
            const Frame f0 = frameAt(center, prof, bank, static_cast<std::size_t>(i));
            const Frame f1 = frameAt(center, prof, bank, static_cast<std::size_t>(i + 1));
            // The tunnel's axis at this step, which is where every wall faces.
            const glm::vec3 axis = f0.pos + f0.up * (crown * 0.5f);
            const float v0 = s[i] / kTile, v1 = s[i + 1] / kTile;
            for (std::size_t k = 0; k + 1 < in.size(); ++k)
                facedQuad(md, place(f0, in[k]), place(f0, in[k + 1]),
                          place(f1, in[k + 1]), place(f1, in[k]),
                          {ou[k], v0}, {ou[k + 1], v0}, {ou[k + 1], v1}, {ou[k], v1},
                          axis);
        }
        if (por > 0.01f) {
            addPortal(md, frameAt(center, prof, bank,
                                  static_cast<std::size_t>(sp.begin)),
                      in, out, -1.0f, crown);
            addPortal(md, frameAt(center, prof, bank,
                                  static_cast<std::size_t>(sp.end)),
                      in, out, 1.0f, crown);
        }
    }
}

#ifndef FITZEL_PLAYER
bool panel(Params& p) {
    bool rc = ImGui::SliderFloat("Bore abutment", &p.abutment, 1.0f, 40.0f, "%.1f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Length of the cutting the terrain ramps down over to\n"
                          "reach each mouth. This is what leaves a face for the\n"
                          "portal to stand in -- too short and the mouth is a\n"
                          "cliff, too long and the road is in a canyon.");
    rc |= ImGui::SliderFloat("Clear height", &p.clearHeight, 3.0f, 14.0f, "%.2f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Headroom above the road at the crown of the arch.");
    rc |= ImGui::SliderFloat("Side clearance", &p.sideClear, 0.0f, 6.0f, "%.2f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far the walls stand beyond the road edge.");
    rc |= ImGui::SliderFloat("Wall skirt", &p.skirt, 0.0f, 6.0f, "%.2f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far the walls carry on below the road, so no gap\n"
                          "opens under them where the ground falls away.");
    rc |= ImGui::SliderFloat("Portal ring", &p.portal, 0.0f, 4.0f, "%.2f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Width of the concrete ring around each mouth (0 = a\n"
                          "bare bore). It is what dresses the cut face.");
    return rc;
}
#endif

} // namespace roadtunnel
