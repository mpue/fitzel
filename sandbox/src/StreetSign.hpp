#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>   // fitzel::MeshData (CPU-side, no GPU)

#include "Component.hpp"
#include "SceneTypes.hpp"

// Street-name signs: a name, a style, and out comes the sign -- the blade, its
// lettering, the frame, the post it hangs on.
//
// The lettering is GEOMETRY, cut from real glyph outlines -- DIN 1451
// Mittelschrift, the typeface of German street signs, baked into SignFont.inc by
// tools/signfontgen.cpp -- as triangles lying a hair proud of the plate. No texture per sign, no atlas, no font file at runtime: every
// sign can say something different, stays sharp at any distance, and costs
// nothing to ship. A town's hundred crossings are a hundred different names.
//
// A STYLE is how a town letters its streets -- white with a black rule in
// Berlin, blue with two dots in Munich, black in Duesseldorf, the enamelled
// navy of an old town. The presets are those; every part of one can be changed.
//
// Two ways in, one generator:
//   - the town (CityPlan) puts a post on a corner of every crossing, two blades
//     naming the two streets, in the town's style;
//   - the "Street signs" panel (StreetSignPanel) builds one to order and places
//     it as an ordinary entity, whose root keeps the parameters so it can be
//     re-opened and rebuilt (StreetSignComponent).
//
// Pure data -> data: no GPU, no scene, like the other generators.
namespace streetsign {

// --- The look -------------------------------------------------------------------

// The rule around the name. Line: square corners. Rounded: corners bowed out.
// Notched: corners cut IN by a quarter circle -- the ornate enamel sign.
enum class Frame { None, Line, Rounded, Notched, Count };
const char* frameName(Frame f);

struct Style {
    glm::vec3 plate{0.95f, 0.95f, 0.93f};
    glm::vec3 ink{0.06f, 0.06f, 0.07f};    // lettering, frame and dots
    Frame     frame    = Frame::Line;
    bool      dots     = false;   // a dot before and after the name (Munich)
    bool      capitals = false;   // the name in capitals (older signs; ss for sharp s)
    float     condense = 1.0f;    // letter width, 1 = the typeface's own
    float     corner   = 0.0f;    // plate corner radius, in letter heights
};

// The built-in looks. Index into this list is what a town stores.
struct Preset { const char* name; Style style; };
const std::vector<Preset>& presets();
Style presetStyle(int index);   // clamped; out of range -> the first

// --- The sign -------------------------------------------------------------------

// Post: one or two blades on top of a post (a crossing). Plate: the blade alone,
// lettered on its front only -- for a wall, a fence, a gate.
enum class Mount { Post, Plate, Count };
const char* mountName(Mount m);

struct Params {
    std::string textA = "Fitzelstra\xC3\x9F" "e";   // UTF-8
    std::string textB;             // second blade (Post only); empty = none
    int       preset     = 0;      // which preset `style` started from (the panel's)
    Style     style      = presetStyle(0);
    float     letterHeight = 0.085f;   // capital height, metres
    Mount     mount      = Mount::Post;
    float     postHeight = 2.9f;   // ground to the top of the post, metres
    float     angleB     = 90.0f;  // second blade's turn from the first, degrees
};

// --- The flat layout (shared by the 3D model and the panel's preview) -----------

// A convex polygon on the plate's face. Metres, origin at the plate centre,
// x to the reader's right, y up, counter-clockwise.
struct Poly {
    std::vector<glm::vec2> pts;
};
struct Face {
    float width = 0.0f, height = 0.0f;   // the plate
    std::vector<glm::vec2> outline;      // the plate's own outline (convex, CCW)
    std::vector<Poly>      ink;          // lettering + frame + dots
};
// Lay `utf8` out on a plate in `style` with capitals `letterHeight` tall.
Face layout(const std::string& utf8, const Style& style, float letterHeight);

// The text as the sign letters it: capitals applied (sharp s -> SS), unknown
// characters dropped. What the lettering says, for tests and the panel.
std::u32string lettered(const std::string& utf8, bool capitals);

// Whether `c` is lettered with the (heavier) trapezoid cut because the ear
// clipper could not fill it exactly -- glyphs whose contours overlap (the
// cedilla of a C, the slash through an O) and want a union first. For tests.
bool glyphFellBack(char32_t c);

// --- Materials and the model ----------------------------------------------------

// The materials a sign wears. Colours are materials named by their value
// ("Street Sign #1E4F8C", the lettering "Street Sign Ink #F0F0EC"), so two
// styles sharing a blue share one material, and changing a colour never
// repaints a sign built in another. The lettering has materials of its own so
// a town can keep it out of the shadow passes without touching any plate.
struct Palette {
    fitzel::AssetId plate, ink, post, cap;
};
Palette ensurePalette(std::vector<MaterialDef>& materials, const Style& style);

// One flat polygon of the model, in the sign's own frame: y up, origin on the
// ground under the post (Post) or at the plate centre (Plate), blade A along +x,
// its front (the side read left to right) facing +z.
struct Poly3 {
    std::vector<glm::vec3> pts;   // convex, counter-clockwise seen from outside
    glm::vec3              normal{0.0f};
    fitzel::AssetId        material;
    bool                   post = false;   // part of the post (the solid bit)
};
std::vector<Poly3> build(const Params& p, const Palette& pal);

// The same, welded per material into render meshes (the town's path).
std::vector<std::pair<fitzel::AssetId, fitzel::MeshData>> meshes(const std::vector<Poly3>& polys);

// The same as an entity subtree -- an Empty root carrying the parameters, with
// a "Post" child (collider) and a "Sign" child -- standing at `at`. The shape
// AddEntitiesCmd wants; front() is the root.
std::vector<Entity> entities(const Params& p, const Palette& pal, int& entityCounter,
                             const glm::vec3& at);

// --- Names --------------------------------------------------------------------

// Real street names to put on signs: the street directory of Frankfurt am Main
// (3167 names, parks and bridges left out), sorted. For the town's streets and
// the panel's "pick a name".
const std::vector<std::string>& frankfurtNames();

void saveParams(nlohmann::json& j, const Params& p);
void loadParams(const nlohmann::json& j, Params& p);

} // namespace streetsign

// The parameters a sign was built from, on its root -- so the panel can re-open
// any sign in the scene and rebuild it. Made by the panel, not the Add Component
// menu, but saved with the scene and with a prefab.
class StreetSignComponent : public ComponentBase {
public:
    streetsign::Params params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<StreetSignComponent>(*this);
    }
    const char* typeId() const override { return "streetSign"; }
    const char* displayName() const override { return "Street sign"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;   // edited in its panel
    }
    void save(nlohmann::json& j) const override { streetsign::saveParams(j, params); }
    void load(const nlohmann::json& j) override { streetsign::loadParams(j, params); }
};
