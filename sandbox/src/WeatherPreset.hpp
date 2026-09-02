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

// --- The air ----------------------------------------------------------------
// Everything the Sky & atmosphere panel edits about the atmosphere itself: the
// cumulus deck, the ice above it, and the height haze. Gathered into a struct for
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

    // The high layer: ice, above everything the storm dial touches. A front
    // rolling in does not blow the cirrus away, it slides underneath it -- which
    // is why a preset has to carry this separately instead of deriving it.
    float cirrus       = 0.35f;
    float cirrusHeight = 1400.0f;
    float cirrusWind   = 2.5f;   // the jet stream, not the surface wind
    float contrails    = 0.0f;

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
