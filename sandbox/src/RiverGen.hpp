#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>   // fitzel::MeshData (CPU-side, no GPU)

#include "SceneTypes.hpp"             // MaterialDef

// Brooks and rivers derived from a drawn path.
//
// The unit of authoring is the same one the road and the fences use -- a handful
// of control points on the ground -- and everything else is derived: the bed cut
// into the terrain, the water surface, where it breaks white, where it falls.
// Only the points and the rule are ever saved (see RiverSystem), exactly like the
// graded road corridor and the roadside city.
//
// The one thing this module does that none of the others has to is decide the
// HEIGHT of what it builds, and that is the whole reason it exists rather than
// being a fourth splinegen::Kind. A fence drapes on the ground and is done; water
// that drapes on the ground runs uphill the moment the author's line crosses a
// rise, and a brook running uphill is not a stylistic problem, it is the thing
// everybody sees first. So the author draws WHERE the water goes and this decides
// HOW HIGH it stands: a strictly descending profile, hugging the ground where the
// ground already falls, cutting where it does not, holding level in a pool above
// a lip and dropping at it. Which stretch is calm, which breaks into rapids and
// which is a fall all come out of that same profile's local gradient, so they can
// never disagree with the geometry the water is actually flowing over.
//
// Pure data -> data, like SplineGen and CityGen: no ImGui, no GPU, no scene, no
// terrain. The ground comes in as a callback; the bed goes out as a section
// function the caller stamps into its own height field. RiverSystem owns the
// paths and does both; the panel lives in RiverPanel.hpp.
namespace rivergen {

// What is being cut. The three differ in their cross-section, which is the only
// thing about a channel that is a kind rather than a number: a brook is a bowl
// worn into the ground, a river is a flat bed between sloping banks, a canal is
// a trough with walls. Everything else that tells one from another -- how wide,
// how deep, how fast, how much of it is white -- is a number, and therefore a
// preset (see Preset), not a fourth branch in here.
enum class Kind { Brook, River, Canal, Count };

const char* kindName(Kind k);

// One rule. Metric throughout, so it reads the same whatever the sample density.
struct Style {
    // --- Channel -------------------------------------------------------------
    float width      = 3.0f;   // water surface width at the source (m)
    // How much wider the mouth is than the source. A real river gathers water on
    // its way down and gets wider for it; 1 keeps a constant section, which is
    // what a canal or a ditch wants.
    float widen      = 1.0f;
    float depth      = 0.6f;   // water surface down to the bed (m)
    // Fraction of the half-width that is flat bottom before the section starts
    // climbing to the waterline. Low is a bowl, high is a trough.
    float bedFlat    = 0.25f;
    // How far past the waterline the cut eases back to the natural ground. This
    // is what stops a channel from being a slot with vertical sides -- and it is
    // where most of the carve's cells go, so it is also the cost dial.
    float bankWidth  = 4.0f;
    // Metres the bank lip stands above the water. A channel in flat ground needs
    // it or the water reads as a puddle with no edge; in a valley the natural
    // ground is already higher and this does nothing.
    float bankRise   = 0.35f;

    // --- Meander ---------------------------------------------------------------
    // A drawn line is a valley axis, not a river. Real water does not run down
    // the middle of it: it wanders, and where it wanders it also deepens and
    // narrows on the outside of each bend and spreads out shallow at the
    // crossings between them. All three of those come from ONE wave here --
    // `wander` in Course -- so they can never end up in the wrong phase relative
    // to each other, which is the mistake that makes an authored river look like
    // a road with water in it.
    //
    // Derived rather than drawn, for the reason this whole editor exists:
    // meandering by hand means placing a control point every few metres and
    // dragging each one precisely, which is the gesture the tool is here to
    // avoid.
    float meander      = 0.0f;   // metres to either side (0 = follow the line)
    float meanderLength= 90.0f;  // metres per wave
    // How far the course is eased, as a multiple of its own width. A drawn line
    // is a polygon and a wide river cannot turn on a corner: a bend tighter than
    // a couple of channel widths is not a bend, it is a fold, and it is where
    // neighbouring sections start cutting through each other's insides.
    //
    // A multiple of the WIDTH rather than a length, so it scales by itself: a
    // two-metre brook still follows every wiggle that was drawn, a thirty-metre
    // river rounds the same drawing into something water could actually take.
    // 0 follows the line exactly -- which is what a dug channel wants, and is
    // why the canal and the ditch have it there.
    float bendEase     = 0.8f;
    // Meanders are a LOWLAND habit: water with a gradient under it goes straight
    // down the fall line and cuts a gorge. So the wander is faded out as the
    // ground steepens, and the gradient it fades out by is `rapidSlope` -- the
    // same number that says where the surface starts to break. One number, two
    // uses, and they agree by construction: a stretch white enough to be a rapid
    // is a stretch too steep to meander.
    //
    // The pool-and-riffle sequence rides on the same wave at twice its frequency
    // (a pool at each bend apex, a riffle at each crossing) -- which is also the
    // ratio real channels have, so there is no separate spacing to set.
    float widthVary    = 0.0f;   // 0..1 of the width the sequence swings
    float depthVary    = 0.0f;   // 0..1 of the depth it swings THE OTHER WAY
    // How hard a bend scours its outside. The deep line of a meandering channel
    // is not down the middle: it runs against the outer bank, with a shallow
    // point bar opposite. 0 keeps the section symmetric.
    float bendScour    = 0.0f;   // 0..1 of the half-width the deep line moves

