#pragma once

#include <vector>

#include <glm/glm.hpp>

namespace fitzel {
struct MeshData;
}

// Tunnels for RoadSystem, and the exact mirror image of RoadBridge.
//
// A road grades the terrain to meet its profile, so a hill in the way is milled
// away into a cutting. A tunnel is the user saying "not here" about the OTHER
// direction: pick the two control points either side of the hill, hit Create
// tunnel, and the road holds its line straight through while the hill above it is
// left standing.
//
//   bridge: lift the profile onto the chord with max() -- rising ground cannot
//           swallow the deck.
//   tunnel: hold the profile down onto the chord with min() -- rising ground
//           cannot push the road up over the hill.
//
// Both then set `gradeW` to 0 over the stretch, smoothstepped across an abutment:
// the bridge so the terrain is not dragged up to a floating deck, the tunnel so
// the hill is not carved out from over the bore.
//
// WHY THERE IS NO HOLE IN THE TERRAIN. The terrain is a heightfield -- one height
// per grid cell -- and a heightfield cannot have a hole in it. It does not need
// one: the abutment ramp IS the cutting. The ground grades down into the hillside
// and then stops, which leaves a near-vertical face exactly where a tunnel mouth
// belongs, and the portal ring is what dresses that face. The bore itself is a
// tube of concrete hung under the hill, open at both ends and open at the bottom
// -- the road ribbon is its floor.
namespace roadtunnel {

// Bore styling, shared by every tunnel on the road.
struct Params {
    float clearHeight = 5.5f;  // headroom above the road at the crown
    float sideClear   = 1.0f;  // clearance beyond the road edge, each side
    float skirt       = 2.0f;  // how far the walls carry on below the road
    float portal      = 1.0f;  // how far the portal ring stands proud of the bore
    float abutment    = 10.0f; // arc length (m) the terrain ramps down over
};

// An inclusive run [begin,end] of centreline sample indices.
struct Span {
    int begin, end;
};

// Work out what the tunnels do to the road. `cores` are the sample runs the user
// asked to bore, one per Create tunnel. This:
//   - holds `prof` DOWN onto the straight chord each bore runs along,
//   - fills `gradeW` (parallel to prof) with how much the terrain should still be
//     graded to the road: 1 as usual, 0 over a bore, smoothstepped across the
//     abutments,
//   - returns the runs the bores actually cover -- each core widened into its
//     abutments, so a tube ends where the ground has come back down to the road.
// With no cores this is a no-op: an all-1 `gradeW` and no spans.
//
// `gradeW` is ASSIGNED, not combined, exactly as roadbridge::plan assigns it. A
// road with both wants the elementwise minimum of the two, which is the caller's
// business -- see RoadSystem::layout().
std::vector<Span> plan(const std::vector<glm::vec2>& center, std::vector<float>& prof,
                       const std::vector<Span>& cores, const Params& p,
                       std::vector<float>& gradeW);

// Append the bore (two walls and an arched crown) and a portal ring at each mouth
// to `md`. `prof` is the road surface the tube stands on and `bank` its cross-fall
// in degrees, which the tube ROLLS WITH for the same reason a deck does -- a bore
// built level around a banked carriageway puts one wall through the road.
//
// Every face is turned INWARD: a bore is a hole, and all of it is seen from the
// inside. The portal rings are the exception, and face out down the road.
void build(const std::vector<glm::vec2>& center, const std::vector<float>& prof,
           const std::vector<float>& bank, const std::vector<Span>& spans,
           float roadWidth, const Params& p, fitzel::MeshData& md);

#ifndef FITZEL_PLAYER
// The bore-styling sliders of the Roads panel. Returns true when something changed
// and the road therefore needs rebuilding.
bool panel(Params& p);
#endif

} // namespace roadtunnel
