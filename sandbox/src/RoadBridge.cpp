#include "RoadBridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#ifndef FITZEL_PLAYER
#include <imgui.h>
#endif

#include <fitzel/graphics/Mesh.hpp>

namespace roadbridge {
namespace {

// World metres per texture tile on the concrete. Not exposed: the deck reads as
// concrete at any road width, and one more slider earns less than it costs.
constexpr float kTile = 4.0f;

// How much clearance a pier needs before it is worth placing. Below this it is a
// stub poking out of the ground, which looks worse than no pier at all.
constexpr float kMinPierHeight = 1.0f;

// How far a pier is sunk into the ground, so it stays planted where the terrain
// under the deck is lower than the base heights the spans were measured against.
constexpr float kPierEmbed = 1.5f;

// The frame of the road at one centreline sample: where the deck surface centre
// is, and which way is sideways. `side` matches RoadSystem::loft's ribbon
// (cross((0,1,0), forward)), so the deck sits square under the road.
struct Frame {
    glm::vec3 pos;
    glm::vec3 side;
    glm::vec3 fwd;
};

Frame frameAt(const std::vector<glm::vec2>& center, const std::vector<float>& prof,
              std::size_t i) {
    glm::vec2 f = (i == 0)                 ? center[1] - center[0]
                : (i + 1 == center.size()) ? center[i] - center[i - 1]
                                           : center[i + 1] - center[i - 1];
    if (glm::length(f) < 1e-4f) f = glm::vec2(0.0f, 1.0f);
    f = glm::normalize(f);
    return {glm::vec3(center[i].x, prof[i], center[i].y),
            glm::vec3(f.y, 0.0f, -f.x), glm::vec3(f.x, 0.0f, f.y)};
}

// A point of the cross-section placed in the world: `o` metres sideways of the
// centreline, `h` metres above the road surface.
glm::vec3 place(const Frame& fr, glm::vec2 oh) {
    return fr.pos + fr.side * oh.x + glm::vec3(0.0f, oh.y, 0.0f);
}

// Append one flat-shaded quad. a,b,c,d run around the face counter-clockwise as
// seen from the front, which puts the normal on the outside.
void quad(fitzel::MeshData& md, const glm::vec3& a, const glm::vec3& b,
          const glm::vec3& c, const glm::vec3& d, glm::vec2 ua, glm::vec2 ub,
          glm::vec2 uc, glm::vec2 ud) {
    const glm::vec3 n = glm::cross(b - a, d - a);
    if (glm::length(n) < 1e-9f) return; // degenerate (zero-length span/edge)
    const glm::vec3 nn = glm::normalize(n);
    const auto base = static_cast<std::uint32_t>(md.vertices.size());
    md.vertices.push_back({a, nn, ua});
    md.vertices.push_back({b, nn, ub});
    md.vertices.push_back({c, nn, uc});
    md.vertices.push_back({d, nn, ud});
    md.indices.insert(md.indices.end(),
                      {base, base + 1, base + 2, base, base + 2, base + 3});
}

// The deck's cross-section, traced anticlockwise in (sideways, height) space with
// the road surface at height 0: across the underside, up the right parapet, back
// along the deck between the parapets, and down the left one. Sweeping it forward
// therefore turns every wall's normal outward.
std::vector<glm::vec2> deckOutline(float roadWidth, const Params& p) {
    const float ho = roadWidth * 0.5f + std::max(p.overhang, 0.0f);
    const float dt = std::max(p.deckThick, 0.05f);
    const float rh = std::max(p.railHeight, 0.0f);
    const float rw = glm::clamp(p.railWidth, 0.02f, ho * 0.4f);

    std::vector<glm::vec2> o{{-ho, -dt}, {ho, -dt}};
    if (rh > 0.01f) {
        o.push_back({ho, rh});
        o.push_back({ho - rw, rh});
        o.push_back({ho - rw, 0.0f});
        o.push_back({-ho + rw, 0.0f});
        o.push_back({-ho + rw, rh});
        o.push_back({-ho, rh});
    } else {
        o.push_back({ho, 0.0f});
        o.push_back({-ho, 0.0f});
    }
    return o;
}

// Cap a deck end by filling the cross-section with rectangles: the slab, plus a
// parapet either side. `front` faces the cap down the road (+forward), else back.
void capRect(fitzel::MeshData& md, const Frame& fr, glm::vec2 lo, glm::vec2 hi,
             bool front) {
    if (hi.x - lo.x < 1e-4f || hi.y - lo.y < 1e-4f) return;
    const glm::vec3 a = place(fr, {lo.x, lo.y}), b = place(fr, {hi.x, lo.y});
    const glm::vec3 c = place(fr, {hi.x, hi.y}), d = place(fr, {lo.x, hi.y});
    const glm::vec2 ua(lo.x / kTile, lo.y / kTile), ub(hi.x / kTile, lo.y / kTile);
    const glm::vec2 uc(hi.x / kTile, hi.y / kTile), ud(lo.x / kTile, hi.y / kTile);
    // Anticlockwise in (sideways, height) faces +forward; reversed faces back.
    if (front) quad(md, a, b, c, d, ua, ub, uc, ud);
    else       quad(md, d, c, b, a, ud, uc, ub, ua);
}

void addCap(fitzel::MeshData& md, const Frame& fr, float roadWidth,
            const Params& p, bool front) {
    const float ho = roadWidth * 0.5f + std::max(p.overhang, 0.0f);
    const float dt = std::max(p.deckThick, 0.05f);
    const float rh = std::max(p.railHeight, 0.0f);
    const float rw = glm::clamp(p.railWidth, 0.02f, ho * 0.4f);
    capRect(md, fr, {-ho, -dt}, {ho, 0.0f}, front); // slab
    if (rh > 0.01f) {
        capRect(md, fr, {-ho, 0.0f}, {-ho + rw, rh}, front);
        capRect(md, fr, {ho - rw, 0.0f}, {ho, rh}, front);
    }
}

// A square column from `top` down to `bottom`, squared to the road frame. Only
// the four walls: the top is hidden under the slab and the foot is buried.
void addPier(fitzel::MeshData& md, const Frame& fr, float top, float bottom,
             float widthM) {
    const float hw = widthM * 0.5f;
    const glm::vec3 u = fr.side * hw, f = fr.fwd * hw;
    const glm::vec3 flat(fr.pos.x, 0.0f, fr.pos.z);
    const glm::vec3 ring[4] = {flat - u - f, flat + u - f, flat + u + f,
                               flat - u + f};
    for (int k = 0; k < 4; ++k) {
        // Wound from the *next* corner back to this one, which is what puts the
        // normal on the outside of the column (see quad()).
        const glm::vec3& p0 = ring[(k + 1) % 4];
        const glm::vec3& p1 = ring[k];
        const float u0 = static_cast<float>(k + 1) * widthM / kTile;
        const float u1 = static_cast<float>(k) * widthM / kTile;
        quad(md, p0 + glm::vec3(0.0f, bottom, 0.0f),
                 p1 + glm::vec3(0.0f, bottom, 0.0f),
                 p1 + glm::vec3(0.0f, top, 0.0f),
                 p0 + glm::vec3(0.0f, top, 0.0f),
             {u0, bottom / kTile}, {u1, bottom / kTile}, {u1, top / kTile},
             {u0, top / kTile});
    }
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

    // Fly: lift the road onto the straight chord between the two points the user
    // picked. max() rather than a plain assignment, so ground rising between them
    // carries the deck over it instead of swallowing it -- a taut string pinned at
    // both ends, which is also what a bridge physically is.
    for (const Span& c : cores) {
        const float len = s[c.end] - s[c.begin];
        if (len < 1e-5f) continue;
        const float h0 = prof[c.begin], h1 = prof[c.end];
        for (int i = c.begin + 1; i < c.end; ++i)
            prof[i] = std::max(prof[i], h0 + (h1 - h0) * (s[i] - s[c.begin]) / len);
    }

    std::vector<float> dist(n, s.back() + 1.0f); // "nowhere near a deck"
    for (const Span& c : cores)
        for (int i = c.begin; i <= c.end; ++i) dist[i] = 0.0f;

    // Distance along the road from each sample to the nearest deck, by the usual
    // two-pass 1-D transform. A closed loop wraps, so run it twice with the seam
    // stitched between passes and a distance can travel the whole way around.
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
        gradeW[i] = e * e * (3.0f - 2.0f * e); // 0 under the deck -> 1 on grade
    }

