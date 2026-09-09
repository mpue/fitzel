#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Terrain.hpp>   // TerrainSettings -- a plain struct, no GL

#include "CityGen.hpp"                // city::Biome
#include "RoadDecal.hpp"              // roaddecal::Decal
#include "RoadLoop.hpp"               // roadloop::Spec
#include "RoadSide.hpp"               // roadside::Line

// A whole race scene from one seed: the landscape, one closed circuit laid out
// on it, the race logic that makes it a race, and the dressing that makes it a
// place.
//
// --- Pure, and why that is not tidiness ---------------------------------------
// This takes a seed, some parameters and a function that answers how high the
// ground is, and returns a DESCRIPTION -- control points, lifts, banks, spans,
// rules and markers. It constructs no RoadSystem (that is a Shader, an
// AssetDatabase and a GL context, see RoadSystem.hpp), touches no Document, and
// has never heard of an Entity. The editor's apply step turns the description
// into a world.
//
// That split is what lets a harness measure a whole circuit -- its tightest
// corner, its steepest slope, whether its checkpoints can be flown through -- in
// a program with no window open. It is the same bargain RaceGrid.hpp makes for
// the starting grid, one level up, and for the same reason: none of those
// questions is about pixels.
//
// --- The ground is handed IN --------------------------------------------------
// `groundAt` rather than a call to fitzel::terrainBaseHeight, because that
// function answers 0 for everywhere while terrainPresent() is false -- a global
// atomic. A generator that called it directly would, in a harness that never set
// the flag, lay every circuit on a flat void: no gradient anywhere, no bridge,
// no tunnel, and every test passing for the wrong reason. Injecting the sampler
// makes that mistake impossible to make by accident.
//
// It takes the SETTINGS as an argument rather than closing over them, so the
// generator can soften the land and ask again. Alpine relief and a nine percent
// gradient limit are two demands that cannot both be met, and the useful answer
// is a gentler landscape with the circuit the author asked for on it -- not a
// report saying no. The settings it settled on come back in Level::terrain, and
// that is what the caller must apply.
//
// --- Deterministic ------------------------------------------------------------
// The same Params and the same sampler give the same Level, down to the order of
// the spans. Nothing here reads a global, a clock or a random_device.
namespace levelgen {

// How the circuit is laid out in plan.
//
// Named shapes rather than one "how much does it cross itself" float, because
// such a knob has a discontinuity in the middle of it: below some value the ring
// does not cross at all and every feature that depends on a crossing is silently
// absent. And named shapes rather than more noise, because what makes two
// circuits feel different is not how bumpy their outline is -- it is whether
// they have straights, and whether they cross.
//
// Every one of them is the same pipeline: a base curve, sampled at equal arc
// length, perturbed along its own normal, then relaxed until its corners are
// drivable. Only the base curve differs, and the crossings are FOUND on it
// rather than declared per shape -- which is why adding one is a function, not
// a special case.
enum class Shape {
    Ring,      // an irregular natural circuit; no straights, no crossings
    Speedway,  // an oval: two long straights and two constant-radius banked turns
    Street,    // a rounded rectangle: four straights and four hard corners
    Eight,     // a figure of eight -- one crossing
    Knot,      // a trefoil -- three crossings, and the shape junctions are for
    Count
};

// For the panel's combo, and for anything that has to name one.
[[nodiscard]] const char* shapeName(Shape s);

// What a self-crossing IS, once there is one. Decided here rather than
// discovered later: roadjunction detects crossings from the drawing (see
// RoadJunction.hpp), so the generator's job is to make the geometry say the
// thing it meant.
enum class Crossing {
    Flyover,  // one branch lifted past roadjunction::Params::clearance
    Level     // both branches held to one height; roadjunction lays an apron
};

// A stretch named by the two control points at its ends -- the same naming
// RoadSystem::BridgeSpec uses. Its own type so this header does not have to
// include RoadSystem.hpp and drag a GL context into everything downstream.
struct Span { int a = 0, b = 0; };

struct Params {
    unsigned seed = 1;

    // --- The land ------------------------------------------------------------
    float relief      = 1.0f;   // 0.2 = rolling plain .. 2.0 = alpine
    float maxGradient = 0.09f;  // rise over run the finished road may not exceed
    // How far the road may leave the ground. Generous on purpose: a big
    // deviation is not an embankment, it is a viaduct or a cutting -- the
    // feature pass turns anything past six metres up into a bridge and anything
    // past eight down into a bore. Holding a gentle gradient across eight
    // kilometres of hill country needs tens of metres in places, and a limit
    // that forbids it does not produce a flatter track, it produces a steeper
    // one.
    float maxLift     = 45.0f;  // metres

