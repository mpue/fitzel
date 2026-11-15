#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "SceneTypes.hpp"

// A winter scene as a stencil to hang in a window: one flat plate, the scene a
// silhouette, the sky and the lit windows cut out, so the light behind it
// draws the picture.
//
//   the frame     round, a rectangle or an arch, a loop at the top to hang it by
//   the snow      hills along the bottom, filling up to the frame
//   on the snow   firs of drooping tiers, a cabin with lit window panes, a round
//                 window in its gable and a lit door, smoke from its chimney in
//                 puffs; a snowman with a hat, arms of twigs, eyes and buttons
//                 cut out; a fence of pointed pickets
//   in the sky    the moon's crescent, stars and snowflakes, each hanging from
//                 the top of the frame on a thread -- a stencil is one piece, so
//                 nothing floats
//
// Drawn as a distance field: shapes joined, windows cut, everything held inside
// the frame, then the outlines traced (marching squares) and the plate made of
// them -- walls along every outline, the top and bottom filled between. One
// closed part, every face level or upright: it prints flat, without support.
//
// Built in the print's frame (mm, Z up; the picture's up is +Y) and handed over
// in metres with Y up: x, z, -y -- lying flat in the scene as on the bed. Pure
// data -> data: no GPU, no ImGui.
namespace winterstencil {

// Saved as indices: append only.
enum class Frame { Round, Rect, Arch, Count };

const char* frameName(Frame f);   // "Round", "Rectangle", "Arch"

struct Params {
    Frame frame   = Frame::Round;
    float width   = 150.0f;   // across (round: its diameter)
    float height  = 120.0f;   // a rectangle's or an arch's, top to bottom
    float border  = 3.0f;     // the frame's band
    float thickness = 1.6f;
    bool  eyelet  = true;     // a loop at the top to hang it by
    int   seed    = 1;        // how the hills run and where everything stands
    float hills   = 0.5f;     // 0: flat snow; 1: rolling hills
    int   trees   = 5;
    bool  house   = true;
    bool  smoke   = true;
    bool  snowman = true;
    bool  fence   = true;
    bool  moon    = true;
    int   stars   = 5;
    int   flakes  = 2;
    float line    = 1.2f;     // the threads, the window bars, the twigs: the thinnest it draws

    glm::vec3 color{0.95f, 0.95f, 0.97f};

    bool operator==(const Params&) const = default;
};

// The parameters as they are built: the frame to a bed, every line two lines
// of a 0.4 mm nozzle at the least, counts to what the sky has room for.
Params sane(const Params& p);

struct Model {
    EditMesh body;              // metres, Y up: centred on X and Z, its bottom on Y 0
    glm::vec3 size{0.0f};       // mm: width (X), height of the picture (Y), thickness
    double volume = 0.0;        // plastic (mm3)
    double grams  = 0.0;        // in PLA
    double metres = 0.0;        // of 1.75 mm filament
    int    pieces = 0;          // separate pieces of plastic: 1 for a stencil that holds together
    int    dropped = 0;         // specks that held on to nothing, left out (none, drawn as it should be)
    glm::vec2 droppedAt{0.0f};  // where the first was
    int    holes  = 0;          // openings: the sky, the panes, the eyelet ...
    double smallestHole = 0.0;  // the narrowest opening: the widest round that fits in it (mm across)
    glm::vec2 smallestHoleAt{0.0f};
    int    trees = 0, stars = 0, flakes = 0;   // as many as found room
    bool   ok = true;
    std::string error;
    std::string printHint;
    std::vector<glm::vec2> tris;   // the top, triangle by triangle, for the panel (mm, the picture's plan)
};

Model build(const Params& p);

fitzel::AssetId ensureMaterial(std::vector<MaterialDef>& materials, const Params& p);

// The stencil as an entity subtree lying at `at` (its bottom there): an Empty
// root in its middle carrying the parameters and a "Stencil" child (a static
// collider). front() is the root.
std::vector<Entity> entities(const Params& p, const fitzel::AssetId& material, int& entityCounter,
                             const glm::vec3& at);

// "Winter stencil round 150 mm"
std::string autoName(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace winterstencil

// The parameters a winter stencil was built from, on its root -- so the panel
// can re-open it and rebuild it. Saved with the scene and with a prefab.
class WinterStencilComponent : public ComponentBase {
public:
    winterstencil::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<WinterStencilComponent>(*this);
    }
    const char* typeId() const override { return "winterStencil"; }
    const char* displayName() const override { return "Winter stencil"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;   // edited in its panel
    }
    void save(nlohmann::json& j) const override { winterstencil::saveParams(j, params); }
    void load(const nlohmann::json& j) override { winterstencil::loadParams(j, params); }
};
