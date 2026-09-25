#pragma once

#include <array>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "SceneTypes.hpp"

// Procedural single-family house: a ROOM PROGRAMME in ("EG: kitchen, living,
// hall, guest bath -- OG: two bedrooms and a bath -- DG: a child's room and an
// office"), a finished house out -- floor plans, a living-area calculation and
// a walkable 3D model with real openings, a staircase and a pitched roof.
//
// Two stages, and both are public because the editor shows the first one:
//
//   layout()   programme -> Plan. Pure 2D: rooms, walls, doors, windows and the
//              stair as rectangles in plan metres, plus the areas (German WoFlV
//              rules: stairs deducted, attic floor under 2 m at half) and the
//              checks an architect would run first (daylight >= 1/8 of the
//              floor, every room reachable, sizes near what was asked for).
//   generate() Plan -> entity subtree. An Empty root carrying the parameters
//              (HouseComponent, so a placed house can be re-opened and re-tuned
//              after a save/load) and a handful of MeshComponent children --
//              walls per storey, slabs, stair, roof, windows, doors -- each one
//              EditMesh with a material per face.
//
// Why meshes and not the editor's primitives, which is what BuildingGen builds
// towers from: a wall here has holes in it. A tower's windows are a shader
// pattern on a box; a house you can walk into needs a door you fit through, a
// window you can look out of and a roof that follows the slope, and a stack of
// boxes that does all three is several hundred objects in the hierarchy. As one
// EditMesh per part it is eight, it collides as the triangles it draws (static
// bodies do, see PhysicsShapes.cpp) and it stays editable in the Modeling panel.
//
// The layout is deliberately one well-understood TYPE rather than a general
// space planner: the classic German "Nebenraumzone" plan. A service band along
// the north wall (hall with a U-stair in the middle, kitchen/baths/storage left
// and right of it), a bearing wall, and the main rooms side by side along the
// south facade. Every column of the service band lines up through all storeys,
// so the wet rooms stack and the walls stand on walls. The ridge runs north-
// south (gable to the street), which is what keeps the stair out from under the
// roof slope in the attic.
//
// Plan frame: x east (0..width), y south (0..depth), z up from the ground.
// Model frame: local X = x - width/2, local Y = z, local Z = y - depth/2 --
// north is -Z, the root stands at the centre of the footprint.
//
// Like BuildingGen this module knows nothing about ImGui or the GPU; the panel
// is HousePanel, the placement lives in main.
namespace housegen {

// Saved as an index: append only.
enum class RoomType {
    Wohnen,     // living room
    Essen,      // dining
    Kueche,     // kitchen
    Diele,      // hall / landing -- the room the stair stands in
    Bad,        // bathroom
    GaesteBad,  // guest WC / shower room
    Schlafen,   // bedroom (parents)
    Kind,       // child's room
    Buero,      // office
    Gast,       // guest room
    HWR,        // utility / plant room
    Abstell,    // storage
    Garderobe,  // cloakroom
    Ankleide,   // walk-in wardrobe
    Count
};
const char* roomTypeName(RoomType t);
// A sensible target area for a new room of this type (m^2).
float defaultArea(RoomType t);
// "Aufenthaltsraum": a room people stay in, which the building codes require
// to have daylight of at least 1/8 of its floor area.
bool isHabitable(RoomType t);
// Goes into the service band on the north side rather than the main band.
bool isService(RoomType t);

struct RoomSpec {
    RoomType    type = RoomType::Wohnen;
    std::string name;          // shown in the plan; empty = the type's name
    float       area = 15.0f;  // target floor area (m^2) -- a WEIGHT, see layout()
    bool operator==(const RoomSpec&) const = default;
};

constexpr int kMaxStoreys = 3;   // EG, OG, 2. OG (plus the attic on top)

struct Params {
    // --- Building ---------------------------------------------------------
    float width        = 10.00f;  // outside, east-west (m)
    float depth        = 8.50f;   // outside, north-south (m)
    int   storeys      = 2;       // full storeys: EG .. (1..3)
    bool  attic        = true;    // attic fitted out as rooms (else a loft)
    float storeyHeight = 2.80f;   // floor to floor
    float clearHeight  = 2.50f;   // floor to ceiling (slab = the difference)
    float plinth       = 0.30f;   // ground floor above the ground
    // Clear depth of the service band along the north wall. It holds the
    // stair (2.00 m) plus the passage beside it, so it cannot go below 3.10.
    float serviceDepth = 3.335f;
    float outerWall    = 0.365f;  // monolithic brick
    float bearingWall  = 0.175f;  // the spine wall and the one beside the stair
    float partition    = 0.115f;  // everything else
    bool  terrace      = true;    // timber deck outside the garden door

