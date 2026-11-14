#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "SceneTypes.hpp"

// Gridfinity: the open storage system on a 42 mm grid (Zack Freedman's) --
// bins that drop into a baseplate in a drawer and stack on each other. Bins and
// baseplates made to its numbers, as objects to print: File > Export Selection
// as 3MF hands a slicer one closed part.
//
// The numbers (the community's reference, kennetek/gridfinity-rebuilt-openscad,
// src/core/standard.scad and bin.scad):
//   grid 42 mm; a bin 0.5 mm smaller a unit (41.5 for one), corners r 3.75;
//   its feet, from the bottom: 45 deg out 0.8, straight up 1.8, 45 deg out
//   2.15 -- 4.75 high, 35.6 wide at the bottom -- joined above by a slab up to
//   7 mm, where the compartments' floor is; height units of 7 mm, the base
//   included, the stacking lip not. The lip stands on the walls' top: its inner
//   face, from there up, 45 deg out 0.7, straight 1.8, 45 deg out 1.9 to the
//   outer wall, the point rounded r 0.6 (so 3.55 high, not 4.4); under it a
//   1.2 mm straight face and a 45 deg slope back out to the wall. Compartments
//   and dividers end at that 1.2 mm face's bottom (without a lip: at the top).
//   Magnets 6.5 x 2.4 holes, screws M3 (3 mm) 6 deep, 13 mm out from the middle
//   of a unit both ways; walls 0.95, dividers 1.2; the label shelf 15.85 deep
//   from the back wall, its underside at 36 deg below a 1.2 mm edge.
//   A baseplate cell: 0.35 straight, 45 deg out 0.7, straight 1.8, 45 deg out
//   2.15 -- 5 high, 36.3 open at the bottom, 42 at the top, where neighbouring
//   cells meet at a knife edge; outer corners r 4.
// A bin set in a plate rests on the slopes of its feet, which lie on the cell's
// slopes just as its bottom reaches the drawer's; between them the straight
// parts keep 0.25 mm apart. Set on another bin it rests the same way on the
// lip, 0.35 mm below the walls' top. gridfinitycheck measures both from the
// meshes.
//
// Every surface is built from rounded rectangles (and circles) stacked along a
// profile and lofted, so the bin is one closed solid by construction: no
// boolean operations, every edge between exactly two faces. Where a flat face
// meets several of those outlines -- the underside between the feet, the tops
// of the dividers -- the outlines share their corners and edges (they are cut
// at each other's corners), the shared edges cancel, and what is left is filled.
//
// Built in the print's own frame (millimetres, Z up, the front -- the scoop, the
// side a label is read from -- towards -Y) and handed over in metres with Y up:
// x, z, -y. Pure data -> data: no GPU, no ImGui.
namespace gridfinity {

enum class Kind { Bin, Baseplate };

struct Params {
    Kind kind = Kind::Bin;

    // --- A bin ---------------------------------------------------------------
    int  unitsX = 2, unitsY = 1;   // grid units (42 mm) across and front to back
    int  unitsZ = 6;               // height units (7 mm), the base included, the lip not
    int  compX  = 1, compY = 1;    // compartments across and front to back
    bool lip     = true;           // the stacking lip
    bool magnets = false;          // holes for 6 x 2 mm magnets under the feet
    bool screws  = false;          // M3 holes under the feet (above the magnets with both)
    bool  label      = false;      // a label shelf along each compartment's back wall
    float labelDepth = 15.85f;     // how far it reaches in from the back wall (mm)
    bool  scoop       = false;     // each compartment's front rounded into its floor
    float scoopRadius = 15.0f;     // (mm)
    float wall    = 0.95f;         // outer wall (mm)
    float divider = 1.2f;          // between compartments (mm)
    float floor   = 7.0f;          // the compartments' floor above the feet's bottom (mm)
    glm::vec3 binColor{0.93f, 0.45f, 0.13f};

