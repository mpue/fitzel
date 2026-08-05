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
    Crown crown        = Crown::Antenna;
    float crownHeight  = 0.18f;    // crown height as a fraction of the body
    bool  beaconLight  = true;     // a real point light in the beacon (1 per tower)
    bool  collider     = true;     // static physics on the masses (drive into it)

    // Look --------------------------------------------------------------------
    // Generated buildings share ONE material set per `palette` slot (A..H), so a
    // whole city costs at most 8 x 4 materials instead of four per tower. Editing
    // the colours below re-tints every building on that slot.
    int       palette = 0;                        // 0..7 -> "Building A".."H"
    glm::vec3 glassTint{0.09f, 0.13f, 0.18f};     // facade
    glm::vec3 frameTint{0.16f, 0.17f, 0.20f};     // bands, fins, mast
    glm::vec3 accentColor{0.20f, 0.85f, 1.00f};   // neon / beacon
    glm::vec3 baseTint{0.42f, 0.43f, 0.46f};      // podium
    float     accentStrength = 3.0f;              // neon glow multiplier
};

// Preset `p`'s massing/detail fields for `s` (colours and seed are left alone,
// so switching style keeps the city's palette).
void applyStyle(Params& p, Style s);

// The four shared surface materials a generated building references.
struct Palette {
    fitzel::AssetId glass, frame, accent, base;
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
