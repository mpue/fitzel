#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "CivicGen.hpp"      // civic::Model
#include "SceneTypes.hpp"    // MaterialDef

// The small things of a street -- the ones nobody notices until they are
// missing, and then the town looks like a model railway before the details:
// the traffic signs at a crossing, the yellow sign with the town's name where a
// street leaves it, a litter bin by the lamp post, the advertising column on
// the corner, a billboard on an empty plot, the manhole covers down the middle
// of the street and the gullies at the kerb, the number beside every front
// door, a shop's name over its window.
//
// Each is a small civic::Model (see CivicBuilder.hpp) standing at the origin of
// its own frame on the ground, its FRONT -- what it shows the street -- facing
// -z, like the other street furniture. CityPlan decides where they go; this
// module only knows what they look like. Lettering is the street signs'
// typeface (StreetSign.hpp), as geometry.
//
// Few materials on purpose ("City Prop ..."): the town merges its furniture per
// kilometre square and material, and every material is a draw there. The sign
// colours double as the posters' and the shop boards' colours.
//
// Pure data -> data: no GPU, no scene.
namespace props {

struct Palette {
    // The traffic-sign colours (RAL 3020, 9016, 5017, 1023, 9005, 6024), and
    // the grey of a sign's back and the galvanised pole.
    fitzel::AssetId red, white, blue, yellow, black, green, back, pole;
    fitzel::AssetId iron;      // manhole covers and gully grates
    fitzel::AssetId bin;       // the litter bins' body
    fitzel::AssetId column;    // an advertising column's cap and foot
    // Lettering that lights up after dark (shop names): white and warm. The
    // caller scales their emission with the street lamps (CitySystem::nightGlow),
    // so by day they are only their colour.
    fitzel::AssetId glowWhite, glowWarm;
};
Palette ensurePalette(std::vector<MaterialDef>& materials);

// --- Traffic signs ----------------------------------------------------------------
// The German ones a town needs (Straßenverkehrs-Ordnung numbers in brackets).
enum class Sign {
    GiveWay,        // (205) Vorfahrt gewähren -- the red-rimmed triangle, point down
    Stop,           // (206) Halt. Vorfahrt gewähren -- the octagon
    PriorityRoad,   // (306) Vorfahrtstraße -- the yellow diamond
    Crossing,       // (350) Fußgängerüberweg -- blue, a white triangle
    Parking,        // (314) Parken -- blue, the white P
    Count
};
const char* signName(Sign s);
// On a pole, the face's lower edge about two metres up.
civic::Model trafficSign(Sign s, const Palette& pal);

// (310/311) The town's name where a street leaves it: yellow, a black rule, the
// name in the street signs' face, on two posts. The front (-z) greets whoever
// drives in; the back is the end of the town, the name struck through in red.
civic::Model townSign(const std::string& name, const Palette& pal);

// --- On the pavement ------------------------------------------------------------
// A litter bin on its post, 0.95 m high.
civic::Model bin(const Palette& pal);
// An advertising column (Litfaßsäule): 2.9 m, a dome on top, four posters
// round it. `seed` picks the posters.
civic::Model advertColumn(std::uint32_t seed, const Palette& pal);
// A billboard (the 18/1 sheet, 3.6 x 2.6 m) on two legs, its poster facing -z.
civic::Model billboard(std::uint32_t seed, const Palette& pal);

// How many poster designs there are (columns and billboards pick from them).
int posterCount();

// --- In the street --------------------------------------------------------------
// A manhole cover, 0.65 m across, its top `rise` above the origin (the road
// surface). Flat on purpose: it lies on the carriageway.
civic::Model manhole(const Palette& pal, float rise = 0.02f);
// A gully grate at the kerb, 0.5 x 0.32 m, its long side along x.
civic::Model gully(const Palette& pal, float rise = 0.02f);
// Zebra stripes across a carriageway `width` wide (along x), the stripes 0.5 m
// wide and apart, 3 m long (along z), `rise` above the road.
civic::Model zebra(float width, const Palette& pal, float rise = 0.015f);

// --- On the buildings -------------------------------------------------------------
// A house number: the enamelled blue plate with a white rule and white digits,
// against a wall at z = 0 and facing -z, centred on the origin.
civic::Model houseNumber(const std::string& number, const Palette& pal);
// A shop's fascia board with its name, centred on the origin, against a wall at
// z = 0 and facing -z. `colour` picks the board (and the lettering that goes
// with it); the lettering lights up after dark. `maxWidth` caps the board (the
// name shrinks to fit).
civic::Model shopSign(const std::string& name, int colour, float maxWidth, const Palette& pal);
// A shop as German high streets have them (UTF-8). `seed` picks.
std::string shopName(std::uint32_t seed);

} // namespace props
