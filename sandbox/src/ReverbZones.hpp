#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "MixerPanel.hpp"
#include "SceneTypes.hpp"

// Reverb Zones: rooms the sound rings in (ReverbZoneComponent, Component.hpp).
//
// Every zone is an entity's box -- placed, turned and sized with the gizmo -- and
// an aux bus of the mixer. Each frame:
//
//   * every zone is tied to its bus: the bus it names; failing that the bus it
//     was tied to before (renamed in the Mixer: the zone takes the new name);
//     failing that a new one, "Room", with a reverb insert and sends from Ambient
//     and SFX -- so a zone shows up in the Mixer the moment it is made;
//   * how far the listener is inside each zone is worked out: 0 outside, 1 more
//     than the zone's `fade` inside every face, in between across that band. A bus
//     several zones send into takes the largest. It follows over a quarter of a
//     second, so walking through a door does not click;
//   * the result goes to Desk::zoneWeight, which Desk::sync multiplies every send
//     to that bus by: the channel's send as set in the Mixer inside, none outside.
//
// Runtime (the player has it too); the editor runs it as well, so a new zone's
// bus is in the Mixer at once and the panel can show where the camera is.
namespace reverbzones {

// How far `p` (world) is inside a box of centre `c`, Euler `rotDeg` (the
// engine's, degrees) and half-extents `half`: 0 outside, 1 at least `fade` inside
// every face (fade 0: 1 anywhere inside).
float inside(const glm::vec3& p, const glm::vec3& c, const glm::vec3& rotDeg, const glm::vec3& half, float fade);

// Tie the zones to their buses, and set each bus's weight for a listener at
// `listener`. `dt` paces the follow (0: jump straight to the new weights).
void update(std::vector<Entity>& entities, mixerui::Desk& desk, const glm::vec3& listener, float dt);

} // namespace reverbzones
