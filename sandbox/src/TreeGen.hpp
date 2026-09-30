#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

// The tree generator: a whole tree -- trunk, limbs, twigs, leaves -- from a
// handful of botanical numbers, as one mesh in two parts (bark and leaf cards).
//
// Pure CPU and deterministic: the same Params give the same tree to the last
// bit, so a saved .tree.json IS the tree, and the preview, the saved .glb and
// the check harness can never disagree. No GL here; TreeGlb.hpp writes the
// result out, TreePreview.hpp draws it, TreeGenPanel.hpp is the editor window.
//
// How a tree is grown (Weber & Penn's "Creation and Rendering of Realistic
// Trees", 1995, reworked in a few places where their model reads as synthetic):
//   1. The SKELETON, top-down. The trunk (level 0) is a polyline that bends by
//      its curve, wiggles by smooth noise (not the per-segment random walk that
//      gives the classic generator its zig-zags), leans and is pulled by
//      gravity or light. It may FORK -- the way an oak's trunk dissolves into
//      limbs -- and every piece carries children of the next level, placed by
//      phyllotaxis (spiral, opposite or whorled), angled away from the parent,
//      and as long as the crown shape allows at that height.
//   2. The THICKNESS, bottom-up (the pipe model, da Vinci's rule): every twig
//      tip has the same radius, and a stem at any point is as thick as all it
//      carries above it, r^e = sum r_i^e. That is what makes a limb swell where
//      it meets the trunk and thin out after each branch, and it is the thing a
//      parametric taper cannot fake. The exponent is SOLVED so the trunk comes
//      out at the radius asked for, with twigs at the radius asked for.
//   3. The SKIN: tubes swept along the axes with parallel-transport frames (no
//      twisting), bark UVs at constant texel density, a root flare with
//      buttress lobes, and faint bark relief so no stem is a perfect cylinder.
//   4. The LEAVES: cards on the finest twigs, turned to the sky, drooping as
//      asked, left out of the crown's shaded core (a real crown is a shell),
//      and lit with normals bent out of the crown so it shades as one soft
//      volume instead of ten thousand flat cards.
namespace treegen {

// The crown's silhouette: how long a limb is by its height in the crown
// (Weber & Penn's shape ratios).
enum class Shape : int {
    Conical = 0,        // longest at the bottom (spruce, fir)
    Spherical,          // longest in the middle (oak, lime)
    Hemispherical,      // wide low dome
    Cylindrical,        // all alike
    TaperedCylindrical, // a little shorter towards the top (beech, birch)
    Flame,              // widest low, drawn to a point (poplar-like)
    InverseConical,     // longest at the top (umbrella, acacia)
    TendFlame,          // a softer flame
    Count
};
const char* shapeName(Shape s);
// Relative limb length (0.2 .. 1) at height h in the crown (0 bottom, 1 top).
float shapeRatio(Shape s, float h);

// One order of stems: 0 is the trunk, 1 the limbs on it, 2 the branches on the
// limbs, 3 the twigs. The child-placement fields (count .. rotate) describe how
// THIS level's stems sit on their parent; they are unused on the trunk.
struct Level {
    int   count     = 0;       // children on a parent of full length
    float start     = 0.0f;    // where along the parent they begin (0..1; level 1: the crown base)
    float end       = 1.0f;    // ...and end
    float length    = 0.5f;    // relative to the parent (level 1: to the tree height)
    float lengthVar = 0.15f;   // +- fraction
    float angle     = 60.0f;   // from the parent's axis at its base (deg)
    float angleTip  = 40.0f;   // ...at its tip (the upper ones rise steeper)
    float angleVar  = 8.0f;    // +- deg
    int   whorl     = 1;       // per node: 1 alternate, 2 opposite, 3+ whorled
    float rotate    = 137.5f;  // turn between successive nodes (deg)
    // The stem's own growth.
    float curve     = 0.0f;    // total bend over its length (deg, + = upwards)
    float gnarl     = 0.15f;   // smooth wiggle (0 = straight)
    float gravity   = 0.0f;    // + droops, - rises towards the light
    float taper     = 0.4f;    // extra thinning towards the tip on top of the pipe model
    int   segments  = 8;       // axis resolution of a full-length stem
    int   sides     = 8;       // ring vertices at most (thin stems use fewer)
    int   forks     = 0;       // successive two-way splits along the stem
    float forkAngle = 30.0f;   // between the two halves (deg)
};

struct Leaves {
    bool  enabled   = true;
    float perStem   = 10.0f;   // leaf clusters on each twig
    int   cluster   = 1;       // leaves in a cluster: a rosette round the twig
                               // (sprays: crossed cards, a bottle brush)
    float size      = 0.14f;   // card length (m)
    float sizeVar   = 0.25f;   // +- fraction
    float start     = 0.25f;   // along the twig where they begin (0..1)
    float angle     = 50.0f;   // leaf from the twig (deg)
    float up        = 0.6f;    // 0 random facing .. 1 blade faces the sky
    float droop     = 0.1f;    // how much the card hangs
    float bend      = 0.55f;   // normals bent out of the crown (soft volume)
    float hollow    = 0.35f;   // the crown's shaded core left bare (0..0.9)
    float fold      = 0.15f;   // midrib fold (0 = flat card, 2 tris; else 4)
    bool  alongTwig = false;   // card lies along the twig (needle sprays)
    bool  onParent  = true;    // also on the outer part of the next-coarser level
    int   cols = 1, rows = 1;  // the texture is an atlas of cols x rows leaves
    float aspect    = 1.0f;    // one cell's width / height (set from the image)
};

struct Params {
    std::string   name = "Tree";
    std::uint32_t seed = 1;