    // --- Profile -------------------------------------------------------------
    // Forced descent, metres per metre. The floor on how flat the water may run:
    // 0 lets a stretch stand level (a pool, a canal), anything above it
    // guarantees the whole course falls from source to mouth.
    float minSlope   = 0.004f;
    // The deepest the bed may be dug below the natural ground. It is a safety
    // rail, not a look: an author whose line climbs a mountain would otherwise
    // get a two-hundred-metre notch and a carve over a million cells. Where the
    // clamp bites the profile has to rise, which Course reports as `uphill` so
    // the panel can say so rather than letting the water quietly flow backwards.
    float maxCut     = 8.0f;
    // Passes of profile smoothing. The gradient drives the whitewater, so an
    // unsmoothed profile speckles foam wherever the ground noise happens to dip.
    int   smooth     = 8;
    // Which end is the source. Off by default: the terrain already knows, and
    // asking the author to notice which way they drew is the kind of question
    // this editor exists to not ask. Set `flip` when the ground is level enough
    // that the guess is a coin toss.
    bool  autoFlow   = true;
    bool  flip       = false;

    // --- Whitewater ----------------------------------------------------------
    // Gradient (m per m) at which the surface starts to break, and the one at
    // which it stops being a slope and becomes a fall.
    float rapidSlope = 0.05f;
    float fallSlope  = 0.40f;
    // A drop shorter than this is a rapid however steep it is -- otherwise every
    // boulder-sized step in the ground noise becomes a waterfall.
    float fallMin    = 1.0f;
    // Metres of level water held above a lip and below a fall. The pool is what
    // makes a fall read as one: water arriving at a lip is calm, and water
    // landing at the bottom is calm again a few metres on.
    float poolLength = 7.0f;
    // Extra depth scooped out under a fall. A plunge pool is darker than the
    // stretch above it, which is most of what says "this fell a long way".
    float plunge     = 0.8f;

    // --- Look ----------------------------------------------------------------
    // The two ends of the depth ramp: what the water looks like where it is a
    // finger deep, and where it is over your head.
    glm::vec3 shallow{0.42f, 0.62f, 0.58f};
    glm::vec3 deep   {0.03f, 0.14f, 0.18f};
    float flowSpeed  = 0.8f;   // surface scroll, m/s (the CURRENT is separate)
    float clarity    = 1.5f;   // higher = the bed shows through deeper water
    float reflect    = 0.55f;  // Fresnel cap on the environment reflection
    float rippleScale= 0.7f;   // ripple frequency (higher = finer)
    float ripple     = 0.05f;  // ripple normal strength
    float foamWidth  = 0.45f;  // metres of foam clinging to each bank
    float sparkle    = 0.6f;   // specular glitter off the ripples

    // --- Banks ---------------------------------------------------------------
    // Which terrain paint layer the channel lays over its bed and margins: the
    // gravel, the sand, the mud that says water has been here. -1 paints nothing,
    // which is what a scene without such a layer wants and what every course
    // starts as.
    //
    // It matters more than it sounds. A cut alone gives you a channel of the same
    // grass as the meadow it runs through, and grass to the waterline is the one
    // thing that stops a stream reading as a stream -- rivers leave their bed
    // behind them, and that leaving is what an eye recognises before the water.
    int   bankLayer  = -1;
    float bankPaint  = 2.0f;   // metres past the waterline the paint reaches
    float bankBlend  = 0.85f;  // 0..1 how strongly it takes the surface over

