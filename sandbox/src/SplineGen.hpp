#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>   // fitzel::MeshData (CPU-side, no GPU)

#include "SceneTypes.hpp"             // MaterialDef

// Spline-derived linear structures: fences, walls and railway track.
//
// The unit of authoring is a PATH -- a handful of control points on the ground --
// and a RULE saying what runs along it. The geometry is derived from the two,
// exactly the way the graded road corridor, the guard rails and the roadside city
// are (see RoadSide.hpp / CityGen.hpp): only path + rule are saved, the fence
// itself is rebuilt whenever either changes. Drag a point and three hundred metres
// of wall follows; change one number and every post along it re-spaces.
//
// That is the whole reason this module exists rather than a folder of fence-post
// prefabs. Placing a kilometre of railway sleeper by sleeper needs precise
// dragging a thousand times over, which is exactly what this editor is built to
// avoid -- and it would cost a thousand hierarchy rows, a thousand undo entries
// and a thousand draw calls into the bargain.
//
// Pure data -> data, like BuildingGen and CityGen: no ImGui, no GPU, no scene.
// SplineSystem owns the paths and uploads what comes back; main draws it and
// gives the flagged pieces a static collider in Play. The panel lives in
// SplinePanel.hpp.
namespace splinegen {

// What runs along the path. The three differ in geometry, not in kind of thing:
// each is a cross-section swept along the spline plus something repeated at a
// spacing (posts, piers, sleepers).
enum class Kind { Fence, Wall, Rail, Count };

const char* kindName(Kind k);

// One rule. Every field is metric, so it reads the same whatever the sample
// density, and the fields of the other two kinds simply sit unused -- one struct
// rather than a variant, because the panel switches sections on `kind` anyway and
// a user flipping a fence to a wall and back expects their fence numbers still to
// be there.
struct Style {
    // --- Shared --------------------------------------------------------------
    // Metres the base is pushed into the ground. A structure that follows the
    // terrain samples it at its own spacing, so on rough ground the surface
    // between two samples rises above the line joining them -- without a skirt
    // you see daylight under the wall. This is the cheapest fix and the one every
    // other module here uses (see roadside::Line::sink).
    float sink      = 0.20f;
    // Extra height above the ground the whole structure is raised by. Negative
    // buries it. Independent of the per-point lifts, which shape the run.
    float lift      = 0.0f;
    bool  collide   = true;   // solid in Play (a box per segment, see Result)
    // Material slot letter. Two paths on the same slot share their materials (and
    // therefore their colours), which is what keeps a hundred fences at three
    // materials rather than three hundred. Same trade city::Biome::palette makes.
    int   palette   = 0;      // 0..3 -> "A".."D"
    unsigned seed   = 5;      // reroll the per-piece jitter

    // --- Fence ---------------------------------------------------------------
    float postSpacing = 2.5f;   // metres between posts
    float postWidth   = 0.12f;  // post cross-section (square)
    float postHeight  = 1.80f;  // metres above the ground
    int   rails       = 3;      // horizontal bars spanning the posts (0 = posts only)
    float railThick   = 0.06f;  // bar cross-section (square)
    float railTop     = 0.95f;  // height of the topmost bar, as a fraction of postHeight
    float railBottom  = 0.25f;  // ...and of the lowest one
    // A panel filling the bays: chain-link, boarding, mesh. Thickness in metres;
    // 0 leaves the fence open. Drawn as its own material so a cutout/alpha map can
    // be dropped on it without touching the posts.
    float infill      = 0.0f;
    float infillTop   = 1.0f;   // fraction of postHeight the panel reaches
    float infillBottom= 0.05f;
    // How much each post's height varies, as a fraction. A dead-even fence reads
    // as a decal; a country fence never is.
    float postJitter  = 0.0f;
    // A cap sitting on each post: the detail that separates a picket fence or an
    // iron railing from a row of sticks. 0 = a bare post top.
    float postCap     = 0.0f;   // cap height in metres
    float postCapOver = 0.03f;  // metres it oversails the post on each side

    // Pickets: vertical bars between the posts, at their own (much tighter)
    // spacing. This is what makes a picket fence, a palisade, an iron railing and
    // a stone balustrade all the same rule with different numbers -- and it is
    // why those are presets rather than four more kinds. 0 = none.
    float picketEvery = 0.0f;   // metres between bars
    float picketWidth = 0.07f;  // across the run
    float picketDepth = 0.03f;  // through the fence
    float picketTop   = 0.98f;  // fraction of postHeight the bars reach
    float picketBottom= 0.06f;

    // --- Wall ----------------------------------------------------------------
    float wallHeight  = 2.20f;
    float wallThick   = 0.40f;  // at the base
    float wallTaper   = 0.90f;  // top thickness as a fraction of the base (1 = plumb)
    float copingHeight= 0.10f;  // capping course on top (0 = none)
    float copingOver  = 0.05f;  // metres it oversails each face
    float pillarEvery = 0.0f;   // metres between piers (0 = a plain wall)
    float pillarWidth = 0.60f;  // pier cross-section along AND across the wall
    float pillarRise  = 0.30f;  // metres a pier stands proud of the wall top
    // A base course: the wall holds `toeHeight` metres of its full thickness
    // (plus `toeOver` of splay) before the taper starts. With no splay this is a
    // vertical foot, which is exactly a Jersey barrier's section; with splay it
    // is a plinth. 0 = the taper runs from the ground up.
    float toeHeight   = 0.0f;
    float toeOver     = 0.0f;   // extra half-width at the very bottom
    // Battlements: merlons standing on the wall top with embrasures between them.
    // 0 = a plain top.
    float merlonEvery = 0.0f;   // metres between merlon CENTRES
    float merlonWidth = 0.90f;  // along the wall (< merlonEvery, or there is no gap)
    float merlonRise  = 0.70f;  // metres above the wall top
    float merlonInset = 0.0f;   // metres narrower than the wall, each side

