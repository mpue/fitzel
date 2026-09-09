#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "SceneTypes.hpp"

// Procedural building generator, specialised on skyscrapers and futuristic
// towers: a handful of numbers in, a finished entity subtree out (a root Empty
// with primitive children), ready to be saved as a .fprefab and dropped into a
// track any number of times.
//
// Deliberately built from the editor's OWN primitives (Box / Cylinder / Sphere /
// Ramp) rather than a generated mesh: the result is an ordinary, selectable,
// editable subtree that already renders, lights, collides and serializes -- so
// prefabs, materials, physics and undo all work with zero new plumbing, and a
// building stays hand-editable after generation (delete the crown, drag a mass).
// Window detail comes from MATERIALS, not geometry, which is what keeps a tower
// at ~30 objects instead of ~3000.
//
// This module is pure data -> data: it knows nothing about ImGui, the GPU or the
// scene it will land in (the panel lives in BuildingPanel.hpp, the placement in
// main). Same split as RoadSystem/RoadPanel.
namespace buildings {

// A starting point, not a straitjacket: applyStyle() only presets the fields
// below, and every one of them stays editable afterwards.
enum class Style {
    Skyscraper,   // classic setback tower with a mast
    NeoTokyo,     // twisted glass needle with neon bands
    Arcology,     // broad stepped megastructure
    Needle,       // slim spire, mostly crown
    Slab,         // wide brutalist wall of a building
    Habitat,      // round tower with a halo ring
    Count
};
const char* styleName(Style s);

// The tower's cross-section. Chamfered adds a 45-degrees-rotated second mass so
// the silhouette reads as an octagon; Cross and L break up a big footprint.
enum class Footprint { Rect, Chamfered, Round, Cross, L, Count };
const char* footprintName(Footprint f);

// What sits on top -- the single biggest contributor to a skyline's character.
enum class Crown { None, Cap, Spire, Antenna, Dome, Halo, Slant, Count };
const char* crownName(Crown c);

struct Params {
    unsigned seed = 1337;          // reroll for a different building, same knobs

    // Massing -----------------------------------------------------------------
    Footprint shape       = Footprint::Rect;
    float     width       = 26.0f; // footprint X (m)
    float     depth       = 26.0f; // footprint Z (m)
    int       floors      = 42;    // storeys (drives the height)
    float     floorHeight = 3.6f;  // m per storey
    int       sections    = 4;     // stacked masses -> that many setbacks
    float     taper       = 0.55f; // top mass width as a fraction of the base
    float     jitter      = 0.25f; // 0 = perfectly regular stack, 1 = wild
    float     twist       = 0.0f;  // total yaw over the full height (degrees)
    int       podiumFloors = 3;    // storeys of the wider base block (0 = none)
    float     podiumSpread = 1.5f; // podium footprint vs the tower's
    float     sink        = 0.6f;  // m pushed into the ground (hides the seam)

    // Detail ------------------------------------------------------------------
    int   bandEvery    = 6;        // mechanical band every N floors (0 = none)
    float bandOverhang = 0.4f;     // m the bands stand proud of the facade
    int   fins         = 4;        // vertical fins per mass (0 = none)
    float finDepth     = 0.6f;     // m the fins stand proud of the facade
    bool  neon         = true;     // emissive strip on every band + the crown
    // Lit lines up the corners of every mass. The bands above measure a
    // building's floors and make it read WIDE; one unbroken vertical line is what
    // the eye follows all the way up, which is where a tower's height comes from.
    // A checkbox and not a count, because "which of the four corners" is not a
    // question anyone wants to answer with a slider.
    bool  edgeStrips   = true;
    // Narrow, and dimmer than a sign. An edge strip is an unbroken line 150 m
    // long: at the neon's own brightness it is by far the loudest thing on the
    // tower, and a facade whose FRAME glows while its surface stays dark is the
    // exact inverse of a lit building. In the photographs the structure is dark
    // and the light comes out of the windows.
    float edgeWidth    = 0.34f;    // m across each strip
    Crown crown        = Crown::Antenna;
    float crownHeight  = 0.18f;    // crown height as a fraction of the body
    bool  beaconLight  = true;     // a real point light in the beacon (1 per tower)
    bool  collider     = true;     // static physics on the masses (drive into it)

    // --- Windows -------------------------------------------------------------
    // Lit windows on the facade, drawn procedurally by the lit shader out of the
    // world position (see MaterialDef::windowGrid). They cost no geometry and no
    // texture, and they are the single biggest step from "sci-fi monolith" to
    // "city at dusk": a tower with them reads as inhabited, one without reads as
    // a smooth prism no matter how good its silhouette is.
    //
    // Like the colours, they live on the SHARED palette material, so they are a
    // district-wide property -- and the row height is not a slider: a window
    // belongs to a storey, so it comes from `floorHeight`.
    bool      windows     = true;
    // 2.6 m, not 3.4: at the old pitch a 26 m facade carried eight windows, which
    // is a warehouse. The reference photographs run to twenty and more across a
    // face, and that density IS the look -- it is what makes a tower read as
    // storeys full of people rather than as a lit box.
    float     windowWidth = 2.6f;   // m of facade per window (the column pitch)
    float     windowLit   = 0.55f;  // fraction lit; 0 = a dead, unoccupied tower
    glm::vec3 windowColor{1.00f, 0.83f, 0.58f};  // warm interior light (sRGB)
    float     windowGlow  = 2.6f;   // emission strength of a lit window

