#pragma once

#include <random>

#include <glm/glm.hpp>

#include "Breakers.hpp"
#include "SpraySystem.hpp"

namespace ocean { class Ocean; }

// Where the open water throws spray, and how much. The pool and the drawing
// are SpraySystem's; this is only the deciding.
//
// Water throws spray where it is torn: off the lip of a breaker as it tips over
// (a sheet of droplets thrown shoreward, a haze hanging behind), off the crests
// of a sea in a gale (spume blown downwind -- the streaks a storm draws across
// the water), and where something hits it or drives through it.
//
// The sea and the surf are sampled at random points around the eye, more of
// them near than far, a few hundred a frame: spray is a speckle of events, and
// a random draw of where to look IS a speckle of events. Rates are per second,
// so the frame rate does not change how much it sprays.
namespace ocean {

struct SeaSprayInput {
    const Ocean*    sea    = nullptr;
    const ShoreMap* shore  = nullptr;
    Breakers        breakers;
    float           level  = 0.0f;   // still water
    glm::vec3       eye{0.0f};
    glm::vec2       windDir{1.0f, 0.0f};   // unit, toward
    float           windSpeed = 0.0f;      // m/s
    float           time = 0.0f;
    float           dt   = 0.0f;
    float           amount = 1.0f;         // the author's dial
};

void emitSeaSpray(const SeaSprayInput& in, SprayPool& pool, std::mt19937& rng);

// A body striking the water at `vel` (m/s), across `radius` (m): a crown of
// droplets and a ring of foam, as big as the blow.
void emitSplash(SprayPool& pool, std::mt19937& rng, glm::vec3 at, glm::vec3 vel, float radius,
                float amount = 1.0f);

// A body driving through the water: the bow wave's spray at `bow`, thrown out
// to both sides of `heading` (unit, horizontal) at `speed` (m/s). Call every
// frame with the frame's `dt`.
void emitBowSpray(SprayPool& pool, std::mt19937& rng, glm::vec3 bow, glm::vec2 heading,
                  float speed, float halfWidth, float dt, float amount = 1.0f);

} // namespace ocean
