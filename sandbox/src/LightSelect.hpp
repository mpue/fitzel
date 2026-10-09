#pragma once
// Which point lights a frame draws when the scene has more than the renderer
// takes. Taking the first ones in entity order meant a level with forty lamps
// lit only the dozen that happened to be created first -- in the editor the
// rest simply did nothing, and only a game script switching lamps by hand made
// them work in Play.
//
// The lights that matter are the ones whose reach is near the eye and inside
// the view. Each gets a score in metres -- how far the eye is from the edge of
// its sphere, plus a penalty for how far that sphere lies outside the view --
// and the best `budget` are kept, nearest first. A hard cut would make a lamp
// pop as the camera crosses the line, so every kept light fades out as its
// score approaches the score of the first light left out. That score is an
// order statistic of continuous scores, so it moves continuously too: lights
// swap places at the cut at zero brightness, never visibly.

#include <fitzel/render/Renderer.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <vector>

namespace lightselect {

inline void choose(std::vector<fitzel::PointLight>& lights, const glm::vec3& eye,
                   const glm::mat4& viewProj, int budget, float band = 3.0f) {
    if (static_cast<int>(lights.size()) <= budget) return;   // all fit: untouched

    // Frustum planes (Gribb/Hartmann), normalised so plane distances are metres.
    glm::vec4 planes[6];
    const glm::mat4 m = glm::transpose(viewProj);
    planes[0] = m[3] + m[0]; planes[1] = m[3] - m[0];
    planes[2] = m[3] + m[1]; planes[3] = m[3] - m[1];
    planes[4] = m[3] + m[2]; planes[5] = m[3] - m[2];
    for (glm::vec4& p : planes) p /= std::max(1e-6f, glm::length(glm::vec3(p)));

    struct Ranked { float score; int index; };
    std::vector<Ranked> ranked;
    ranked.reserve(lights.size());
    for (int i = 0; i < static_cast<int>(lights.size()); ++i) {
        const fitzel::PointLight& l = lights[i];
        // A switched-off effect light (intensity 0) gives nothing to anyone.
        if (l.color.r <= 0.0f && l.color.g <= 0.0f && l.color.b <= 0.0f) continue;
        float outside = 0.0f;
        for (const glm::vec4& p : planes)
            outside = std::max(outside, -(glm::dot(glm::vec3(p), l.position) + p.w) - l.range);
        const float score = glm::length(l.position - eye) - l.range + 3.0f * outside;
        ranked.push_back({score, i});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) { return a.score < b.score; });

    std::vector<fitzel::PointLight> kept;
    kept.reserve(static_cast<std::size_t>(budget));
    const bool over = static_cast<int>(ranked.size()) > budget;
    const float cut = over ? ranked[static_cast<std::size_t>(budget)].score : 0.0f;
    for (int k = 0; k < std::min(budget, static_cast<int>(ranked.size())); ++k) {
        fitzel::PointLight l = lights[ranked[k].index];
        if (over) {
            const float t = glm::clamp((cut - ranked[k].score) / band, 0.0f, 1.0f);
            const float fade = t * t * (3.0f - 2.0f * t);
            l.color *= fade;
            l.shadowStrength *= fade;
        }
        kept.push_back(l);
    }
    lights.swap(kept);
}

} // namespace lightselect