    // --- Roof -------------------------------------------------------------
    float roofPitch     = 40.0f;  // degrees
    float kneeWall      = 1.00f;  // "Kniestock": attic wall height at the eaves
    float eaveOverhang  = 0.50f;
    float gableOverhang = 0.30f;

    // --- What gets built (a cut-away is a house without its upper floors) --
    bool roof     = true;
    int  cutLevel = 3;            // highest level built, 0 = EG only
    bool collider = true;         // static physics on everything but the doors

    // --- Look ----------------------------------------------------------------
    glm::vec3 facadeColor{0.93f, 0.92f, 0.88f};  // render, broken white
    glm::vec3 plinthColor{0.26f, 0.28f, 0.29f};  // anthracite
    glm::vec3 roofColor{0.56f, 0.23f, 0.15f};    // clay tile
    glm::vec3 frameColor{0.19f, 0.20f, 0.21f};   // window frames, gutters
    glm::vec3 floorColor{0.63f, 0.48f, 0.33f};   // oak

    // --- Room programme ----------------------------------------------------
    // Per full storey, EG first. The attic has its own list so that adding a
    // storey does not silently move the attic's rooms down into it.
    std::array<std::vector<RoomSpec>, kMaxStoreys> storeyRooms;
    std::vector<RoomSpec>                          atticRooms;

    Params();   // the classic three-level family house
    bool operator==(const Params&) const = default;
};

// Starting points, one per common German house type. Like BuildingGen's styles
// they set the building and the room programme and leave the colours alone.
enum class Preset {
    Classic,        // 2 storeys + attic, the family house on the drawing board
    Bungalow,       // everything on one level
    Townhouse,      // narrow and tall, three storeys
    SemiDetached,   // Doppelhaushaelfte: narrow, deep, 2 storeys + attic
    TownVilla,      // Stadtvilla: square, two full storeys, low roof
    CountryHouse,   // 1.5 storeys: high knee wall, most rooms under the roof
    LargeFamily,    // five bedrooms and a guest room
    Cottage,        // small holiday house, sleeping under the roof
    MultiGen,       // grandparents on the ground floor, family above
    Count
};
const char* presetName(Preset p);
void applyPreset(Params& p, Preset preset);

// --- The plan --------------------------------------------------------------

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    float area() const { return w() * h(); }
};

enum class Side { N, S, W, E, None };

struct Wall {
    Rect r;
    bool alongX   = true;   // runs east-west (its length is along x)
    Side exterior = Side::None;
    bool bearing  = false;
};

struct Opening {
    enum class Kind { Door, EntryDoor, Pass, Window, FrenchDoor };
    Kind  kind   = Kind::Window;
    Rect  r;                // footprint, spanning the full wall thickness
    bool  alongX = true;    // same as the wall it sits in
    Side  exterior = Side::None;
    float sill = 0.0f;      // above the storey's finished floor
    float head = 2.0f;
    // Door leaf in plan, opened 90 degrees: it hangs at `hinge`, its free edge
    // is at `open`, and the swing arc ends at `close` (the other jamb).
    glm::vec2 hinge{0.0f}, open{0.0f}, close{0.0f};
    bool  hasLeaf = false;
    int   room    = -1;     // the room a window lights (index into rooms)
    float width() const { return alongX ? r.w() : r.h(); }
    float area() const { return width() * (head - sill); }
};

