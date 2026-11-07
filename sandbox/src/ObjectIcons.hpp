#pragma once

#include <imgui.h>

struct Entity;

// What an object without a mesh of its own IS, as a picture: a bulb for a point
// light, a film camera for a camera, a speaker for an audio source. The viewport
// draws one over every such object (overlay::icons), so a scene's lights,
// cameras and sounds are seen -- and clicked -- where they stand, instead of
// hunted for by name in the Hierarchy.
//
// Painted like the toolbar's pictures (ToolbarIcons.hpp, Pictogram.hpp), and
// several of them ARE those pictures: vectors into a draw list, sharp at any
// size, nothing to load. Editor only.
namespace objicon {

enum class Kind {
    None,          // it has geometry of its own (or it is the sun): no icon
    Empty,         // a bare pivot: the small cross it has always had
    PointLight, SpotLight,
    Camera, CameraSwitcher,
    AudioSource, SoundZone, ReverbZone, Synth,
    PlayerStart, Trigger, SceneTrigger, Spawner, DoorOpener, Script,
    Particles, Fog,
    Checkpoint, FinishLine,
};

// Which picture `e` gets. A Light by its Light component (point or spot); an
// Empty by the component that makes it what it is -- the first match in a fixed
// order, so an Empty carrying a Camera and a Script reads as the camera; an
// Empty with none of them by the plain cross. Everything else: None.
Kind kindOf(const Entity& e);

// Its colour: the Add menu's colour for the category the object is added from
// (lights amber, cameras blue, sound violet...), so the icon and the menu row
// it came from agree.
ImU32 tint(Kind k);

// The picture alone, centred on `c` and fitting a circle of radius `r`, in
// `col`. `fill` is what a light's bulb or beam is filled with: the colour it
// shines. The other pictures ignore it.
void glyph(ImDrawList* dl, Kind k, ImVec2 c, float r, ImU32 col, ImU32 fill);

} // namespace objicon