    // --- Signage -------------------------------------------------------------
    // Lit signs on the facades, and the single biggest step from "a tower at
    // night" to "a city somebody lives in". A skyline made of glazing and neon
    // trim reads as architecture; the same skyline with banners down its corners,
    // screens across its flanks and a lit ground floor reads as a STREET -- and
    // that is what the references everyone has in mind are actually made of.
    //
    // Geometry, not shader, and deliberately so. A sign has to sit on ONE face at
    // ONE place with ONE colour; a hashed facade pattern would put the same sign
    // on all four sides of every building on the palette. The cost is a handful
    // of boxes on a tower that already has thirty -- the fins alone cost more.
    //
    // None of it collides: signs are 30 cm of plastic on a wall, and a craft that
    // clips one should keep flying rather than explode against an advert.
    int   signs      = 3;      // facade signs per tower (0 = none)
    bool  screens    = true;   // allow the wide screen kind, not only banners
    bool  shopfronts = true;   // lit ground floor around the podium
    bool  roofSign   = true;   // a lit sign standing on the roof, on a frame
    float signGlow   = 5.0f;   // emission of the sign faces
    // Two sign colours besides the neon accent, so a street is not monochrome.
    // Three is the number that reads as a city: any two of them together look
    // like a scheme, and five look like a toy.
    glm::vec3 signColorA{1.00f, 0.12f, 0.45f};   // magenta
    glm::vec3 signColorB{0.30f, 0.95f, 1.00f};   // cyan
    // The ground floor is NOT one of them, and that is the whole lesson of the
    // first attempt: the lit band round a podium is the largest continuous
    // emissive surface in a district and it sits at eye level, so whatever colour
    // it is, the street becomes. Made magenta, a city turns purple through the
    // bloom before a single sign has been read.
    //
    // Shopfronts are interior light -- warm, and dimmer than a sign, because they
    // are lit rooms rather than advertising. Magenta is the spice in these
    // references, never the base coat.
    glm::vec3 shopColor{1.00f, 0.82f, 0.58f};    // warm interior white

    // --- Wear ----------------------------------------------------------------
    // How run-down the building is. `weathering` acts on the MATERIALS (shared
    // per palette slot, so it is a district-wide property): it kills the mirror
    // finish and soils the colours. The default is deliberately not 0 -- a
    // freshly generated tower should look like a building, and 0.70 reflectivity
    // on glass is a chrome ball, not a facade. Set it to 0 for the pristine
    // curtain-wall look.
    float weathering = 0.55f;      // 0 = new glass tower .. 1 = derelict concrete
    // Fraction of a tower's masses that are bare structural concrete (the `base`
    // material) instead of glazing. This is what varies building to building
    // WITHOUT costing another material: a street where some towers are glass and
    // their neighbours are raw concrete reads as neglected, where a street of
    // uniformly dark glass just reads as night.
    float grime      = 0.0f;       // 0..1
    // Rooftop and terrace clutter: tanks, vents, plant boxes and ducts scattered
    // over the setback roofs. Pure silhouette junk, and the cheapest way to make
    // a clean extrusion look lived-in and unmaintained.
    int   clutter    = 0;

    // Look --------------------------------------------------------------------
    // Generated buildings share ONE material set per `palette` slot (A..H), so a
    // whole city costs at most 8 x 4 materials instead of four per tower. Editing
    // the colours below re-tints every building on that slot.
    int       palette = 0;                        // 0..7 -> "Building A".."H"
    glm::vec3 glassTint{0.09f, 0.13f, 0.18f};     // facade
    glm::vec3 frameTint{0.16f, 0.17f, 0.20f};     // bands, fins, mast
    glm::vec3 accentColor{0.20f, 0.85f, 1.00f};   // neon / beacon
    // Dark, not mid-grey. The podium is the biggest unlit surface at street level,
    // and at 0.42 it was the brightest thing in a night render -- brighter than
    // the windows above it, which is the opposite of every photograph of a city
    // at night, where the base is a dark mass with lit shopfronts cut into it.
    glm::vec3 baseTint{0.15f, 0.155f, 0.17f};     // podium
    float     accentStrength = 3.0f;              // neon glow multiplier
};

// Preset `p`'s massing/detail fields for `s` (colours and seed are left alone,
// so switching style keeps the city's palette).
void applyStyle(Params& p, Style s);

// The shared surface materials a generated building references. Six per palette
// slot, so a whole city still costs 8 x 6 materials rather than six per tower.
struct Palette {
    fitzel::AssetId glass, frame, accent, base;
    // The sign faces. Separate materials rather than a tinted accent, because
    // emission is a material property here -- and because a street where every
    // sign is the same colour is a street with one sign on it.
    fitzel::AssetId signA, signB;
    // The accent again, at a fraction of its glow: for the long unbroken lines
    // (the corner strips), where full neon brightness swamps everything else.
    fitzel::AssetId trim;
    // ...and the ground floor, which is warm and dim rather than a third sign
    // colour (see Params::shopColor).
    fitzel::AssetId shop;
};

// Find-or-create the material set for `p.palette` in the project's library and
// re-apply the colours from `p` to it. Returns the four GUIDs the generator
// stamps onto its entities. The materials themselves persist with the PROJECT
// (as .fmat files), not with the prefab -- a prefab only stores their GUIDs.
Palette ensurePalette(std::vector<MaterialDef>& materials, const Params& p);

// Build the building. Returns a parent-before-child entity list whose front() is
// an Empty root standing at `groundPos` -- exactly the shape AddEntitiesCmd and
// prefab::fromSubtree want. Ids are minted from `entityCounter`.
std::vector<Entity> generate(const Params& p, const Palette& pal,
                             int& entityCounter, const glm::vec3& groundPos);

// How many objects the current parameters produce (the panel shows it before you
// commit, so the cost of "more fins" is visible). Runs the generator on a
// throwaway, which is cheap at these counts.
int objectCount(const Params& p);

} // namespace buildings