// A roof window, as the plan rectangle it covers on one slope.
struct RoofWindow {
    Rect r;
    bool east = false;
    int  room = -1;
    float area = 0.0f;      // glazed area along the slope
};

struct PlanRoom {
    RoomType    type = RoomType::Wohnen;
    std::string name;
    Rect        r;
    float target     = 0.0f;  // what was asked for (0 = no target)
    float floorArea  = 0.0f;  // clear floor area
    float livingArea = 0.0f;  // counted per WoFlV
    float daylight   = 0.0f;  // window + roof window area
    bool  hall       = false; // the hall/landing with the stair
    bool  autoAdded  = false; // filled in by the generator, not in the programme
    bool  reachable  = false; // has a door (or pass) to the hall or a neighbour
};

struct LevelPlan {
    std::string label;          // "EG", "OG", "1. OG", "2. OG", "DG"
    float z         = 0.0f;     // finished floor, above the ground
    bool  attic     = false;
    bool  loft      = false;    // unfinished attic (no rooms, not living area)
    bool  stairUp   = false;
    bool  stairDown = false;
    std::vector<PlanRoom>   rooms;
    std::vector<Wall>       walls;
    std::vector<Opening>    openings;
    std::vector<RoofWindow> roofWindows;
    float living = 0.0f;
};

struct Stair {
    Rect  r;                    // the whole U: both flights and the landing
    float landingX = 0.0f;      // flights run x0..landingX, landing beyond
    float flight   = 0.95f;     // width of one flight
    float tread    = 0.27f;
    int   risers   = 16;        // per storey, 8 per flight
    float rise     = 0.175f;
};

struct Plan {
    Params params;              // after sane() -- what was actually built
    std::vector<LevelPlan> levels;
    bool  hasStair = false;
    Stair stair;
    float living     = 0.0f;    // total living area
    float grossFloor = 0.0f;    // BGF, all levels
    float eaveZ      = 0.0f;    // roof edge above the ground
    float ridgeZ     = 0.0f;    // ridge above the ground
    std::vector<std::string> notes;   // plan checks, most important first
    int warnings = 0;                 // the first `warnings` notes are problems,
                                      // the rest are for information
};

Plan layout(const Params& p);

// Underside of the roof (m above the ground) at plan position x, for this plan.
float roofUnderside(const Plan& plan, float x);

// --- The model ---------------------------------------------------------------

// The shared surface materials a generated house references. One set per
// project ("House Facade" ...), re-tinted from the parameters on every build.
struct Palette {
    fitzel::AssetId facade, plinth, roof, timber, interior, floor, frame, glass,
                    door, stair;
};
Palette ensurePalette(std::vector<MaterialDef>& materials, const Params& p);

// Build the house. Returns a parent-before-child entity list whose front() is an
// Empty root standing at `groundPos` (the centre of the footprint, on the
// ground) -- the shape AddEntitiesCmd and prefab::fromSubtree want.
std::vector<Entity> generate(const Params& p, const Palette& pal,
                             int& entityCounter, const glm::vec3& groundPos);

// Faces the current parameters produce, for the panel's footer.
int faceCount(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace housegen

// The parameters a house was generated from, kept on its root. Not in the Add
// Component menu -- it is made by the House panel -- but saved with the scene
// and with a prefab, so "Load selected" in the panel can re-open any house that
// was ever placed and rebuild it with different rooms.
class HouseComponent : public ComponentBase {
public:
    housegen::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<HouseComponent>(*this);
    }
    const char* typeId() const override { return "house"; }
    const char* displayName() const override { return "House"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none; // edited in its panel
    }
    void save(nlohmann::json& j) const override { housegen::saveParams(j, params); }
    void load(const nlohmann::json& j) override { housegen::loadParams(j, params); }
};
