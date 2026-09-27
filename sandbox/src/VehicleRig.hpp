#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "SceneTypes.hpp"

// Which children of a vehicle are its wheels -- shared by the Vehicle panel's
// "Make drivable" (VehicleTool), prefabs whose wheel ids went stale, and the
// towns' traffic, which spins the wheels of the prefabs it drives. Header-only
// because the traffic is part of the exported player, which has no editor UI
// to link the tool from.
namespace vehiclerig {

// True when an entity name reads like a wheel. Substring match for the long
// words; "rad" (German) only as a whole token so "gradient"/"radio" don't bite.
// "steer"/"lenk" are excluded so a steering wheel never lands in a wheel slot.
inline bool nameLooksLikeWheel(const std::string& name) {
    std::string n;
    n.reserve(name.size());
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.find("steer") != std::string::npos || n.find("lenk") != std::string::npos)
        return false;
    for (const char* w : {"wheel", "tire", "tyre", "reifen", "felge"})
        if (n.find(w) != std::string::npos) return true;
    for (std::size_t at = n.find("rad"); at != std::string::npos; at = n.find("rad", at + 1)) {
        const bool leftOk  = at == 0 || !std::isalpha(static_cast<unsigned char>(n[at - 1]));
        const bool rightOk = at + 3 >= n.size() ||
                             !std::isalpha(static_cast<unsigned char>(n[at + 3]));
        if (leftOk && rightOk) return true;
    }
    return false;
}

// The four wheels among `rootId`'s direct children, into FL FR RL RR (-1 where
// none was found): by name first, else by disc-shaped boxes (round in YZ -- the
// axle runs along X on a car), assigned by quadrant in the chassis frame (Z
// flipped for a nose on -Z, forward == 1); the outermost candidate wins each
// corner, so mirrors and hubcaps near the centre lose out.
inline void guessWheels(const std::vector<Entity>& entities, int rootId, int forward, int out[4]) {
    struct Cand { const Entity* e; bool byName; };
    std::vector<Cand> cands;
    int named = 0;
    for (const Entity& e : entities) {
        if (e.parent != rootId) continue;
        const bool byName = nameLooksLikeWheel(e.name);
        const float r = std::max(e.half.y, e.half.z);
        const bool disc = r > 1e-3f &&
                          std::abs(e.half.y - e.half.z) <= 0.3f * r &&
                          e.half.x <= 0.75f * r;
        if (byName || disc) { cands.push_back({&e, byName}); named += byName ? 1 : 0; }
    }
    if (named >= 4) // enough named wheels -> ignore the shape guesses
        cands.erase(std::remove_if(cands.begin(), cands.end(),
                                   [](const Cand& c) { return !c.byName; }),
                    cands.end());

    const float s = (forward == 1) ? -1.0f : 1.0f;
    const Entity* slot[4] = {nullptr, nullptr, nullptr, nullptr};
    float score[4] = {0, 0, 0, 0};
    for (const Cand& c : cands) {
        const glm::vec3 lc = c.e->localCenter;
        const float fz = lc.z * s;
        if (std::abs(lc.x) < 0.05f) continue; // centred (steering wheel, spare)
        const int   q  = (fz > 0.0f ? 0 : 2) + (lc.x > 0.0f ? 1 : 0);
        const float sc = std::abs(lc.x) + std::abs(fz);
        if (!slot[q] || sc > score[q]) { slot[q] = c.e; score[q] = sc; }
    }
    for (int i = 0; i < 4; ++i) out[i] = slot[i] ? slot[i]->id : -1;
}

} // namespace vehiclerig
