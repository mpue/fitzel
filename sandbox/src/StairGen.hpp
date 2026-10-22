#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "SceneTypes.hpp"

// Procedural staircases: a height and a type in, a stair you can walk up out --
// straight, quarter- and half-turn (with a landing or with winders), spiral and
// curved; built massive, on a concrete slab, between two stringers, on a single
// spine or as floating treads out of a wall; with posts, balusters, glass or a
// solid parapet as the railing.
//
// Two stages, both public because the editor shows the first one:
//
//   layout()   Params -> Plan. Pure 2D plus heights: every tread (landings and
//              the arrival platform included) as a convex polygon in plan with
//              the height it is walked on, its two sides as polylines (the
//              stringers, railings and walls follow those) and the figures a
//              stair is judged by -- rise, going, pitch, the step rule -- plus
//              the checks run on them.
//   generate() Plan -> entity subtree. An Empty root carrying the parameters
//              (StairComponent, so a placed stair can be re-opened and re-tuned
//              after a save/load) and a MeshComponent child per part: treads,
//              structure, railing, glass, walls.
//
// Every stair kind is built by one small "turtle" that lays flights, landings
// and winder fans one after the other -- always turning LEFT. A right-hand
// stair is that plan mirrored, so there is exactly one copy of each shape.
//
// Plan frame: x to the right of the first flight, y up the first flight, z up.
// Model frame: local X = x, local Y = z, local Z = -y -- the first flight climbs
// towards -Z, and the root stands at the foot of the stair: the middle of the
// bottom edge of the first riser, on the ground.
//
// Like HouseGen this module knows nothing about ImGui or the GPU; the panel is
// StairPanel, the placement lives in main.
namespace stairgen {

// Saved as an index: append only.
enum class Kind {
    Straight,   // one flight, optionally broken by a landing
    LLanding,   // quarter turn on a square landing
    LWinder,    // quarter turn on winders (viertelgewendelt)
    ULanding,   // half turn on a landing across both flights
    UWinder,    // half turn on winders (halbgewendelt)
    Spiral,     // round a column (Spindeltreppe)
    Curved,     // round an open eye (Bogentreppe)
    Count
};
const char* kindName(Kind k);
bool isTurning(Kind k);   // has a left/right
bool isRound(Kind k);     // Spiral or Curved

// Saved as an index: append only.
enum class Structure {
    Solid,       // every step a block down to the ground (outdoor, cellar)
    Slab,        // steps on an inclined concrete slab
    Stringers,   // treads between two side stringers (Zweiholm / Wangen)
    Spine,       // treads on one central beam (Mittelholm)
    Floating,    // treads only, anchored in the wall or the column
    Count
};
const char* structureName(Structure s);

// Saved as an index: append only.
enum class RailStyle {
    Posts,       // posts, handrail and two rails between them
    Balusters,   // a handrail on vertical bars, no gap over 12 cm
    Glass,       // glass panels under a handrail
    Parapet,     // a solid half-height wall
    Count
};
const char* railStyleName(RailStyle s);

// Which sides, in the walking direction up the stair. On a turning stair Left
// is the INNER side and Right the outer one, whichever way it turns -- so a
// railing at the eye stays at the eye when the stair is flipped. Saved as an
// index.
enum class Sides { None, Left, Right, Both, Count };
const char* sidesName(Sides s);

// Saved as an index: append only. A look is a set of colours and surfaces;
// stairs of the same look share their materials ("Stair Oak Tread", ...), so
// re-tinting one look never repaints a stair built in another.
enum class Look {
    OakSteel,     // oiled oak treads, dark steel, stainless rail
    Concrete,     // fair-faced concrete, black steel rail
    Sandstone,    // natural stone
    Marble,       // white marble, brass rail
    WhiteModern,  // white everything, light oak treads
    Rustic,       // dark timber
    Industrial,   // black steel, galvanised rail
    Custom,
    Count
};
const char* lookName(Look l);
// The colours of a look, written into `p` (Custom leaves them alone).
void applyLook(struct Params& p, Look l);

struct Params {
    Kind  kind       = Kind::ULanding;
    bool  turnRight  = false;     // turning stairs: which way the turn goes

    // --- Size ---------------------------------------------------------------
    float height     = 2.80f;     // total rise, floor to floor (m)
    float width      = 1.00f;     // clear flight width (m)
    bool  autoRisers = true;      // risers from the height and `targetRise`
    int   risers     = 16;        // when not auto
    float targetRise = 0.175f;    // what auto aims for
    bool  autoGoing  = true;      // going from the step rule 2R + G = 63 cm
    float going      = 0.27f;     // tread depth, nosing to nosing (m)

    // --- Plan ---------------------------------------------------------------
    int   splitStep    = 0;       // treads before the turn / mid landing; 0 = half
    bool  midLanding   = false;   // straight stair: a landing part-way up
    float landingDepth = 1.00f;   // straight and U landings (L landings are square)
    float eye          = 0.15f;   // U stairs: gap between the flights
    int   winders      = 3;       // L winder: per quarter turn (U: twice this)
    float innerRadius  = 0.12f;   // round stairs: radius of the column / eye
    bool  autoTurn     = true;    // round stairs: turn from the going on the walking line
    float turn         = 360.0f;  // round stairs: total turn when not auto (deg)
    float topPlatform  = 0.0f;    // arrival platform depth (0 = arrive at a floor edge)

