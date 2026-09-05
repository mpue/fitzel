#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "FogMedium.hpp"

// Weather presets: a named sky you can pick, edit and keep.
//
// The weather was one slider. "Storm", 0 to 1, and everything else about the air
// -- how much cloud, how big, how high, how thick the haze, whether there is mist
// on the ground, how loud the rain is -- was a separate control in a separate
// panel, or not a control at all. That works while there is one sky in a project
// and stops working the moment there are two: getting back to the morning you had
// last week means remembering fourteen numbers spread over two windows.
//
// So the sky is a VALUE now. Everything a weather is made of gathers into one
// struct that can be copied, compared, named, written to the project and applied
// again, and the storm dial becomes one field of it rather than the whole idea.
//
// What a preset deliberately does NOT carry:
//
//  - the colour grade and the exposure. Those are the camera, not the weather.
//    A preset that moved them would fight the Colour grade panel, and worse,
//    would make every preset a look as well as a sky.
//  - where the mist volume STANDS. Its size, thickness and drift travel; its
//    centre is a place in the world, and a place belongs to the scene. (Applying
//    a preset with mist in it sits the box on the ground under the camera --
//    the same thing the panel's "Sit on ground" button does.)
//  - which sound files play. The four loops are fixed (rain, wind, breeze,
//    storm); what differs between a downpour and a squall is how loud each is,
//    and that is what the preset carries.
namespace weather {

// --- One sheet layer --------------------------------------------------------
// Every cloud type except the cumulus. They are sheets and not volumes because
// that is what they physically are: a stratus is two hundred metres of uniform
// grey, an altocumulus is one element thick, and a cirrus has no measurable
// underside at all. Marching any of them spends sixty samples arriving at the
// number the first one already had, and the cumulus march is already the most
// expensive thing in the frame -- so exactly one layer is marched and the rest
// are decided where the ray meets their plane.
//
// One struct for all of them rather than a field set per type: what differs
// between a mackerel sky and a fibrous cirrus is the MASK and how it takes
// light, and both of those live in the shader. Up here they are the same five
// numbers, which is what lets the panel, the preset file and the uniform upload
// each handle them in one loop instead of six copies.
struct Sheet {
    bool  on     = false;
    float amount = 0.5f;      // how much sky it takes, 0..1
    float height = 2000.0f;   // metres
    // Feature size, as a multiplier. Elements are sized in METRES, not in
    // degrees -- raise a layer and it gets finer, which is most of why a
    // cirrocumulus looks like grain and a stratocumulus looks like rolls. This
    // is the author's thumb on top of that.
    //
    // On the contrail layer it means something else, and deliberately: how far
    // the traffic has SPREAD. Nothing about a line has a feature size, and
    // "sharp lines or old smears" is the one thing that slider needs to say.
    float scale  = 1.0f;
    float wind   = 3.0f;      // metres/second, along `dir`
    float dir    = 0.0f;      // heading in radians, measured in the XZ plane
};

// --- The air ----------------------------------------------------------------
// Everything the Sky & atmosphere panel edits about the atmosphere itself: the
// marched cumulus deck, the sheet layer per cloud type above and below it, and
// the height haze. Gathered into a struct for
// one reason -- a preset is a copy of one of these, and a dozen loose floats in
// main() cannot be copied, compared or written to a file as a unit.
//
// The defaults are the editor's own opening sky, so a default-constructed Sky is
// the sky a new project starts with rather than an arbitrary zero.
struct Sky {
    // The cumulus deck. `scale` is a frequency, so LOWER means bigger clouds --
    // the one number in here that reads backwards, and the reason the panel says
    // so next to the slider.
    float coverage = 0.5f;
    float density  = 1.0f;
    float scale    = 0.0009f;
    float wind     = 5.0f;
    float base     = 700.0f;   // metres
    float top      = 2400.0f;  // metres

    // The sheet layers, one per cloud type, each on its own and at its own
    // height. See Sheet above for why they are separate slots rather than a
    // list, and why the cumulus is not one of them.
    //
    // The defaults are the sky the editor has always opened with: a little
    // cirrus and nothing else, so a project made before there were layers looks
    // the same after this as it did before.
    Sheet stratus       = {false, 0.60f,  600.0f, 1.6f,  4.0f, 0.0f};
    Sheet stratocumulus = {false, 0.55f, 1200.0f, 1.0f,  5.0f, 0.6f};
    Sheet altocumulus   = {false, 0.50f, 3400.0f, 1.0f,  7.0f, 1.1f};
    Sheet cirrus        = {true,  0.35f, 1400.0f, 1.0f,  2.5f, 0.0f};
    Sheet cirrocumulus  = {false, 0.45f, 5200.0f, 1.0f,  9.0f, 0.3f};
    Sheet contrails     = {false, 0.40f, 1800.0f, 0.6f,  1.2f, 0.4f};

