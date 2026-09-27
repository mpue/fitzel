#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>   // fitzel::MeshData (CPU-side, no GPU)

#include "CityGen.hpp"                // city::Piece (the colliders)
#include "SceneTypes.hpp"             // MaterialDef

// The buildings a town has one or two of: church, police station, fire
// station, hospital -- and the industrial estate at its edge.
//
// Each is a PLOT generator: it is handed the whole plot it stands on (a town
// block, minus its pavement) and lays out the building AND what belongs around
// it -- the forecourt, the parked patrol cars, the fire engines on the apron,
// the ambulances at the emergency entrance -- because that is what makes a
// police station read as one from the street. A box with a blue stripe is a
// box; the same box with a carport of patrol cars beside it is a police station.
//
// Built as triangle meshes rather than as BuildingGen's primitives, because the
// shapes that make these types legible -- a pitched nave, a spire, a sawtooth
// roof, a silo's cone -- are not boxes, cylinders or spheres. Windows on the
// office facades come from the lit shader's procedural window grid (see
// MaterialDef::windowGrid), not from geometry; the church gets real ones, since
// a tall arched window IS its character.
//
// Pure data -> data like the other generators: no GPU, no scene. CityPlan places
// the result through city::merge.
namespace civic {

// The kinds a town can put on a whole block. Append only: the order is the
// index a rule's per-kind settings are kept under while the town is planned.
enum class Kind {
    Church, Police, FireStation, Hospital, Industry,
    // The cultural ones.
    TownHall, School, Kindergarten, Library, Museum, Theatre, Pool,
    // Supply and transport.
    PetrolStation, PowerPlant, Landfill, Station,
    Count
};
constexpr int kKinds = static_cast<int>(Kind::Count);
const char* kindName(Kind k);

// The shared materials ("City Civic ..."), one set per project, re-applied on
// every build like the other generators' palettes.
struct Palette {
    fitzel::AssetId stone, slate, tile, copper, darkGlass, wood, gold;
    fitzel::AssetId office, hospital, brick, brickOffice;
    fitzel::AssetId fireRed, policeBlue, blueLamp, redCross, white;
    fitzel::AssetId concrete, metal, metalRoof, chimneyRed, darkGrey, tank;
    // The street furniture: pavements, traffic lights, bus stops. Few on
    // purpose -- every material is a draw per chunk; the rest reuse the above
    // (poles and frames `metal`, signal heads and dark lamps `darkGrey`).
    fitzel::AssetId pavement, shelterGlass, busYellow, busGreen;
    // The cultural buildings: a cream facade with windows, lawn and sand for
    // the grounds, pool water, a kindergarten's yellow.
    fitzel::AssetId hall, lawn, sand, poolWater, kinder;
    // Supply and transport: asphalt, a fuel brand's colour, landfill earth and
    // rubbish, track gravel and rails, a train's red, the steel of a pylon.
    fitzel::AssetId asphalt, fuel, earth, rubbish, gravel, rail, trainRed, steel;
    // The signal lamps, per approach axis (0 = along the town's X streets, 1 =
    // along Z): each colour its own material, dark in the library, lit per
    // frame by what signalPhase() says -- see CitySystem::forEachSignalLamp.
    fitzel::AssetId signalRed[2], signalAmber[2], signalGreen[2];
};

// --- Traffic signals -------------------------------------------------------------
// The German sequence, the two axes in counterphase with a second of all-red
// between them: green 12 s, amber 3 s, red; red+amber 1 s, then green again.
// One cycle is 34 s. Every light in every town shows the same phase -- one
// material per lamp colour and axis is what keeps them in the merged batches.
struct SignalLamps { bool red = false, amber = false, green = false; };
constexpr float kSignalCycle = 34.0f;
constexpr float kSignalGlow  = 8.0f;    // emission strength of a lit lamp
// What axis 0 and axis 1 show `t` seconds into the town's clock.
void signalPhase(double t, SignalLamps& axis0, SignalLamps& axis1);
// `windowLit` is the town's lit-window fraction, for the office facades.
Palette ensurePalette(std::vector<MaterialDef>& materials, float windowLit);

// One generated plot, in its own frame: x across the frontage, z away from the
// street (the street is at z = -depth/2, beyond the front edge), y up from the
// ground at 0. `parts` are welded meshes per material; `solids` the boxes a
// vehicle should hit (yaw 0, in the same frame).
struct Model {
    std::vector<std::pair<fitzel::AssetId, fitzel::MeshData>> parts;
    std::vector<city::Piece> solids;
    float height = 0.0f;   // tallest point, for the record
    bool  empty() const { return parts.empty(); }
};

// Lay out `kind` on a `width` x `depth` plot. `seed` varies it (tower height,
// storeys, which halls an estate gets). Returns an empty model when the plot is
// too small for the kind to be recognisable at all.
Model build(Kind kind, float width, float depth, std::uint32_t seed, const Palette& pal);

// --- Power lines ---------------------------------------------------------------
// A lattice pylon of the two-circuit kind ("Donaumast"), standing at the origin,
// its arms along x -- the line runs along z. `attach` gets the seven points the
// wires hang from (six conductors, then the earth wire at the top), in the
// pylon's frame.
Model pylon(const Palette& pal, std::vector<glm::vec3>* attach = nullptr);
// A wire from `a` to `b` (world space) sagging `sag` metres at mid-span, as a
// thin three-sided tube.
void cable(fitzel::MeshData& md, glm::vec3 a, glm::vec3 b, float sag, float radius = 0.035f);

// --- Street furniture ----------------------------------------------------------
// Each stands at the origin of its own frame on the ground, its FRONT -- what it
// shows the street -- facing -z, like a plot's.

// A traffic light on a pole at the kerb: one signal head facing the traffic that
// comes towards it, its three lamps wearing axis `axis`'s signal materials.
Model trafficLight(int axis, const Palette& pal);

// A bus shelter, 3.6 m along the kerb and 1.5 m deep, open to the street: a
// glass back and one glass side, a roof, a bench.
Model busShelter(const Palette& pal);

// The stop's sign on its pole: the round yellow disc with the green H (both
// faces) and under it a plate with the stop's name in the street signs'
// typeface (see StreetSign.hpp). The name only on the +z face -- the one the
// bus coming to the stop sees -- because it is most of the sign's vertices.
Model busStopSign(const std::string& name, const Palette& pal);

} // namespace civic
