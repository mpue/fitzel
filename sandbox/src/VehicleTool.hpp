#pragma once

#include <functional>
#include <string>

class Document;
class VehicleComponent;
struct Entity;

// The "connect a model to the vehicle system" tool: one-click wheel/chassis
// auto-detection that fills a VehicleComponent from a model's child AABBs,
// plus the component's inspector UI and the Vehicle panel's scene-vehicle
// section. The drive integration (Jolt spawn, per-frame transform sync,
// arcade test-drive) lives in main, wired off the component.
namespace vehicleui {

// One-click setup on the entity `rootId`: find the four wheels among its
// children (by name -- wheel/tire/tyre/rad/reifen/felge -- else by a
// disc-shaped-AABB heuristic), assign them to the FL/FR/RL/RR slots by
// quadrant, and derive wheel radius/width, track, axle positions and the
// chassis box from the AABBs. Adds the VehicleComponent when missing; safe
// to re-run (geometry is refreshed, tuned mass/torque are kept). Returns a
// human-readable report of what was detected.
std::string autoSetup(Document& doc, int rootId);

// Inspector body for a VehicleComponent on `root`: the metadata properties,
// four wheel-slot pickers over the entity's children, and a re-detect button.
void inspector(VehicleComponent& vc, Entity& root, Document& doc);

// The "Scene vehicles" section of the Vehicle panel: lists every drivable
// entity, and runs `makeDrivable(entityId)` (autoSetup wrapped undoably by
// main) on the current selection. `selectedId` is the selected entity's id
// (-1 none). Returns an entity id the user clicked to select, or -1.
int panelSection(Document& doc, int selectedId,
                 const std::function<std::string(int)>& makeDrivable);

} // namespace vehicleui