    // --- Construction -------------------------------------------------------
    Structure structure      = Structure::Stringers;
    float     treadThickness = 0.05f;
    float     nosing         = 0.03f;   // tread overhang over the riser below
    bool      closedRisers   = false;   // board in every riser (board structures)
    float     stringerWidth  = 0.06f;
    float     stringerDepth  = 0.30f;   // stringer / spine / slab depth
    bool      column         = true;    // round stairs: a centre column

    // --- Railing ------------------------------------------------------------
    Sides     railSides     = Sides::Both;
    RailStyle railStyle     = RailStyle::Balusters;
    float     railHeight    = 0.90f;    // above the nosing line
    float     balusterGap   = 0.11f;    // clear gap between bars
    Sides     wallSides     = Sides::None;
    float     wallHeight    = 2.30f;    // above the nosing line
    float     wallThickness = 0.17f;

    // --- Look ---------------------------------------------------------------
    Look      look = Look::OakSteel;
    glm::vec3 treadColor{0.63f, 0.48f, 0.33f};
    glm::vec3 structureColor{0.13f, 0.13f, 0.14f};
    glm::vec3 railColor{0.74f, 0.75f, 0.77f};
    glm::vec3 wallColor{0.90f, 0.89f, 0.86f};

    bool collider = true;         // static physics on everything you can touch

    bool operator==(const Params&) const = default;
};

// Starting points. Each sets the type, the size, the construction, the
// railing and the look -- a whole stair in one click.
enum class Preset {
    Cellar,        // straight, concrete, wall on both sides, wall handrail
    Straight,      // straight timber stair between stringers
    LLanding,      // quarter landing, oak and steel
    LWinder,       // quarter winder, the classic house stair
    ULanding,      // half landing, concrete slab, glass railing
    UWinder,       // half winder in timber
    Spiral,        // steel spiral round a column
    Curved,        // marble sweep with balusters
    Floating,      // cantilevered oak treads out of a white wall
    Outdoor,       // wide sandstone steps, solid, no railing
    FireEscape,    // open steel U stair with landings
    Grand,         // wide marble stair with a mid landing
    Count
};
const char* presetName(Preset p);
const char* presetHint(Preset p);
void applyPreset(Params& p, Preset preset);

// --- The plan --------------------------------------------------------------

struct Tread {
    // Sides in plan, both from the front (riser) edge to the back edge. A
    // winder's inner side is the one pivot point. The polygon is `right`
    // followed by `left` reversed: convex, counter-clockwise.
    std::vector<glm::vec2> left, right;
    float top       = 0.0f;   // walking surface above the foot of the stair
    float lineFront = 0.0f;   // nosing line at the front edge
    float lineBack  = 0.0f;   //   ... and at the back (equal on a landing)
    bool  landing   = false;  // flat: a landing or the arrival platform
    bool  arrival   = false;  // the platform at the top
    float walkFront = 0.0f;   // distance along the walking line, front edge
    float walkBack  = 0.0f;   //   ... back edge

    std::vector<glm::vec2> polygon() const;
};

struct Plan {
    Params params;            // after sane() -- what is actually built
    std::vector<Tread> treads;
    int   risers  = 0;
    float rise    = 0.0f;     // per riser
    float going   = 0.0f;     // on the walking line
    float run     = 0.0f;     // walking-line length from the first riser to the top
    float pitch   = 0.0f;     // degrees
    float stepRule = 0.0f;    // 2R + G (m), 0.59..0.65 is comfortable
    float turnDeg = 0.0f;     // round stairs: total turn
    float walkRadius = 0.0f;  // round stairs: radius of the walking line
    float outerRadius = 0.0f; // round stairs
    glm::vec2 centre{0.0f};   // round stairs: plan position of the axis
    glm::vec2 bbMin{0.0f}, bbMax{0.0f};   // footprint of the treads
    // Railing and walls resolved to the sides of the plan as built (the
    // parameters name them for the left-turning stair; a right-hand one is
    // its mirror image).
    bool railLeft = false, railRight = false, wallLeft = false, wallRight = false;
    std::vector<glm::vec2> walkLine;      // plan polyline, foot to top
    std::vector<std::string> notes;       // checks, the first `warnings` are problems
    int warnings = 0;
};

Plan layout(const Params& p);

// --- The model ---------------------------------------------------------------

// The shared surface materials of one look ("Stair Oak Tread", ...), re-tinted
// from the parameters on every build.
struct Palette {
    fitzel::AssetId tread, structure, rail, glass, wall;
};
Palette ensurePalette(std::vector<MaterialDef>& materials, const Params& p);

// Build the stair. Returns a parent-before-child entity list whose front() is
// an Empty root standing at `footPos` -- the shape AddEntitiesCmd and
// prefab::fromSubtree want. `yawDeg` turns it about the vertical (0: the first
// flight climbs towards -Z).
std::vector<Entity> generate(const Params& p, const Palette& pal, int& entityCounter,
                             const glm::vec3& footPos, float yawDeg = 0.0f);

// Faces the current parameters produce, for the panel's footer.
int faceCount(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace stairgen

// The parameters a stair was generated from, kept on its root -- like
// HouseComponent: not in the Add Component menu, but saved with the scene and
// with a prefab, so "Edit selected stair" can re-open any stair ever placed.
class StairComponent : public ComponentBase {
public:
    stairgen::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<StairComponent>(*this);
    }
    const char* typeId() const override { return "stair"; }
    const char* displayName() const override { return "Stair"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none; // edited in its panel
    }
    void save(nlohmann::json& j) const override { stairgen::saveParams(j, params); }
    void load(const nlohmann::json& j) override { stairgen::loadParams(j, params); }
};
