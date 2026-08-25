#include "RoadDecal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/gtc/constants.hpp>

namespace roaddecal {
namespace {

// How finely a patch is diced, in metres. A decal is a flat rectangle in the
// road's OWN (along, across) coordinates, but the road it lies on is neither
// flat nor straight, so the rectangle is walked in small steps and every step is
// put back on the surface.
//
// Each of those points lands exactly on the asphalt (surfaceAt interpolates
// between two rungs, which is the ribbon's own quad). What the step size decides
// is what happens BETWEEN them: a patch edge that skips over a rung cuts the
// corner the road turns there, and on the inside of a bend that shortcut passes
// under the surface. Three quarters of a metre is comfortably shorter than the
// road's own sample spacing (~2 m, see RoadSystem's kSampleStep), so a patch
// edge cannot span a rung and the lift has nothing to fight.
constexpr float kStep = 0.75f;

// The road's frame at one centreline sample: the point on the middle of the
// carriageway, the direction across it (centre -> right) and the surface normal.
// Built exactly as RoadSystem::loft builds a rung -- same central-difference
// tangent, same roll about the centreline -- so a patch cannot end up on a
// different surface than the ribbon it is lying on.
struct Frame {
    glm::vec3 c, side, up;
};

Frame frameAt(const std::vector<glm::vec2>& center, const std::vector<float>& y,
              const std::vector<float>& bank, std::size_t i) {
    const std::size_t n = center.size();
    glm::vec2 fwd = (i == 0)         ? center[1] - center[0]
                  : (i + 1 == n)     ? center[i] - center[i - 1]
                                     : center[i + 1] - center[i - 1];
    if (glm::length(fwd) < 1e-4f) fwd = glm::vec2(0.0f, 1.0f);
    fwd = glm::normalize(fwd);
    const glm::vec2 side(fwd.y, -fwd.x);

    const float br = glm::radians(i < bank.size() ? bank[i] : 0.0f);
    const float cb = std::cos(br), sb = std::sin(br);
    return {glm::vec3(center[i].x, i < y.size() ? y[i] : 0.0f, center[i].y),
            glm::vec3(side.x * cb, -sb, side.y * cb),
            glm::vec3(side.x * sb, cb, side.y * sb)};
}

} // namespace

Decal preset(Blend b) {
    Decal d;
    d.blend = b;
    switch (b) {
        case Blend::Cutout:  // a painted marking: arrow, number, grid square
            d.length = 6.0f; d.width = 4.0f; d.cutoff = 0.5f;
            break;
        case Blend::Blend:   // a stain: large, soft, and letting the tarmac through
            d.length = 8.0f; d.width = 6.0f; d.opacity = 0.6f;
            break;
        case Blend::Opaque:  // a surface swap: full width, sat flat on the road
            d.length = 10.0f; d.width = 8.0f; d.lift = 0.015f;
            break;
    }
    return d;
}

float centerlineLength(const std::vector<glm::vec2>& center) {
    float total = 0.0f;
    for (std::size_t i = 1; i < center.size(); ++i)
        total += glm::length(center[i] - center[i - 1]);
    return total;
}

fitzel::MeshData generate(const Decal& d, const std::vector<glm::vec2>& center,
                          const std::vector<float>& surfaceY,
                          const std::vector<float>& bankDeg) {
    fitzel::MeshData md;
    if (!d.enabled || d.texture.empty()) return md;
    if (center.size() < 2 || surfaceY.size() < center.size()) return md;
    if (d.length < 1e-3f || d.width < 1e-3f) return md;

    // Arc length at every centreline sample -- the road's own ruler, and what
    // `dist` is measured against.
    std::vector<float> arc(center.size(), 0.0f);
    for (std::size_t i = 1; i < center.size(); ++i)
        arc[i] = arc[i - 1] + glm::length(center[i] - center[i - 1]);
    const float total = arc.back();
    if (total < 1e-3f) return md;

    // A point on the carriageway `s` metres along and `o` metres right of the
    // middle, with the surface normal there. The two bracketing rungs are placed
    // first and the result interpolated BETWEEN THEM -- not from an interpolated
    // frame -- because that is what the ribbon's own quads do, so the patch lands
    // on the asphalt rather than on a smooth curve the asphalt only approximates.
    auto surfaceAt = [&](float s, float o, glm::vec3& p, glm::vec3& n) {
        s = std::clamp(s, 0.0f, total);
        const auto it = std::upper_bound(arc.begin(), arc.end(), s);
        std::size_t i1 = static_cast<std::size_t>(it - arc.begin());
        i1 = std::clamp<std::size_t>(i1, 1, center.size() - 1);
        const std::size_t i0 = i1 - 1;
        const float span = arc[i1] - arc[i0];
        const float t    = span > 1e-6f ? (s - arc[i0]) / span : 0.0f;

        const Frame f0 = frameAt(center, surfaceY, bankDeg, i0);
        const Frame f1 = frameAt(center, surfaceY, bankDeg, i1);
        p = glm::mix(f0.c + f0.side * o, f1.c + f1.side * o, t);
        n = glm::mix(f0.up, f1.up, t);
        n = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0.0f, 1.0f, 0.0f);
    };

