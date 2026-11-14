#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "QrEncode.hpp"
#include "SceneTypes.hpp"

// QR codes as objects: a text in, a plate with the code standing up on it out
// -- a sign for a wall in a level, or a print for a 3D printer (File > Export
// Selection as 3MF takes it as it is: two parts, two colours, both closed).
//
// The plate is a slab as wide as the code, its quiet zone and the frame; the
// dark modules stand on it, every run of them ONE closed solid with no wall
// between two neighbours, so a slicer reads it watertight. Where two dark
// modules meet only at a corner their corners are drawn in by a hair (2 % of a
// module) -- welded there, four walls would share one edge and the solid would
// not be a solid. How thick the plate is and how far the code stands up are the
// two numbers a print is tuned by. The plate can be left out -- no background,
// the modules alone (a sign on a wall that is its own background, an inlay, a
// second colour printed apart) -- and a frame can run round the quiet zone, a
// rim in the code's colour and height. The frame is cells of the same grid as
// the modules (its own width at the edge), so with no quiet zone it and the
// code still merge into one solid.
//
// Lying flat, read from above: the code's top edge towards -Z, its left towards
// -X, the plate's bottom on the ground at the root -- so it prints face up and
// is not mirrored on the bed.
//
// Two ways in, one generator: the "QR codes" panel (QrCodePanel) builds one to
// order, and the Add menu puts a default one down to be edited there. The root
// keeps the parameters (QrCodeComponent) so a placed code can be re-opened and
// rebuilt. Pure data -> data: no GPU, no ImGui.
namespace qrcode {

struct Params {
    std::string   text   = "https://pueski.de";   // UTF-8
    qrencode::Ecc ecc    = qrencode::Ecc::Medium;
    float size   = 0.06f;    // the outer side: frame and quiet zone included (m)
    int   border = 4;        // quiet zone, in modules (the standard asks for 4)
    bool  background = true; // a plate under the code
    float plate  = 0.002f;   // its thickness (m)
    float relief = 0.001f;   // how far the dark modules (and the frame) stand up (m)
    bool  frame  = false;    // a rim round the quiet zone, in the code's colour
    float frameWidth = 0.002f;   // (m)
    glm::vec3 plateColor{0.93f, 0.93f, 0.91f};
    glm::vec3 inkColor{0.04f, 0.04f, 0.05f};

    bool operator==(const Params&) const = default;
};

// The parameters as they are built: sizes kept to what makes a solid.
Params sane(const Params& p);

// How far a corner shared only diagonally is drawn in, as a share of a module.
constexpr float kPinch = 0.02f;

// Where everything lies across the square, from its outer edge in: the frame,
// the quiet zone, the code. Metres.
struct Layout {
    float width = 0.0f;   // the outer side
    float frame = 0.0f;   // the frame's width, 0 without one
    float pitch = 0.0f;   // one module
    float inset = 0.0f;   // outer edge to the code's first module
    float plate = 0.0f;   // plate thickness, 0 without one
    float code  = 0.0f;   // the code's own side
};
Layout layoutOf(const Params& p, int codeSize);   // `p` as sane() leaves it

struct Model {
    qrencode::Code code;    // invalid when the text does not fit
    Layout   layout;
    EditMesh plate;         // the slab, empty without one
    EditMesh modules;       // the dark modules and the frame, faces wound outward
};

// Encode and build, in the code's own frame (see above). A text too long for
// version 40 gives no code and empty meshes.
Model build(const Params& p);

// The dark modules of `code` alone as closed solids, `pitch` a module, from
// height y0 to y1, centred on the origin (row 0 towards -Z) -- what a card
// (CardGen.hpp) puts its code down with.
EditMesh moduleSolids(const qrencode::Code& code, float pitch, float y0, float y1);

// The materials ("QR Code #EDEDE8" ...), named by their colour so two codes in
// the same colours share them.
struct Palette {
    fitzel::AssetId plate, ink;
};
Palette ensurePalette(std::vector<MaterialDef>& materials, const Params& p);

// The code as an entity subtree standing at `at`: an Empty root carrying the
// parameters, a "Plate" child (static collider) and a "Code" child. The shape
// AddEntitiesCmd wants; front() is the root. Empty when the text does not fit.
std::vector<Entity> entities(const Params& p, const Palette& pal, int& entityCounter,
                             const glm::vec3& at);

// The name a new code gets: "QR " and the start of its text.
std::string autoName(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace qrcode

// The parameters a code was built from, on its root -- so the panel can re-open
// any code in the scene and rebuild it. Made by the panel and the Add menu,
// saved with the scene and with a prefab.
class QrCodeComponent : public ComponentBase {
public:
    qrcode::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<QrCodeComponent>(*this);
    }
    const char* typeId() const override { return "qrCode"; }
    const char* displayName() const override { return "QR code"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;   // edited in its panel
    }
    void save(nlohmann::json& j) const override { qrcode::saveParams(j, params); }
    void load(const nlohmann::json& j) override { qrcode::loadParams(j, params); }
};