    // --- A baseplate ------------------------------------------------------------
    int  plateX = 4, plateY = 3;    // cells across and front to back
    bool  plateFloor     = false;   // a solid floor (else an open frame on the drawer's bottom)
    float floorThickness = 3.2f;    // under the cells (mm)
    bool  plateMagnets = false;     // magnet holes in the floor, under a bin's
    bool  plateScrews  = false;     // a countersunk hole in each cell to screw it down
    bool  fitDrawer = false;        // as many cells as fit, a solid margin round them
    float drawerW = 0.0f, drawerD = 0.0f;   // the drawer's inside (mm)
    glm::vec3 plateColor{0.42f, 0.44f, 0.47f};

    bool operator==(const Params&) const = default;
};

// The standard's numbers the generator and its check share (mm).
constexpr double kPitch     = 42.0;
constexpr double kBinRadius = 3.75;
constexpr double kFootH     = 4.75;
constexpr double kHeightU   = 7.0;
constexpr double kLipH      = 4.4;
constexpr int    kArcSegments = 6;   // a rounded corner drawn in this many

// The parameters as they are built: counts and sizes kept to what makes a
// solid -- the floor above the holes, compartments no narrower than their
// rounded corners, the cells a drawer takes.
Params sane(const Params& p);

// The most compartments `units` grid units take side by side.
int maxCompartments(int units, float wall, float divider);

// How high the floor may go and still leave room inside, and how low it may go
// above the feet and holes (mm).
float minFloor(const Params& p);

struct Model {
    EditMesh mesh;              // metres, Y up: centred on X and Z, its bottom on Y 0
    glm::vec3 size{0.0f};       // mm: width (X), depth (Y), height, the lip included
    double volume   = 0.0;      // the plastic (mm3)
    double capacity = 0.0;      // inside one compartment, to the top of the walls (mm3)
    int    compartments = 0;    // 0: solid
    float  lipTop = 0.0f;       // a bin: the lip's top above the walls' (mm)
    bool   ok = true;           // false: a face could not be filled (see `error`)
    std::string error;
    // The outlines seen from above (mm, print frame): the outside first, then
    // the compartments' tops and floors (a bin) or the cells' tops and bottoms
    // (a plate) -- what the panel draws.
    std::vector<std::vector<glm::vec2>> plan;
    std::vector<std::vector<glm::vec2>> holes;   // a plate's, as seen from above
};

// Build `p` (as sane() leaves it).
Model build(const Params& p);

// `mesh` (a Model's) cut by the plane x = `x` (mm, the print's frame): its
// outlines as (y, z) loops in mm -- the panel's cut-away view. Every face that
// crosses the plane gives one piece, and the pieces are joined end to end.
std::vector<std::vector<glm::vec2>> section(const EditMesh& mesh, float x);

// The material ("Gridfinity #F0731F"), named by its colour.
fitzel::AssetId ensureMaterial(std::vector<MaterialDef>& materials, const Params& p);

// The object as an entity subtree standing at `at` (its bottom there): an
// Empty root in its middle carrying the parameters, and one "Bin" or
// "Baseplate" child (a static collider). The shape AddEntitiesCmd wants;
// front() is the root.
std::vector<Entity> entities(const Params& p, const fitzel::AssetId& material, int& entityCounter,
                             const glm::vec3& at);

// "Gridfinity bin 2x1x6", "Gridfinity baseplate 4x3".
std::string autoName(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace gridfinity

// The parameters a bin or plate was built from, on its root -- so the panel can
// re-open it and rebuild it. Saved with the scene and with a prefab.
class GridfinityComponent : public ComponentBase {
public:
    gridfinity::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<GridfinityComponent>(*this);
    }
    const char* typeId() const override { return "gridfinity"; }
    const char* displayName() const override { return "Gridfinity"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;   // edited in its panel
    }
    void save(nlohmann::json& j) const override { gridfinity::saveParams(j, params); }
    void load(const nlohmann::json& j) override { gridfinity::loadParams(j, params); }
};