    // --- The circuit ---------------------------------------------------------
    Shape shape       = Shape::Ring;
    float length      = 3000.0f;// target centreline length (m)
    int   corners     = 12;     // control points around the lap
    float irregular   = 0.35f;  // 0 = a plain oval .. 1 = wildly uneven
    float width       = 20.0f;  // carriageway width (hellfire's is 20)
    float maxBank     = 14.0f;  // degrees of cross-fall in the tightest corner
    // The speed the AI must be able to hold in the tightest corner. This is what
    // SETS the minimum radius (see minRadius): the opponents corner at
    // sqrt(grip / curvature) and know nothing about how the track was made.
    float cornerPace  = 26.0f;  // m/s

    // --- Features ------------------------------------------------------------
    Crossing crossing = Crossing::Flyover;  // read only when shape == Eight
    float    flyover  = 9.0f;   // metres one branch is lifted
    int      bridges  = 2;      // a ceiling; fewer if the ground offers fewer
    int      tunnels  = 1;
    // Vertical loops. 0 by default and deliberately: roadloop leaves the loop's
    // stretch out of the flat ribbon, and the opponents drive that ribbon -- so a
    // generated loop is something the player rides and the field walks through.
    int      loops    = 0;

    // --- The race ------------------------------------------------------------
    int   checkpoints   = 4;
    int   gridSlots     = 8;
    float laps          = 3.0f;
    // Tick slot 0 for the player. On, because a circuit opened straight from the
    // editor has no craft otherwise: with no marker claiming the player,
    // racegrid has no prefab to build one from.
    bool  pinPlayerPole = true;

    // --- Dressing ------------------------------------------------------------
    float cityAmount  = 0.6f;   // 0 = no district .. 1 = a canyon most of the lap
    bool  sideObjects = true;