    // The image's turn, applied to the PATCH rather than to its UVs. Rotating the
    // texture coordinates instead would push them outside 0..1 at any angle that
    // is not a half turn, and a repeating sampler answers that by tiling the
    // neighbouring copy of the image into the corners. Turning the rectangle
    // keeps every UV inside the image; the geometry is put back on the surface
    // point by point either way, so the patch stays glued to the road.
    const float sr = glm::radians(d.spin);
    const float cs = std::cos(sr), ss = std::sin(sr);

    const float halfL = d.length * 0.5f, halfW = d.width * 0.5f;
    // Dice both ways: with a turned patch, moving ACROSS the rectangle also moves
    // along the road, so the cross direction needs the same treatment as the long
    // one or a rotated decal cuts the corner between its edges.
    const int na = std::max(1, static_cast<int>(std::ceil(d.length / kStep)));
    const int nb = std::max(1, static_cast<int>(std::ceil(d.width  / kStep)));

    const int copies = std::clamp(d.repeat, 1, 512);
    const float step = d.spacing > 1e-3f ? d.spacing : d.length;

    for (int k = 0; k < copies; ++k) {
        const float dc = d.dist + static_cast<float>(k) * step;
        // Off the end of the road: skipped, not clamped. A run of repeats that
        // walks past the finish should stop there, not pile its remaining copies
        // up on the last metre.
        if (dc + halfL < 0.0f || dc - halfL > total) continue;

        const auto base = static_cast<std::uint32_t>(md.vertices.size());
        for (int ia = 0; ia <= na; ++ia) {
            // a runs along the patch, b across it, both in metres from its centre.
            const float fa = static_cast<float>(ia) / static_cast<float>(na);
            const float a  = -halfL + fa * d.length;
            for (int ib = 0; ib <= nb; ++ib) {
                const float fb = static_cast<float>(ib) / static_cast<float>(nb);
                const float b  = -halfW + fb * d.width;

                const float s = dc     + a * cs - b * ss;
                const float o = d.offset + a * ss + b * cs;

                glm::vec3 p, n;
                surfaceAt(s, o, p, n);
                // v = 0 at the far end, so the top of the image points the way the
                // road is going: an arrow drawn pointing up the picture points up
                // the track.
                md.vertices.push_back({p + n * d.lift, n, {fb, 1.0f - fa}});
            }
        }

        // Two triangles per cell, wound so the face points along the normal (the
        // same winding RoadSystem::loft uses for the ribbon, for the same reason:
        // a back-facing decal is invisible under back-face culling).
        const auto stride = static_cast<std::uint32_t>(nb + 1);
        for (int ia = 0; ia < na; ++ia) {
            for (int ib = 0; ib < nb; ++ib) {
                const std::uint32_t v0 = base + static_cast<std::uint32_t>(ia) * stride +
                                         static_cast<std::uint32_t>(ib);
                const std::uint32_t v1 = v0 + stride;
                md.indices.insert(md.indices.end(),
                                  {v0, v1, v0 + 1, v0 + 1, v1, v1 + 1});
            }
        }
    }
    return md;
}

} // namespace roaddecal