    // --- Rail (railway track) ------------------------------------------------
    float gauge         = 1.435f; // metres between the rail centres (standard gauge)
    float ballastWidth  = 3.60f;  // top width of the bed (0 = no ballast)
    float ballastHeight = 0.45f;
    float ballastSlope  = 1.30f;  // metres the bed spreads per metre of height
    float sleeperSpacing= 0.60f;
    float sleeperLength = 2.60f;  // across the track
    float sleeperWidth  = 0.26f;  // along the track
    float sleeperHeight = 0.16f;
    float railHeight    = 0.16f;
    float railWidth     = 0.07f;

    // --- Colour --------------------------------------------------------------
    // Three slots, meaning whatever the kind needs them to: posts/rails/panel,
    // wall/coping/pier, steel/sleeper/ballast. Applied to the shared palette
    // materials, so editing one recolours every path on that slot.
    glm::vec3 colorA{0.42f, 0.36f, 0.30f};
    glm::vec3 colorB{0.38f, 0.33f, 0.28f};
    glm::vec3 colorC{0.55f, 0.55f, 0.57f};

    // How many world metres one texture tile covers. Applied to the swept parts
    // (wall faces, ballast, rails) along the run AND to the repeated boxes
    // (posts, sleepers, merlons), which are mapped planar in their own space --
    // so one texture reads at the same scale wherever it lands.
    float texTile = 2.0f;

    // Per-element material overrides. Empty (the default) means "use the shared
    // palette slot this kind and `palette` name", which is what every path did
    // before and what keeps a hundred fences at three materials. Set one to a
    // GUID from the project's material library and THAT element -- the posts, the
    // coping, the ballast -- takes it, textures and all.
    //
    // Overriding is per PATH, not per palette slot: two fences on slot A can put
    // different boards on their panels while sharing their posts.
    fitzel::AssetId matA, matB, matC;

    bool operator==(const Style& o) const;
    bool operator!=(const Style& o) const { return !(*this == o); }
};

// A ready-made structure. Presets are the whole answer to "how many kinds of
// fence are there": every one of these is the same three rules with different
// numbers, so adding a type is a row in a table rather than a branch in the
// generator -- and every number stays editable afterwards, which is what makes a
// preset a starting point instead of a menu you are stuck inside.
enum class Preset {
    // Fences
    PostRail, Picket, ChainLink, Palisade, Wire, IronRailing, Ranch, Balustrade,
    Hoarding, Security,
    // Walls
    GardenWall, PierWall, DryStone, Retaining, Battlement, Jersey, ConcretePanel,
    Parapet, SeaWall, LowBoundary,
    // Track
    StandardGauge, NarrowGauge, Tram, Siding,
    Count
};

const char* presetName(Preset p);
// Which kind a preset builds, so the panel can group the list and setting one
// can switch the path's kind with it.
Kind        presetKind(Preset p);
// The style a preset seeds. Colours included: half of what tells a dry stone wall
// from a concrete one is that it is not the same grey.
Style       preset(Preset p);

// The defaults a Kind seeds: its first preset. What a freshly added path and a
// scene field that was never written both fall back to.
Style preset(Kind k);

// The shared material set one palette slot names, per kind.
struct Palette {
    fitzel::AssetId primary;    // posts / wall face / rail steel
    fitzel::AssetId secondary;  // bars / coping / sleepers
    fitzel::AssetId tertiary;   // infill panel / piers / ballast
};

// Find-or-create that set in the project's library and re-apply `s`'s colours to
// it. The materials persist with the PROJECT (as .fmat files) like any other; a
// path only stores their GUIDs. Called before every rebuild, so a colour edit
// shows up without a separate "apply" step.
Palette ensurePalette(std::vector<MaterialDef>& materials, Kind k, const Style& s);

// One drawable part of the finished run: geometry merged per material, in WORLD
// space (so it draws with an identity model matrix). Same shape as city::Batch
// and for the same reason -- this renderer is bound by how many draws it issues,
// not by their triangles, and a fence is thousands of little boxes.
struct Batch {
    fitzel::AssetId  material;
    glm::vec3        lo{0.0f}, hi{0.0f};  // world AABB (culling)
    fitzel::MeshData data;                // released by the caller after upload
};

// A collision box. Coarse on purpose: one box per short run of path, sized to the
// structure's envelope rather than to its parts. A car hitting a fence needs the
// fence to be there, not to be able to thread the gap between two rails.
struct Collider {
    glm::vec3 center{0.0f};
    glm::vec3 half{1.0f};
    float     yaw = 0.0f;   // degrees about +Y
};

struct Result {
    std::vector<Batch>    batches;
    std::vector<Collider> colliders;
    int   pieces = 0;   // posts/sleepers/piers placed (what the panel reports)
    int   verts  = 0;
    float length = 0.0f; // metres of path
    bool  budgetHit = false; // generation stopped at maxPieces

    void clear() { *this = Result{}; }
    bool empty() const { return batches.empty(); }
};

// Build the run along `path` -- the sampled, already-draped centreline in world
// space, one point per sample. `closed` welds the last sample back to the first.
//
// `maxPieces` is a hard budget on the repeated parts: a mis-typed sleeper spacing
// of 0.006 m costs a short track instead of the session. Deterministic: same path,
// same style, same seed gives the same geometry, so nudging one control point does
// not reshuffle a fence the author has been looking at.
Result generate(Kind k, const Style& s, const std::vector<glm::vec3>& path,
                bool closed, const Palette& pal, int maxPieces = 4000);

} // namespace splinegen