    // --- Trunk ---
    float height     = 16.0f;  // metres
    float heightVar  = 0.1f;   // +- fraction, per seed
    float radius     = 0.35f;  // above the flare (m)
    float twigRadius = 0.004f; // every tip (m) -- the pipe model's unit
    float flare      = 0.6f;   // extra radius at the ground (fraction)
    float flareHeight = 1.2f;  // how high the flare reaches (m)
    int   buttress   = 5;      // root lobes around the foot (0 = round)
    float lean       = 3.0f;   // trunk tilt (deg)
    float forkStart  = 0.35f;  // trunk forks happen between these heights
    float forkEnd    = 0.6f;   //   (fractions of the height)
    Shape shape      = Shape::TaperedCylindrical;
    float relief     = 0.03f;  // bark relief on the trunk (fraction of radius)

    int   levels = 3;          // branch orders beyond the trunk (1..3)
    Level level[4];

    Leaves leaves;

    // --- Look ---
    std::string barkTexture;   // image file; "" = built-in
    std::string barkNormal;    // tangent-space normal map for the bark; "" = none
    float barkNormalStrength = 1.0f; // saved as the glTF normalTexture.scale
    bool  barkNormalDX = false;      // green points down (DirectX): flipped on export
    std::string leafTexture;   // RGBA with alpha; "" = built-in
    float barkTile   = 0.9f;   // bark texture width on the trunk (m)
    float barkAspect = 2.0f;   // one tile's height / width (set from the image)

    // --- Budget ---
    float detail = 1.0f;       // rings, sides and segments (0.25 .. 2)
    float leafDensity = 1.0f;  // cards (fewer grow larger to cover the same)
};

// One generated tree, metres, trunk foot at the origin, +Y up. Vertices are
// eight floats (position, normal, uv) -- the layout every tree path in the
// engine uses. UVs follow glTF: v = 0 is the TOP row of the image.
struct Mesh {
    std::vector<float>         bark;
    std::vector<std::uint32_t> barkIdx;
    std::vector<float>         leaves;
    std::vector<std::uint32_t> leafIdx;
    glm::vec3 lo{0.0f}, hi{0.0f};
    int   stems = 0;           // stem pieces grown (a fork makes a new piece)
    int   leafCards = 0;
    float exponent = 2.0f;     // the pipe-model exponent the solve landed on
    std::size_t barkTris() const { return barkIdx.size() / 3; }
    std::size_t leafTris() const { return leafIdx.size() / 3; }
    bool empty() const { return barkIdx.empty() && leafIdx.empty(); }
};

Mesh generate(const Params& p);

// Ready-made species. Textures are left empty (the panel fills in what it
// finds), so every preset also works on a machine without the files.
struct Preset {
    const char* name;
    const char* hint;
    Params      params;
};
const std::vector<Preset>& presets();

// Saved as a .tree.json beside the .glb, so a tree can be reopened and edited.
void toJson(const Params& p, nlohmann::json& j);
Params fromJson(const nlohmann::json& j);   // missing keys keep their defaults

} // namespace treegen