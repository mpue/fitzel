#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "SceneTypes.hpp"

// Cable holders to print, five kinds from one set of parameters:
//
//   comb      a bar with a row of seats the cables snap into -- free-standing,
//             on an adhesive pad (a recess for it underneath), screwed down
//             (countersunk flanges) or on magnets
//   desk clip the same bar on a clamp that hooks over a desk's edge, its lip
//             leaning up a little so it grips
//   tray      a channel screwed under the desk, the front wall lower so cables
//             go in from the front, round outlets in the floor
//   hook      for coiled cables and headphones, screwed to a wall or under
//             the desk
//   winder    a flat bone to wind an earphone or charging cable round, a slit
//             at each end for the cable's ends
//
// The seats are keyholes: a round seat a little wider than the cable and a neck
// narrower than it (a share of the seat, 1 makes it an open U), so a cable is
// pressed in and stays. Every seat has its own diameter, and its own label,
// raised on the front in a second colour (a second part: two filaments, or a
// colour change). The edges a cable touches and the top edges are rounded front
// and back -- the outline drawn in step by step towards the faces -- and so are
// a winder's.
//
// Every part is one or two closed solids made by SolidBuild (outlines carried
// along an axis, holes bored, no booleans), so File > Export Selection as 3MF
// hands a slicer clean parts. Built in the print's frame (mm, Z up, the front
// towards -Y) and handed over in metres with Y up: x, z, -y.
// Pure data -> data: no GPU, no ImGui.
namespace cableholder {

// Saved as indices: append only.
enum class Style { Comb, DeskClip, Tray, Hook, Winder, Count };
enum class Mount { Free, Adhesive, Screws, Magnets, Count };   // how a comb holds on
enum class HookMount { Wall, UnderDesk, Count };

const char* styleName(Style s);   // "Cable comb", ...
const char* mountName(Mount m);

struct Params {
    Style style = Style::Comb;

    // --- Seats: a comb's and a desk clip's -------------------------------------------
    int   slots     = 4;
    std::string diameters = "5";   // per seat, left to right, comma separated; the last one repeats (mm)
    float clearance = 0.3f;        // added to every diameter
    float neck      = 0.75f;       // the opening as a share of the seat's width; 1: an open U
    float tooth     = 4.0f;        // plastic between two seats
    float margin    = 5.0f;        // beyond the outer seats
    float depth     = 15.0f;       // front to back
    float base      = 3.0f;        // plastic under the seats
    float rounding  = 1.2f;        // the edges round the seats and along the top
    std::string labels;            // per seat, comma separated, raised on the front
    float labelHeight = 3.0f;      // a capital's height (mm)
    Mount mount = Mount::Free;

    // --- A desk clip ------------------------------------------------------------------
    float deskThickness = 25.0f;
    float grip     = 0.8f;         // how far the lip under the desk leans up at its tip
    float reach    = 18.0f;        // how far the lip reaches under the desk
    float clipWall = 4.0f;         // the clamp's plates

    // --- A tray under the desk --------------------------------------------------------
    float trayLength = 200.0f, trayWidth = 40.0f, trayHeight = 35.0f, trayWall = 2.5f;
    bool  trayOpenFront = true;    // a lower front wall: cables go in from the front
    bool  trayOutlets   = true;    // round holes in the floor for cables to drop through
    float outletDiameter = 22.0f, outletSpacing = 60.0f;

    // --- A hook -------------------------------------------------------------------------
    HookMount hookMount = HookMount::Wall;
    float hookWidth = 20.0f, hookThickness = 5.0f;
    float hookPlate  = 45.0f;      // the plate against the wall or the desk
    float hookReach  = 35.0f;      // the arm, straight
    float hookRadius = 10.0f;      // inside the bend at its end
    float hookTip    = 12.0f;      // the tip after the bend, straight

    // --- A winder -----------------------------------------------------------------------
    float winderLength = 70.0f, winderWidth = 32.0f, winderThickness = 3.5f;
    float notchWidth = 14.0f, notchDepth = 11.0f;   // the ends the cable is wound through
    float slit = 1.6f;             // the slits a cable's ends are pressed into
    std::string winderLabel;

    // --- Screws and magnets, wherever they are used -------------------------------------
    float screwShank = 3.5f, screwHead = 7.0f;   // countersunk
    float magnetDiameter = 6.0f, magnetHeight = 2.0f;

    glm::vec3 color{0.16f, 0.17f, 0.19f};
    glm::vec3 labelColor{0.96f, 0.74f, 0.22f};

    bool operator==(const Params&) const = default;
};

// The parameters as they are built: sizes kept to what makes a solid (the base
// under labels and magnets, the roundings the teeth leave room for, ...).
Params sane(const Params& p);

// The seats' diameters (the cable's, before the clearance) and their labels,
// one each, read from the comma separated lists.
std::vector<float> diametersOf(const Params& p);
std::vector<std::string> labelsOf(const Params& p);

// A drawing of the part from one side, for the panel: outlines in mm, the
// first the outside.
struct View {
    std::string name;
    std::vector<std::vector<glm::vec2>> loops;
    int solids = 1;   // the first ones are plastic, the rest holes
};

struct Model {
    EditMesh body;              // metres, Y up: centred on X and Z, its bottom on Y 0
    EditMesh labels;            // the raised labels, empty without any
    glm::vec3 size{0.0f};       // mm: width (X), depth (Y), height, labels included
    double volume = 0.0;        // the body's plastic (mm3)
    bool   ok = true;           // false: a face could not be filled (see `error`)
    std::string error;
    std::string printHint;      // how it lies on the bed best
    int    missingGlyphs = 0;   // label characters the font does not have
    std::vector<View> views;
};

Model build(const Params& p);

struct Palette {
    fitzel::AssetId body, labels;
};
Palette ensurePalette(std::vector<MaterialDef>& materials, const Params& p);

// The part as an entity subtree standing at `at` (its bottom there): an Empty
// root in its middle carrying the parameters, a "Holder" child (a static
// collider) and, with labels, a "Labels" child. front() is the root.
std::vector<Entity> entities(const Params& p, const Palette& pal, int& entityCounter, const glm::vec3& at);

// "Cable comb 4 x 5 mm", "Desk cable clip 3 x 6 mm", "Cable tray 200 mm", ...
std::string autoName(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace cableholder

// The parameters a holder was built from, on its root -- so the panel can
// re-open it and rebuild it. Saved with the scene and with a prefab.
class CableHolderComponent : public ComponentBase {
public:
    cableholder::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<CableHolderComponent>(*this);
    }
    const char* typeId() const override { return "cableHolder"; }
    const char* displayName() const override { return "Cable holder"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;   // edited in its panel
    }
    void save(nlohmann::json& j) const override { cableholder::saveParams(j, params); }
    void load(const nlohmann::json& j) override { cableholder::loadParams(j, params); }
};