    // The height haze: aerial perspective, everywhere, with no shape.
    float fogDensity = 0.0045f;
    float fogFalloff = 0.028f;
};

// --- Mist -------------------------------------------------------------------
// The world-wide volumetric volume, as a preset carries it: what the air is made
// of and how far it reaches, but not where it is. Opt-in per preset (see
// Preset::setMist), because a scene may have authored mist that a weather has no
// business deleting -- and because marching it is the most expensive thing in
// here, so a preset must be able to say nothing about it at all.
struct Mist {
    bool      enabled = false;
    bool      follow  = true;   // keep the box on the eye in X/Z
    float     span    = 600.0f; // metres across (the box's X and Z)
    float     height  = 40.0f;  // metres tall
    FogMedium medium;
};

// --- Sound ------------------------------------------------------------------
// Gains on the four looping layers and on the thunder, multiplied onto the
// volumes the storm dial already derives. Not replacements: the dial still says
// when rain is falling at all, and this says how a given weather sounds while it
// does -- which is the difference between a downpour (loud rain, no gale) and a
// squall (the other way round).
struct Audio {
    float rain    = 1.0f;
    float wind    = 1.0f;
    float breeze  = 1.0f;
    float storm   = 1.0f;
    float thunder = 1.0f;
};

// --- A preset ---------------------------------------------------------------
struct Preset {
    std::string name;
    // Shipped with the editor. A built-in can be applied and overwritten in the
    // project, never deleted -- so "Storm" is always there to come back to.
    bool builtin = false;

    // The storm dial, unchanged in meaning: 0 clear, 1 full. It still drives the
    // rain, the wet road, the wave height and the light, and it still overrides
    // the cloud numbers above towards a storm's own as it rises. A preset simply
    // sets it along with everything else instead of being the only way to say
    // what the weather is.
    float storm       = 0.0f;
    bool  autoWeather = false;
    // Whether the sky is allowed to flash at all. Separate from the dial because
    // "heavy rain" and "thunderstorm" are different weathers at the same storm
    // value, and before this every hard rain came with lightning whether the
    // author wanted it or not.
    bool  lightning = true;
    // HOW MUCH is falling, as a multiplier on what the dial derives. The dial
    // says whether it rains at all and how hard the sky is working; this says how
    // heavy the fall is, and the two are genuinely different questions -- a gale
    // with no rain in it and a windless downpour are both weathers, and neither
    // was reachable while one slider owned both.
    //
    // It is a COUNT, not an opacity: at 0.3 there are three tenths as many
    // streaks and three tenths as many rings on the ground, each the same size
    // and the same brightness it would have in a downpour. Light rain is fewer
    // drops, not more transparent ones, and the old fade said the opposite.
    float rain = 1.0f;   // 0 = dry, 1 = what the dial alone used to give, 2 = twice

    // Opt-in: a weather that means a time of day (a dawn mist, a midday sun)
    // moves the clock, and one that does not leaves it where the author put it.
    bool  setTimeOfDay = false;
    float timeOfDay    = 8.0f;   // hours [0,24)

    Sky   sky;
    bool  setMist = false;
    Mist  mist;
    Audio audio;
};

// --- The live state, by reference -------------------------------------------
// Where a preset's values land, and where a captured one comes from. References
// rather than a copy because these ARE the editor's running weather -- applying
// a preset has to change what is on screen this frame, not a snapshot of it.
//
// The mist arrives in pieces rather than as a VolumetricFog::Settings so this
// header stays free of the renderer that owns it: that one drags a Mesh, a
// RenderTarget and two Shaders behind it, and a preset is a struct of numbers.
struct Live {
    float& storm;
    bool&  autoWeather;
    bool&  lightning;
    float& rain;
    float& timeOfDay;
    Sky&   sky;
    Audio& audio;

    bool&      mistOn;
    bool&      mistFollow;
    glm::vec3& mistCenter;
    glm::vec3& mistSize;
    FogMedium& mistMedium;
};

// --- Built-ins --------------------------------------------------------------
// The skies every project starts with. Enough of a spread that the dial's whole
// range is reachable by name, and each one is a weather somebody would actually
// ask for rather than a step on a slider.
std::vector<Preset> builtins();

// --- The project's list -----------------------------------------------------
// Read <projectFolder>/weather.json and merge it over the built-ins: a saved
// preset with a built-in's name REPLACES it (that is what "Update" does to a
// built-in), anything else is appended. A missing or unreadable file gives the
// built-ins alone, so a project that has never saved one still opens with five.
std::vector<Preset> load(const std::string& projectFolder);

// Write <projectFolder>/weather.json. Built-ins that still match their shipped
// values are left out, so the file holds what the author actually decided rather
// than a copy of the defaults -- and so a later change to a built-in reaches
// every project that never touched it.
void save(const std::string& projectFolder, const std::vector<Preset>& presets);

// Index of `name` in `presets`, or -1. Case-insensitive: a preset is picked by
// name in a combo, and two that differ only in case are the same one to a user.
int indexOf(const std::vector<Preset>& presets, const std::string& name);

// --- Applying and capturing -------------------------------------------------
// Write `p` into the live weather. `groundY` is the terrain height under the
// camera and is used for one thing: sitting the mist volume on the ground, so a
// dawn layer is a layer on the floor of the valley the author is standing in
// rather than at whatever height the last scene left the box.
void apply(const Preset& p, const Live& live, float groundY);

// The other direction: everything the live weather currently is, as a preset
// called `name`. `setTimeOfDay` and `setMist` are the caller's decision -- they
// are not readable from the state, only from what the author meant.
Preset capture(const std::string& name, const Live& live,
               bool setTimeOfDay, bool setMist);

// Whether the live weather still IS this preset, within a tolerance that ignores
// the last digit of a slider. What the panel uses to mark a picked preset as
// edited -- a name still showing after the author has dragged half the sky
// somewhere else is a lie the panel tells once and is not believed again.
bool matches(const Preset& p, const Live& live);

} // namespace weather