    // --- Names. Settings rather than constants: these are the prefabs of ONE
    //     project, and a missing one must cost the LOOK of an object and nothing
    //     else -- the race is defined by the components.
    std::string playerPrefab = "player_sf_fighter_cam";
    std::string rivalPrefab  = "opp_sf_fighter";
    std::string finishPrefab = "start_finish";
    // Not "Checkpoint": there are two prefabs by that name in the roads project
    // and one of them carries a gate of 12 x 6 x 3, which on a 20 m road is a
    // gate the opponents' racing line never reaches -- a race nobody finishes.
    std::string checkPrefab  = "CheckpointPortal";
    std::string railModel;                    // "" = the guard rail draws nothing
    std::string startDecal   = "roads_basic_crossing.png";
};

// The raised lip along the carriageway. Fixed rather than exposed: it is part of
// what "a track" means here (a craft drifting wide is turned back down instead
// of sailing off), and it enters minRadius through surfaceHalf.
inline constexpr float kEdgeWidth = 3.0f;    // metres up the slope
inline constexpr float kEdgeAngle = 55.0f;   // degrees from the carriageway

// The AI's own cornering law: vCorner = sqrt(grip / curvature)
// (RaceSim.cpp, and OpponentComponent::grip's default in Component.hpp).
inline constexpr float kAiGrip = 14.0f;

// Half the drivable section, and the tightest corner that section may take.
// Both derived in ONE place so the generator, the panel's report and the harness
// cannot disagree about what "drivable" means.
[[nodiscard]] float surfaceHalf(const Params& p);
[[nodiscard]] float minRadius(const Params& p);

// One placed piece of race logic. Position, heading and a gate is all any of the
// three needs; the apply step turns it into an entity (or a prefab instance) and
// nothing here knows what a prefab is.
struct Marker {
    enum class Kind { Finish, Checkpoint, Grid };
    Kind      kind = Kind::Checkpoint;
    // The road surface point. For a gate that is where the gate box's centre
    // sits on the carriageway; for a grid slot it is where the craft's marker
    // goes, and the craft floats its own ride height above it.
    glm::vec3 pos{0.0f};
    // Scene yaw in degrees, in the convention every craft here uses:
    // dir = (sin H, 0, cos H). Written into an Euler triple ONLY as
    // (0, headingDeg, 0) -- see sceneHeading in SandboxMath.hpp for why the
    // middle component is not a heading in general.
    float     headingDeg = 0.0f;
    // Gate box. Always filled, never left for the prefab to supply: one of the
    // prefabs a user might name here carries no gate fields at all, and the
    // default it then loads is unraceable on a wide road.
    float     gateW = 0.0f, gateH = 0.0f, gateD = 0.0f;
    int       slot   = 0;       // Grid only
    bool      player = false;   // Grid only
    std::string prefab;         // the name this marker asks for ("" = none)
    float     station = 0.0f;   // arc length along the centreline; diagnostics
    int       lobe    = 0;      // which half of a figure of eight; 0 on a ring
};

// The circuit, in exactly the terms RoadSystem holds a road in.
struct Track {
    std::vector<glm::vec2> points;   // control points, world XZ
    std::vector<float>     lift;     // parallel to points, metres
    std::vector<float>     bank;     // parallel to points, degrees
    bool  closed    = true;
    float width     = 20.0f;
    float grade     = 0.90f;
    float shoulder  = 8.0f;
    float edgeWidth = kEdgeWidth;
    float edgeAngle = kEdgeAngle;
    std::vector<Span>           bridges;
    std::vector<Span>           tunnels;
    std::vector<roadloop::Spec> loops;
};

// What the generator measured about what it produced.
//
// Not a nicety. A generator whose only output is geometry can be judged only by
// driving it, and a seed that produces one undrivable corner in two hundred is
// not something anyone finds that way.
struct Report {
    float length            = 0.0f;
    float minRadius         = 0.0f;  // tightest corner on the SAMPLED centreline
    float maxGradient       = 0.0f;
    float maxBank           = 0.0f;
    float crossingClearance = 0.0f;  // the TIGHTEST vertical separation (m)
    float minSelfDistance   = 0.0f;  // closest approach of two distant stretches
    int   crossings         = 0;
    int   bridges = 0, tunnels = 0, loops = 0;
    int   droppedFeatures   = 0;     // asked for, but the track had no room
    int   corridorCells     = 0;     // 1 m cells RoadSet::buildAll will visit
    bool  ok = true;
    std::string why;                 // the first constraint that was NOT met
};

struct Level {
    fitzel::TerrainSettings       terrain;
    Track                         track;
    std::vector<roadside::Line>   sideLines;
    std::vector<roaddecal::Decal> decals;
    std::vector<city::Biome>      biomes;
    std::vector<Marker>           markers;
    Report                        report;
};

// The landscape this seed asks for.
//
// Separate from generate() because of the chicken and egg: the circuit is laid
// out AGAINST a landscape, so the caller must have that landscape -- and a
// sampler for it -- before it can ask for a circuit.
//
// TerrainSettings::seed is a NOISE-COORDINATE OFFSET, not RNG state, so this
// steps it by thousands. Stepping it by ones gives the same landscape shifted a
// metre, which reads as the seed not working.
[[nodiscard]] fitzel::TerrainSettings terrainFor(const Params& p);

// Everything that stands on it. `groundAt(x, z)` must answer for exactly the
// settings terrainFor() returned, and must be the BASE height with no edits
// layered on -- RoadSystem::layout grades its corridor from the base too,
// precisely so that a rebuild is idempotent, and a generator that measured the
// EDITED ground would lay a different circuit every time it ran on a scene that
// already had one in it.
using GroundFn = std::function<float(const fitzel::TerrainSettings&, float, float)>;

[[nodiscard]] Level generate(const Params& p, const GroundFn& groundAt);

// The road's own longitudinal smoothing, reproduced so the generator can predict
// the profile RoadSystem::layout will settle on. Shared with the harness, which
// has to measure the gradient of the road that will exist rather than of the
// control points that were asked for.
//
// A duplicate of RoadSystem's private `smooth`/`passesFor`, deliberately: the
// alternative is exporting them from a header that needs a GL context. Every
// gradient target here carries a margin so a small drift is harmless.
void  lowPass(std::vector<float>& profile, int passes);
[[nodiscard]] int passesForSigma(float sigmaMetres);

// Over how long a stretch a slope counts as a slope. Two adjacent centreline
// samples are two metres apart, and the rise between two points two metres apart
// is a bump, not a gradient -- the road's own smoothing already bounds those.
// Twenty metres is about a craft and its suspension.
inline constexpr float kGradientWindow = 20.0f;

// The steepest slope in a profile, measured over kGradientWindow rather than
// between neighbouring samples. `arc` and `profile` are parallel, and the
// profile is treated as a ring. One definition, shared by the generator, the
// panel's report and the harness, for the same reason surfaceHalf is.
[[nodiscard]] float worstGradient(const std::vector<float>& arc,
                                  const std::vector<float>& profile);

} // namespace levelgen