    // The deck reaches as far as the falloff does. That merges bridges closer than
    // two abutments into one deck, and lands each end where the terrain has ramped
    // all the way up to the road -- so the end cap buries itself.
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
           const std::vector<float>& ground, const std::vector<Span>& spans,
           float roadWidth, const Params& p, fitzel::MeshData& md) {
    if (spans.empty() || center.size() < 2) return;

    const std::vector<float> s = arcLengths(center);
    const std::vector<glm::vec2> outline = deckOutline(roadWidth, p);
    const float dt = std::max(p.deckThick, 0.05f);

    // Distance around the cross-section, so the concrete's u runs continuously over
    // the underside, up the parapets and back without a seam per face. One entry
    // longer than the outline: the closing edge's far end is the full perimeter,
    // not a wrapped-around zero.
    std::vector<float> ou(outline.size() + 1, 0.0f);
    for (std::size_t k = 1; k <= outline.size(); ++k)
        ou[k] = ou[k - 1] +
                glm::distance(outline[k % outline.size()], outline[k - 1]);

    for (const Span& sp : spans) {
        // Sweep the cross-section along the span: one quad per outline edge per
        // step, walls out. The edges wrap, so the outline closes back onto itself
        // and the deck is a solid. The deck top between the parapets is part of the
        // outline, so the road ribbon (which floats 6 cm up) never leaves a gap.
        for (int i = sp.begin; i < sp.end; ++i) {
            const Frame f0 = frameAt(center, prof, static_cast<std::size_t>(i));
            const Frame f1 = frameAt(center, prof, static_cast<std::size_t>(i + 1));
            const float v0 = s[i] / kTile, v1 = s[i + 1] / kTile;
            for (std::size_t k = 0; k < outline.size(); ++k) {
                const glm::vec2 e0 = outline[k];
                const glm::vec2 e1 = outline[(k + 1) % outline.size()];
                quad(md, place(f0, e0), place(f0, e1), place(f1, e1), place(f1, e0),
                     {ou[k], v0}, {ou[k + 1], v0}, {ou[k + 1], v1}, {ou[k], v1});
            }
        }
        addCap(md, frameAt(center, prof, static_cast<std::size_t>(sp.begin)),
               roadWidth, p, /*front=*/false);
        addCap(md, frameAt(center, prof, static_cast<std::size_t>(sp.end)),
               roadWidth, p, /*front=*/true);

        if (p.pierSpacing < 0.5f || p.pierWidth < 0.05f) continue;

        // Piers at even spacing *between* the ends, never at them -- the abutments
        // already carry the deck where it meets the ground.
        const float len = s[sp.end] - s[sp.begin];
        const int bays = static_cast<int>(std::round(len / p.pierSpacing));
        for (int b = 1; b < bays; ++b) {
            const float at = s[sp.begin] + len * static_cast<float>(b) /
                                               static_cast<float>(bays);
            // Locate the sample the pier stands on and interpolate its frame.
            int i = sp.begin;
            while (i + 1 < sp.end && s[i + 1] < at) ++i;
            const float seg = s[i + 1] - s[i];
            const float t = (seg > 1e-5f) ? glm::clamp((at - s[i]) / seg, 0.0f, 1.0f)
                                          : 0.0f;
            Frame fr = frameAt(center, prof, static_cast<std::size_t>(i));
            const Frame nx = frameAt(center, prof, static_cast<std::size_t>(i + 1));
            fr.pos  = glm::mix(fr.pos, nx.pos, t);
            fr.side = glm::normalize(glm::mix(fr.side, nx.side, t));
            fr.fwd  = glm::normalize(glm::mix(fr.fwd, nx.fwd, t));

            const float gnd = glm::mix(ground[i], ground[i + 1], t);
            const float top = fr.pos.y - dt; // flush with the slab underside
            if (top - gnd < kMinPierHeight) continue;
            addPier(md, fr, top, gnd - kPierEmbed, p.pierWidth);
        }
    }
}

#ifndef FITZEL_PLAYER
bool panel(Params& p) {
    bool rc = ImGui::SliderFloat("Abutment", &p.abutment, 1.0f, 20.0f, "%.1f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Length of the ramp the terrain rises over to meet each\n"
                          "deck end (and where the deck ends bury themselves).");
    rc |= ImGui::SliderFloat("Deck thickness", &p.deckThick, 0.1f, 3.0f, "%.2f m");
    rc |= ImGui::SliderFloat("Deck overhang", &p.overhang, 0.0f, 3.0f, "%.2f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far the deck sticks out past the road surface.");
    rc |= ImGui::SliderFloat("Parapet height", &p.railHeight, 0.0f, 2.5f, "%.2f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Height of the walls along the deck (0 = none).");
    rc |= ImGui::SliderFloat("Parapet width", &p.railWidth, 0.05f, 1.0f, "%.2f m");
    rc |= ImGui::SliderFloat("Pier spacing", &p.pierSpacing, 0.0f, 60.0f, "%.1f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Distance between the columns holding the deck up\n"
                          "(0 = no piers, for a single clear span).");
    rc |= ImGui::SliderFloat("Pier width", &p.pierWidth, 0.2f, 5.0f, "%.2f m");
    return rc;
}
#endif

} // namespace roadbridge