    // Stones in and beside the channel, and reeds standing in the shallows.
    // Derived instances along the line, never entities -- a kilometre of brook
    // wants a few thousand of them and a scene cannot carry a few thousand
    // hierarchy rows, undo snapshots and draw calls for gravel (the same trade
    // roadside::Line and city::Biome make).
    //
    // Both are counts per hundred metres rather than spacings, so a wide river
    // and a narrow brook read the same at the same number.
    float stones     = 0.0f;   // per 100 m (0 = none)
    float stoneSize  = 0.35f;  // typical radius (m)
    float stoneSpread= 1.4f;   // how far past the waterline they scatter (m)
    float reeds      = 0.0f;   // clumps per 100 m (0 = none)
    float reedHeight = 1.1f;   // m
    float reedDepth  = 0.35f;  // only where the water is shallower than this (m)
    glm::vec3 stoneColor{0.44f, 0.42f, 0.39f};
    glm::vec3 reedColor {0.36f, 0.44f, 0.22f};

    // --- Current (Play) ------------------------------------------------------
    // Metres per second the water pushes anything floating in it, at the surface
    // in the middle of the channel. 0 is still water.
    float current    = 1.2f;

    unsigned seed    = 7;      // reroll the per-stretch jitter

    bool operator==(const Style& o) const;
    bool operator!=(const Style& o) const { return !(*this == o); }
};

// A ready-made channel. Same trade splinegen::Preset makes: adding a type of
// water is a row in a table rather than a branch in the generator, and every
// number stays editable afterwards.
enum class Preset {
    Brook, MountainStream, Creek, River, WideRiver, Ditch, Canal, MillRace,
    Count
};

const char* presetName(Preset p);
Kind        presetKind(Preset p);
Style       preset(Preset p);
// The defaults a Kind seeds: its first preset. What a freshly added path and a
// scene field that was never written both fall back to.
Style       preset(Kind k);

// --- The course --------------------------------------------------------------
// The solved profile, station by station along the sampled path. Everything
// downstream -- the carve, the surface, the foam, the spray, the current --
// reads this and nothing else, so none of them can disagree about where the
// water is or how fast it is going.
struct Course {
    std::vector<glm::vec3> line;    // centreline AT THE WATER SURFACE, world
    std::vector<glm::vec2> dir;     // unit XZ tangent, pointing DOWNSTREAM
    std::vector<float>     ground;  // natural ground height at each station
    std::vector<float>     bed;     // channel floor (surface - deep)
    std::vector<float>     half;    // half-width of the water here (m)
    // The water depth at each station, and where in the section it is deepest.
    // Both vary along the course -- pools are deep and narrow, riffles wide and
    // shallow, and on a bend the deep line sits against the outer bank rather
    // than down the middle. `shift` is metres off the centreline, positive
    // towards the left-hand bank looking downstream.
    std::vector<float>     deep;
    std::vector<float>     shift;
    // The meander wave that drives all of it, -1..1. Kept because the editor
    // draws it and the panel reports it: a river whose wander was quietly
    // suppressed by the terrain should be able to say so.
    std::vector<float>     wander;
    // Arclength from the source, IN PLAN. Everything that reasons about the
    // course measures in this one: a gradient is a drop over a plan distance, and
    // a pool is so many metres of ground.
    std::vector<float>     s;
    // ...and the distance along the WATER SURFACE, which on a fall is the whole
    // height of the fall longer. Only the texturing uses it, and it has to: the
    // ripples are a physical size on the surface, and running them off the plan
    // distance freezes the pattern down a fall face into vertical streaks --
    // which is precisely what a curtain of falling water does not do.
    std::vector<float>     sSurf;
    // The coordinate the surface pattern is actually drawn in: surface distance
    // DIVIDED BY the local speed, so it is a travel time rather than a length.
    //
    // This exists because the obvious way is wrong. Scrolling the pattern by
    // `time * localSpeed` makes the phase depend on position TIMES time, so its
    // gradient picks up a term that grows without bound -- and once `time` is
    // large enough that term wins and the texture visibly runs backwards on any
    // stretch where the speed changes. Shifting one coordinate by one clock
    // instead cannot do that: the pattern moves at the local speed everywhere,
    // stays continuous across the joins, and stretches where the water speeds
    // up, which is what a real current does to foam.
    std::vector<float>     flow;
    std::vector<float>     white;   // 0..1 whitewater
    std::vector<float>     slope;   // local gradient, m per m (>= 0 downstream)
    // How far the water at this station stands CLEAR of the ground it came off,
    // 0 (on the bed) to 1 (in mid air). Only a fall makes it anything else: over
    // a lip the surface leaves the rock on a parabola and for a few metres it is
    // genuinely flying (see solve, step 6b).
    //
    // Two stages have to know. The carve, because a section stamped under a jet
    // would RAISE the ground to meet it and build an earth ramp down the cliff.
    // And the shader, because falling water is not rippled water: it is drawn as
    // filaments that stretch as they accelerate, and this is what tells it where
    // to do that.
    std::vector<float>     air;

