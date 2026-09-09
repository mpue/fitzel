#pragma once

#include <vector>

#include <glm/glm.hpp>

namespace fitzel {
struct MeshData;
}

// Junctions: where two carriageways meet on the level.
//
// A road already knows how to cross another one -- it flies over it, or bores
// under it, or is simply lifted past it, and the ground query picks the branch
// below the asker's ceiling (see RoadSystem::surfaceHeightAt). What it could not
// do is meet one. Two ribbons at the same height fight for the depth buffer,
// their profiles are smoothed out of the base terrain independently and so miss
// each other by decimetres at the very point they are supposed to touch, and the
// corridor grading hands the shared ground to whichever road happens to be later
// in the list.
//
// --- Why nothing here is authored --------------------------------------------
// A bridge is a decision: the author points at a gap and says "not here". A
// junction is not a decision, it is a FACT about the drawing -- two centrelines
// meet, or they do not. So there is no Create button, no spec list, no entry in
// RoadSystem::Shape and no undo command: junctions are detected on every build
// from the geometry that is already there, and the panel lists what was found
// rather than offering something to add.
//
// The author still has the last word, and it is the word they already had: LIFT
// one branch. `clearance` metres of separation and the crossing reads as an
// over/under again, exactly as it did before this file existed. That works
// through ptLift, through a bridge chord and through a tunnel bore alike,
// because all three are in the profile by the time detection sees it.
//
// --- Three rules that keep Build idempotent -----------------------------------
// RoadSet builds in two passes -- trace every road, find the crossings, then
// build every road knowing them -- and roadcheck asserts that building twice
// changes nothing (tools/roadcheck.cpp). That survives only because every step
// is a pure function of the scene:
//
//   1. Detection runs on the UNPULLED trace, always. Feeding the pulled profile
//      back in would let a crossing that is rejected at the pulled height be
//      accepted at the unpulled one, and the road would flip between the two on
//      alternate builds.
//   2. The crossing list is OUTPUT ONLY. Written by every build, read by the
//      panel and the viewport overlay, and never an input to find(), assign() or
//      the profile. This is the one a well-meaning "let us cache the junctions so
//      Build is faster" patch breaks.
//   3. One pass of detection, one of building. Not a fixed-point solve.
//
// Everything in here is plain geometry: it takes polylines and hands back
// polylines and triangles. It knows nothing about RoadSystem, which is why a
// crossing between two DIFFERENT roads and a road crossing ITSELF are the same
// code path -- the two branches simply happen to come from one trace.
namespace roadjunction {

// Per road, saved with it (see RoadSystem::junctionStyle), and every rule for
// combining the two roads' copies at a crossing is written so that EITHER road
// can refuse or widen: min(clearance), max(blend), max(margin),
// max(minAngleDeg), and `enabled` is a veto. A junction is a thing two roads do
// together, so neither gets to impose it on the other.
struct Params {
    bool  enabled     = true;
    // Vertical separation at which the crossing stops being a junction and goes
    // back to being an over/under. Two storeys of a drivable flyover clear this
    // comfortably; two roads following the same hillside never do.
    float clearance   = 2.5f;   // metres
    // How far along each road the profile is ramped to reach the shared height.
    // Short enough to stay a local correction, long enough that a craft does not
    // feel a step. This is the knob to raise on steep ground.
    float blend       = 18.0f;  // metres, each side
    // How far the plate reaches past the far carriageway, so the two ribbons end
    // underneath it rather than butting against its edge.
    float margin      = 1.0f;   // metres
    // Below this the two roads are converging, not crossing. It is the one test
    // that keeps a service road running six metres from a main road -- and ending
    // beside it -- from being read as an junction; see the T rule in find().
    float minAngleDeg = 22.0f;
};

// One road as the finder needs to see it: the centreline and everything the
// profile already decided, taken from RoadSystem::layout() BEFORE any junction
// pull is applied (rule 1 above). Deriving it from the base terrain only is what
// makes the same call give the same answer however many corridors a previous
// Build cut.
struct Trace {
    std::vector<glm::vec2> center;
    std::vector<float>     prof;      // road surface height, parallel to center
    std::vector<float>     gradeW;    // 1 = on the ground, < 1 = deck, bore or abutment
    std::vector<float>     arc;       // arc length at each sample, arc[0] = 0
    std::vector<char>      standing;  // 1 = a loop stands on end at this sample
    float half    = 0.0f;             // surfaceHalf(): carriageway plus its lip
    bool  closed  = false;
    bool  enabled = true;
    Params params;
};

// One place two carriageways meet. `roadA == roadB` is a road crossing itself,
// and nothing downstream treats that as a special case.
struct Crossing {
    int   roadA = -1, roadB = -1;   // indices into the trace list
    int   segA  = -1, segB  = -1;   // the segment on each, and where along it
    float tA = 0.0f, tB = 0.0f;
    glm::vec2 at{0.0f};             // where they meet, world XZ
    float arcA = 0.0f, arcB = 0.0f; // arc length of `at` along each road
    float yA = 0.0f, yB = 0.0f;     // each branch's own unpulled height there
    float y  = 0.0f;                // the height both are pulled to
    float sinAngle = 0.0f;          // 1 = square, 0 = tangent
    glm::vec2 dirA{0.0f, 1.0f};     // road A's heading here; the apron's own axis
    bool  tee = false;              // one road ENDS here: three mouths, not four
    // The apron, world XZ, wound CCW seen from above. Carried on the crossing
    // rather than derived per road on purpose: both roads grade these cells, and
    // handing them the same polygon and the same height is what makes the result
    // independent of which road the list happens to build last.
    std::vector<glm::vec2> plate;
    // Where B's own station sits relative to `at`, along B, signed the way B's
    // arc runs. Zero for an X -- the two centrelines meet at a point on both. At
    // a T they do not: `at` is on A's centreline and B stops short of it, and
    // every length measured along B has to carry that gap.
    float offB = 0.0f;
    // The stretch of ribbon each road leaves out, as a window in its own arc
    // length: centre and half length. Not symmetric about the crossing in
    // general -- a road that ends just past it is cut on one side only.
    float cutAtA = 0.0f, cutA = 0.0f;
    float cutAtB = 0.0f, cutB = 0.0f;
};

// What one junction does to one road's PROFILE. The hole in its ribbon is not
// in here: that is decided against the apron polygon itself (see profile), not
// against a length along the road.
struct Pull {
    float target = 0.0f;  // the shared height
    // The road is held FLAT at `target` within `flat` of `flatAt`, and only then
    // ramped back to its own profile over `blend`. The flat span is not an
    // indulgence -- the apron IS flat, so a road still climbing underneath it
    // meets its edge at a step. That was the whole of the first version's
    // trouble: the ramp was centred on the crossing and had only reached a fifth
    // of the way by the time it got out from under the plate.
    float flatAt = 0.0f;
    float flat   = 0.0f;
    float blend  = 0.0f;
};
struct Plan {
    std::vector<Pull>     pulls;
    // The crossings this road DRAWS the apron for -- the lower-indexed road of
    // the pair, which for a self-crossing is that road. One plate, one mesh, one
    // set of collider triangles, however many roads meet on it.
    std::vector<Crossing> plates;
    // Every crossing this road takes part in, drawn or not. The corridor grading
    // needs all of them: the ground under an apron belongs to the apron whoever
    // draws it.
    std::vector<Crossing> grade;
    bool empty() const { return pulls.empty() && plates.empty() && grade.empty(); }
};

// Every crossing among `traces`, including each trace against itself. Order is
// stable (sorted by road pair then by station), so two runs on the same scene
// produce the same list in the same order.
//
// Cost is bounded by a uniform grid over the segments rather than by the square
// of the sample count: a three-kilometre circuit is fifteen hundred samples, and
// testing it against itself pair by pair is a million tests per Build.
std::vector<Crossing> find(const std::vector<Trace>& traces);

// Split the crossings into one Plan per trace (same size and order as `traces`).
std::vector<Plan> assign(const std::vector<Crossing>& xs,
                         const std::vector<Trace>& traces);

// What one road's plan does to its profile, its cross-fall and its ribbon, all
// three parallel to the road's samples:
//   `outOffset`   metres to ADD to prof -- exactly (target - prof) under the
//                 apron, easing to 0 by the end of the blend beyond it.
//   `outBankFade` 1 = keep the cross-fall, 0 = level. The apron is one flat
//                 polygon at one height; a branch still banked under it grades a
//                 tilted bed and pokes out of one side.
//   `outCut`      1 = leave the ribbon's forward quad out at this sample. Read by
//                 RoadSystem::loft exactly as a loop's footprint is.
// All three come back EMPTY when the plan is empty, which is the caller's cue to
// skip the work -- the same contract RoadSystem::pointRamp has.
//
// The hole is decided by the APRON, not by a length along the road: a quad is
// dropped only when BOTH its rungs have their full width -- centreline plus
// `half` either side -- inside the plate. Both halves of that matter. Testing
// the polygon rather than an arc window is what keeps a curved road honest,
// where arc length runs longer than the straight the apron was measured on.
// Testing BOTH rungs is what closes the gap the first version left: a quad is
// bounded by two samples, so dropping it on the strength of the first one alone
// removed road all the way out to the second, up to a sample spacing past the
// apron's edge.
void profile(const Plan& jp, const std::vector<glm::vec2>& center,
             const std::vector<float>& prof, const std::vector<float>& arc,
             float half, bool closed, std::vector<float>& outOffset,
             std::vector<float>& outBankFade, std::vector<char>& outCut);

// Append the aprons for `plates` to `md`, flat at each crossing's height.
// Winding matches the rest of the engine (CCW = front face).
//
// `fitted` picks between the two things an apron's texture can be, and they want
// opposite mappings:
//   false -- the carriageway's own asphalt, tiled in WORLD space so it runs
//            continuously through every mouth and the joint does not show.
//   true  -- a junction image (markings and all), mapped ONCE across the plate
//            and squared up with road A, so what the author painted lands where
//            they painted it. No tiling makes sense for that: a stop line drawn
//            twice is not a stop line.
void build(const std::vector<Crossing>& plates, float texTile, bool fitted,
           fitzel::MeshData& md);

// Is `p` inside the convex plate polygon? For the corridor grading, which has to
// flatten the ground the apron covers -- at an oblique crossing that reaches well
// outside both carriageways, and the natural ground would otherwise rise through
// the apron's corners.
bool insidePlate(const std::vector<glm::vec2>& plate, const glm::vec2& p);

#ifndef FITZEL_PLAYER
// The junction sliders of the Roads panel. Returns true when something changed
// and the road therefore needs rebuilding.
bool panel(Params& p);
#endif

} // namespace roadjunction
