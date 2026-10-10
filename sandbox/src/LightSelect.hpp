#pragma once
// Which point lights a frame draws when the scene has more than the renderer
// takes. Taking the first ones in entity order meant a level with forty lamps
// lit only the dozen that happened to be created first -- in the editor the
// rest simply did nothing, and only a game script switching lamps by hand made
// them work in Play.
//
// The lights that matter are the ones whose reach is near the eye. Each gets a
// score in metres -- how far the eye is from the edge of its sphere -- and the
// best `budget` are kept, nearest first. Where the camera LOOKS does not count:
// a penalty for spheres outside the view changed by metres per frame as the
// camera turned, so a lamp ran through its whole fade in a frame or two, and
// floodlights and their glow in the fog blinked in and out with every turn of
// the head. A lamp behind the eye still lights the fog, the reflections and
// the shadows in front of it. A hard cut would make a lamp
// pop as the camera crosses the line, so every kept light fades out as its
// score approaches the score of the first light left out. That score is an
// order statistic of continuous scores, so it moves continuously too: lights
// swap places at the cut at zero brightness, never visibly.
//
// Shadows are a second, smaller budget: the renderer shadows only the first
// kMaxShadowedPoints lights that ask for it. Handing those slots out in list
// order made a lamp drop its shadow at full brightness the moment a nearer
// shadowed lamp overtook it -- a wall jumped from shadowed to lit through the
// geometry as the camera walked. So the shadowed lights are ranked the same
// way, and a kept one's shadow strength fades out as its score approaches that
// of the first shadowed light left without a slot: the shadow is handed over
// at strength zero, where shadowed and unshadowed look the same.

#include <fitzel/render/Renderer.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <vector>

namespace lightselect {

inline void choose(std::vector<fitzel::PointLight>& lights, const glm::vec3& eye, int budget,
                   int shadowBudget = fitzel::Renderer::kMaxShadowedPoints,
                   float band = 3.0f) {
    int shadowed = 0;
    for (const fitzel::PointLight& l : lights)
        if (l.castShadows) ++shadowed;
    // All fit, shadows included: untouched.
    if (static_cast<int>(lights.size()) <= budget && shadowed <= shadowBudget) return;

    struct Ranked { float score; int index; };
    std::vector<Ranked> ranked;
    ranked.reserve(lights.size());
    for (int i = 0; i < static_cast<int>(lights.size()); ++i) {
        const fitzel::PointLight& l = lights[i];
        // A switched-off effect light (intensity 0) gives nothing to anyone.
        if (l.color.r <= 0.0f && l.color.g <= 0.0f && l.color.b <= 0.0f) continue;
        const float score = glm::length(l.position - eye) - l.range;
        ranked.push_back({score, i});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) { return a.score < b.score; });

    const auto smooth = [band](float gap) {
        const float t = glm::clamp(gap / band, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const bool over = static_cast<int>(ranked.size()) > budget;
    const float cut = over ? ranked[static_cast<std::size_t>(budget)].score : 0.0f;
    // The score of the first shadowed light that finds no shadow slot.
    bool shadowOver = false;
    float shadowCut = 0.0f;
    for (int k = 0, n = 0; k < static_cast<int>(ranked.size()); ++k) {
        if (!lights[ranked[k].index].castShadows) continue;
        if (n++ == shadowBudget) { shadowOver = true; shadowCut = ranked[k].score; break; }
    }

    std::vector<fitzel::PointLight> kept;
    kept.reserve(static_cast<std::size_t>(budget));
    int shadowsGiven = 0;
    for (int k = 0; k < std::min(budget, static_cast<int>(ranked.size())); ++k) {
        fitzel::PointLight l = lights[ranked[k].index];
        if (over) {
            const float fade = smooth(cut - ranked[k].score);
            l.color *= fade;
            l.shadowStrength *= fade;
        }
        if (l.castShadows) {
            if (shadowsGiven >= shadowBudget) {
                l.castShadows = false;     // its strength reached zero at shadowCut
            } else {
                ++shadowsGiven;
                if (shadowOver) l.shadowStrength *= smooth(shadowCut - ranked[k].score);
            }
        }
        kept.push_back(l);
    }
    lights.swap(kept);
}

} // namespace lightselect
