#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "SceneTypes.hpp"

// A pen holder to print that takes as little plastic as it can and nothing but
// pens:
//
//   a honeycomb   one hexagonal cell a pen, the walls between two cells shared
//                 and as thin as two lines of the nozzle -- the least wall that
//                 stands up round a set of pens, no infill anywhere
//   pen-sized     a cell takes one pen and nothing much wider: the cell is kept
//                 to 14 mm across its flats, so the widest thing that goes in
//                 at all (corner to corner) is narrower than the smallest euro
//                 coin, even on edge. No bowl, no compartment, no shelf: the top
//                 is the walls' edges and nothing else
//   no floor      each cell ends in a funnel that leads into a small drain, so
//                 crumbs, pencil shavings and what else is that small fall
//                 through onto the desk; a pen's tip stands in the drain's
//                 throat, clear of the desk. The funnels are shells: under them
//                 the holder is hollow, open to the desk, so the bottom is no
//                 solid block either
//
// Round (rings of cells round one) or in rows (staggered, as honeycombs are),
// on a thin foot to stand steady, or on the walls alone.
//
// One closed solid made by SolidBuild (outlines carried along an axis, no
// booleans), so File > Export Selection as 3MF hands a slicer a clean part.
// Built in the print's frame (mm, Z up, the front towards -Y) and handed over
// in metres with Y up: x, z, -y. Pure data -> data: no GPU, no ImGui.
namespace penholder {

// Saved as indices: append only.
enum class Layout { Round, Rows, Count };
enum class SlopeTo { Rim, Front, Count };   // where the cells' mouths fall towards

const char* layoutName(Layout l);   // "Round", "Rows"
const char* slopeName(SlopeTo s);   // "To the rim", "To the front"

// The widest a cell may be across its flats (mm): its corners then lie 16.17 mm
// apart, and the smallest euro coin (16.25 mm) does not go in even on edge.
constexpr float kMaxCell = 14.0f;

struct Params {
    Layout layout = Layout::Round;
    int   rings   = 1;          // round: rings of cells round the middle one (0: one cell, 1: 7, 2: 19)
    int   columns = 4, rows = 2;   // in rows

    float pen     = 11.0f;      // the thickest pen it takes (mm across)
    float play    = 1.0f;       // added to that: the cell across its flats
    float height  = 50.0f;      // the walls' top above the desk, where it is highest
    // The mouths falling away from the middle (a cone) or towards the front (a
    // plane, the back highest) by this many degrees: the outer pens stand out
    // further and come to hand. 0: level.
    float slope   = 0.0f;
    SlopeTo slopeTo = SlopeTo::Rim;
    float wall    = 0.9f;       // every wall: two lines of a 0.45 mm extrusion
    float drain   = 3.0f;       // the hole at the bottom of each cell
    float tipRoom = 9.0f;       // the drain's throat: a pen's or a pencil's point stands in it, clear of the desk
    float foot    = 4.0f;       // how far the foot reaches beyond the cells (0: none)
    float footHeight = 1.2f;

    glm::vec3 color{0.20f, 0.42f, 0.62f};

    bool operator==(const Params&) const = default;
};

// The shortest throat under a drain `drain` wide: a pencil's point standing
// in it stays a millimetre above the desk.
float minThroat(float drain);

// The parameters as they are built: the cell kept to kMaxCell, the drain to
// what the funnel and its wall leave room for, its throat long enough for a
// pencil's point, the height above the funnels, the slope to what leaves the
// shallowest cell 10 mm deep above its funnel.
Params sane(const Params& p);

// The cells' middles (mm, the print's plan), the holder's middle at the origin.
std::vector<glm::dvec2> cellCentres(const Params& p);

// A drawing of the part from one side, for the panel: outlines in mm, the
// first `solids` plastic, the rest holes.
struct View {
    std::string name;
    std::vector<std::vector<glm::vec2>> loops;
    int solids = 1;
};

struct Model {
    EditMesh body;              // metres, Y up: centred on X and Z, its bottom on Y 0
    glm::vec3 size{0.0f};       // mm: width (X), depth (Y), height
    double volume = 0.0;        // plastic (mm3)
    double grams  = 0.0;        // in PLA
    double metres = 0.0;        // of 1.75 mm filament
    int    cells  = 0;
    std::vector<glm::dvec2> centres;   // the cells' middles in the body's plan (mm, the print's frame)
    double cell   = 0.0;        // a cell across its flats (mm)
    double widest = 0.0;        // the widest thing that goes into a cell: corner to corner
    double lowestTop  = 0.0;    // the walls' top where it is lowest (mm above the desk)
    double shallowest = 0.0;    // the shallowest cell, from its mouth's lowest point to its funnel
    bool   ok = true;           // false: a face could not be filled (see `error`)
    std::string error;
    std::string printHint;
    std::vector<View> views;
};

Model build(const Params& p);

fitzel::AssetId ensureMaterial(std::vector<MaterialDef>& materials, const Params& p);

// The holder as an entity subtree standing at `at` (its bottom there): an Empty
// root in its middle carrying the parameters and a "Holder" child (a static
// collider). front() is the root.
std::vector<Entity> entities(const Params& p, const fitzel::AssetId& material, int& entityCounter,
                             const glm::vec3& at);

// "Pen holder 7 x 11 mm"
std::string autoName(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace penholder

// The parameters a pen holder was built from, on its root -- so the panel can
// re-open it and rebuild it. Saved with the scene and with a prefab.
class PenHolderComponent : public ComponentBase {
public:
    penholder::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<PenHolderComponent>(*this);
    }
    const char* typeId() const override { return "penHolder"; }
    const char* displayName() const override { return "Pen holder"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;   // edited in its panel
    }
    void save(nlohmann::json& j) const override { penholder::saveParams(j, params); }
    void load(const nlohmann::json& j) override { penholder::loadParams(j, params); }
};
