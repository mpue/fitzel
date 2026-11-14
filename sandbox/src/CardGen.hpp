#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "Lettering.hpp"
#include "QrEncode.hpp"
#include "SceneTypes.hpp"

// Cards: a plate with rounded corners and something written on it -- a business
// card, a name badge, a gift tag, the Wi-Fi card for the guests -- the lettering
// and a QR code standing up from it. File > Export Selection as 3MF takes it as
// it is (two parts, two colours, every solid closed); in a scene it is a sign.
//
// The text is set in any font the machine has (Lettering.hpp), a first line
// larger if wanted. The text and the code each sit at one of nine anchors on
// the area inside the margin, moved from there by an offset -- so a card keeps
// its look when the text changes, and the panel can let them be dragged and
// snap them back. "Fit" shrinks the text until it stays clear of the code, the
// keyring hole and the margin. A frame narrows that area by its own width, and
// a hole moves inside the frame: switching either on never runs one thing into
// another.
//
// Millimetres throughout (a card is a thing for the hand), turned into metres
// on the way into the scene. Lying flat and read from above like a QR code
// (QrCode.hpp): its top towards -Z, the plate's bottom on the ground at the
// root. Pure data -> data: no GPU, no ImGui.
namespace card {

// Where a thing sits: one of nine anchors on the area inside the margin, row
// by row from the top left (0 1 2 / 3 4 5 / 6 7 8), moved from there.
struct Place {
    int   anchor = 3;
    float dx = 0.0f, dy = 0.0f;   // mm, right and down

    bool operator==(const Place&) const = default;
};

struct Params {
    // --- The card ----------------------------------------------------------------
    float width = 85.0f, height = 55.0f;   // mm
    float thickness = 1.2f;                // the plate (mm)
    float corner    = 3.0f;                // corner radius (mm)
    float margin    = 4.0f;                // edge (or frame) to what is printed (mm)
    float relief    = 0.6f;                // how far text, code and frame stand up (mm)

    // --- The text ----------------------------------------------------------------
    std::string text = "Max Mustermann\nGrafik & 3D-Druck\nmax@beispiel.de\n0151 2345678";
    std::string font;                      // a font file; empty = defaultFontPath()
    float capHeight = 3.0f;                // a capital's height (mm)
    bool  headline  = true;                // the first line larger ...
    float headlineScale = 1.3f;            // ... by this much
    float spacing   = 1.15f;               // line distance, times the font's own
    lettering::Align align = lettering::Align::Left;
    bool  fit = true;                      // shrink the text to keep it clear
    Place textAt{3, 0.0f, 0.0f};

    // --- The QR code ---------------------------------------------------------------
    bool qr = true;
    std::string qrText = "https://pueski.de";
    qrencode::Ecc ecc = qrencode::Ecc::Medium;
    float qrSize = 26.0f;                  // the modules' square (mm), quiet zone not counted
    Place qrAt{5, 0.0f, 0.0f};

    // --- Frame and hole --------------------------------------------------------------
    bool  frame = false;
    float frameWidth = 0.8f;               // mm
    float frameInset = 1.5f;               // card edge to the frame (mm)
    bool  hole = false;                    // for a key ring
    float holeDiameter = 5.0f;             // mm
    int   holeCorner = 0;                  // 0 top left, 1 top right, 2 bottom left, 3 bottom right

    glm::vec3 cardColor{0.93f, 0.93f, 0.91f};
    glm::vec3 inkColor{0.04f, 0.04f, 0.05f};

    bool operator==(const Params&) const = default;
};

// The parameters as they are built: sizes kept to what makes a card.
Params sane(const Params& p);

// Card sizes to start from.
struct Preset { const char* name; float width, height, corner; };
const std::vector<Preset>& presets();

// A rectangle on the card: mm, y up, origin at the card's middle.
struct Box {
    glm::dvec2 lo{0.0}, hi{0.0};
    glm::dvec2 size() const { return hi - lo; }
    glm::dvec2 centre() const { return 0.5 * (lo + hi); }
    bool overlaps(const Box& o, double gap = 0.0) const {
        return lo.x < o.hi.x + gap && o.lo.x < hi.x + gap && lo.y < o.hi.y + gap && o.lo.y < hi.y + gap;
    }
    bool inside(const Box& o) const {
        return lo.x >= o.lo.x - 1e-6 && lo.y >= o.lo.y - 1e-6 && hi.x <= o.hi.x + 1e-6 && hi.y <= o.hi.y + 1e-6;
    }
};

// Where a thing of `size` lands at `at` on `area`.
Box place(const Box& area, glm::dvec2 size, const Place& at);
// The anchor and offset that put a thing of `size` with its lower-left corner
// at `lo` -- from the nearest anchor, onto it when within `snap` mm. For
// dragging.
Place placeAt(const Box& area, glm::dvec2 size, glm::dvec2 lo, double snap);

struct Layout {
    Box card;                  // the whole card
    Box area;                  // inside the margin (and the frame)
    lettering::Block text;     // at the size used, its own frame (see Lettering.hpp)
    double textScale = 1.0;    // what Fit left of capHeight (1: as set)
    Box textBox;               // where the block's box lies
    qrencode::Code code;       // invalid without a code, or when its text is too long
    Box qrBox;
    double qrPitch = 0.0;      // one module (mm)
    glm::dvec2 holeAt{0.0};
    double holeRadius = 0.0;   // 0 without a hole
    std::string fontPath;      // the font used ("" when none could be read)
    std::vector<std::string> warnings;
};
Layout layoutOf(const Params& p);   // `p` as sane() leaves it

// The plate's outline (and the hole's), the frame's outer and inner outline:
// mm, card frame, counter-clockwise.
std::vector<glm::dvec2> roundedRect(const Box& b, double radius);
std::vector<glm::dvec2> circle(glm::dvec2 c, double r);

struct Model {
    Layout   layout;
    EditMesh plate;    // the card
    EditMesh relief;   // text, code and frame, standing on it
};
Model build(const Params& p);

// The materials ("Card #EDEDE8", "Card Ink #0A0A0D"), named by their colour.
struct Palette {
    fitzel::AssetId card, ink;
};
Palette ensurePalette(std::vector<MaterialDef>& materials, const Params& p);

// The card as an entity subtree standing at `at`: an Empty root carrying the
// parameters, a "Card" child (static collider) and a "Relief" child.
std::vector<Entity> entities(const Params& p, const Palette& pal, int& entityCounter, const glm::vec3& at);

// The name a new card gets: "Card " and the start of its first line.
std::string autoName(const Params& p);

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace card

// The parameters a card was built from, on its root -- so the panel can re-open
// any card in the scene and rebuild it.
class CardComponent : public ComponentBase {
public:
    card::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<CardComponent>(*this);
    }
    const char* typeId() const override { return "card"; }
    const char* displayName() const override { return "Card"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;   // edited in its panel
    }
    void save(nlohmann::json& j) const override { card::saveParams(j, params); }
    void load(const nlohmann::json& j) override { card::loadParams(j, params); }
};
