#pragma once

#include <vector>

#include <glm/glm.hpp>

namespace fitzel {
struct MeshData;
}

// Bridges for RoadSystem.
//
// A road grades the terrain up to meet its profile, so a ravine it crosses gets
// filled in with an embankment. A bridge is the user saying "not here": pick the
// two control points either side of the gap, hit Create bridge, and the road flies
// straight between them on a deck while the ground below is left alone.
//
// The deck is a concrete cross-section (slab + parapets) swept along the road and
// hung under its ribbon, with piers dropped to the ground. Each span is widened by
// an abutment zone where the terrain still ramps up to the road, so the deck ends
// bury themselves in their own embankment instead of stopping in mid-air.
namespace roadbridge {

// Deck styling, shared by every bridge on the road.
struct Params {
    float deckThick   = 0.6f;  // slab thickness below the road surface
    float overhang    = 0.4f;  // deck edge beyond the road surface, each side
    float railHeight  = 1.1f;  // parapet height above the deck (0 = none)
    float railWidth   = 0.3f;  // parapet thickness
    float pierSpacing = 14.0f; // metres between piers (0 = none)
    float pierWidth   = 1.2f;  // side of the square pier column
    float abutment    = 6.0f;  // arc length (m) the terrain ramps up over
};

// An inclusive run [begin,end] of centreline sample indices.
struct Span {
    int begin, end;
};

// Work out what the bridges do to the road. `cores` are the sample runs the user
// asked to bridge, one per Create bridge. This:
//   - lifts `prof` onto the straight chord each deck flies along,
//   - fills `gradeW` (parallel to prof) with how much the terrain should still be
//     graded up to the road: 1 as usual, 0 under a deck, smoothstepped across the
//     abutments,
//   - returns the runs the decks actually cover -- each core widened into its
//     abutments, so a deck ends where the ground has risen to meet it.
// With no cores this is a no-op: an all-1 `gradeW` and no spans, i.e. exactly the
// plain road behaviour.
std::vector<Span> plan(const std::vector<glm::vec2>& center, std::vector<float>& prof,
                       const std::vector<Span>& cores, const Params& p,
                       std::vector<float>& gradeW);

// Append the deck (slab, parapets, end caps) and piers for every span to `md`.
// `prof` is the road surface the deck hangs under; `ground` is where the piers
// land. Winding matches the rest of the engine (CCW = front face).
void build(const std::vector<glm::vec2>& center, const std::vector<float>& prof,
           const std::vector<float>& ground, const std::vector<Span>& spans,
           float roadWidth, const Params& p, fitzel::MeshData& md);

#ifndef FITZEL_PLAYER
// The deck-styling sliders of the Roads panel. Returns true when something changed
// and the road therefore needs rebuilding.
bool panel(Params& p);
#endif

} // namespace roadbridge