    // Sample index of each control point, so the editor can mark the stretch a
    // point owns. Already flipped with the course when the flow was reversed.
    std::vector<int>       ptSample;

    bool  reversed = false; // the author's points ran mouth-to-source
    float length   = 0.0f;  // metres of channel
    float drop     = 0.0f;  // total descent, source to mouth (m)
    float maxCut   = 0.0f;  // deepest the bed sits below the natural ground (m)
    bool  uphill   = false; // maxCut forced the profile to rise somewhere
    int   falls    = 0;     // steps classified as a fall

    bool empty() const { return line.size() < 2; }
};

// Solve the course over `flat` (the sampled path in world XZ, source-to-mouth
// order not assumed) against the ground `groundAt` reports. `bias` is one value
// per SAMPLE -- metres the water surface is pushed above the ground it would
// otherwise hug, which is how a per-control-point handle dams a pool or digs a
// cut without ever being able to make the water climb.
Course solve(Kind k, const Style& st, const std::vector<glm::vec2>& flat,
             const std::vector<float>& bias, const std::vector<int>& ptSample,
             const std::function<float(float, float)>& groundAt);

// The height the channel wants the GROUND to be, `d` metres to the side of a
// station whose water stands at `surf` with its floor at `bed` and a half-width
// of `half`, over natural ground `g`.
//
// One function, called by the carve for every cell and by nothing else -- but it
// is public because "where does the bank stop" is a question the vegetation
// filter and the bank dressing both have to be able to ask without re-deriving
// it and getting a slightly different answer.
// `shift` moves the deepest point of the section off the centreline, which is
// what a bend does -- the section stays anchored to the waterline at both
// +/- half, so the outer side is steep and the inner one runs out as a point bar.
float sectionHeight(Kind k, const Style& st, float d, float half, float shift,
                    float surf, float bed, float g);

// How far from the centreline the carve reaches at a station of half-width
// `half`: the waterline plus the bank. Outside this the ground is untouched.
inline float reach(const Style& st, float half) { return half + st.bankWidth; }

// --- Geometry ----------------------------------------------------------------
// The water surface: one strip along the course, in WORLD space (identity model
// matrix), carrying everything the river shader needs in its vertex attributes.
//
// The `paint` attribute -- four floats every mesh vertex already has and which
// only the terrain otherwise uses -- carries it: x is the METRES of water under
// the vertex, y is the whitewater factor, z is the channel's half-width in
// metres (so the bank foam can sit a fixed distance in from an edge that moves)
// and w is Course::air, which is what tells the shader it is drawing a falling
// curtain rather than a surface. That is why the river needs no vertex format of
// its own, and why the fall curtains can be part of the same strip.
struct Surface {
    fitzel::MeshData data;
    glm::vec3        lo{0.0f}, hi{0.0f};  // world AABB (culling)
    int              verts = 0;
};

Surface surface(Kind k, const Style& st, const Course& c);

// Where the water is loud enough to throw spray: the foot of every fall and the
// hardest of the rapids. Emitters, not particles -- the caller feeds them to the
// particle pool at whatever rate it can afford.
// --- Bank dressing -----------------------------------------------------------
// One drawable part of the stones and reeds, merged per material and in WORLD
// space (so it draws with an identity model matrix). Same shape as city::Batch
// and splinegen::Batch, and for the same reason: this renderer is bound by how
// many draws it issues, not by their triangles.
struct Batch {
    fitzel::AssetId  material;
    glm::vec3        lo{0.0f}, hi{0.0f};   // world AABB (culling)
    fitzel::MeshData data;                 // released by the caller after upload
};

// The two shared materials the dressing wears. Find-or-created in the project's
// library and re-coloured from `st` on every rebuild, so a colour edit lands
// without a separate apply step -- exactly like splinegen::ensurePalette.
struct Dressing {
    fitzel::AssetId stone, reed;
};
Dressing ensureDressing(std::vector<MaterialDef>& materials, const Style& st);

// Scatter stones and reeds along `c`. Deterministic from the style's seed: the
// same course gives the same boulders, so nudging one control point does not
// reshuffle a bank the author has been looking at. `maxPieces` is a hard budget,
// so a mistyped count costs a short bank instead of the session.
std::vector<Batch> dressing(Kind k, const Style& st, const Course& c,
                            const Dressing& mats, int maxPieces = 4000);

struct SprayPoint {
    glm::vec3 pos{0.0f};      // at the water surface
    glm::vec3 dir{0.0f};      // downstream, in the plane
    float     strength = 0.0f; // 0..1
    float     width    = 1.0f; // metres across to scatter over
};

std::vector<SprayPoint> spray(const Style& st, const Course& c);

} // namespace rivergen
