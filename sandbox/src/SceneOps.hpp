#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "SceneTypes.hpp" // Entity, EntityType

struct EditorContext;

// What the editor does to objects as a whole, each one an undoable step: delete
// and duplicate (one object, or the whole selection), unpack a prefab instance,
// make a camera the main one, and the hierarchy menu's "add ..." family -- an
// Empty or a primitive under an object, a cloth hung from it, a camera that
// shoots it or sits in it, an Empty slid in above it, a vehicle's lights. The
// hierarchy panel, the Inspector, the Edit menu and the Delete key all call
// these. Editor only.
namespace sceneops {

// Half-thickness a new Plane gets. Not zero: the pick box would be a sheet
// nobody can click and the box collider would be degenerate.
constexpr float kPlaneHalfY = 0.05f;

// True if entity `id` is `ancestorId` or below it (to reject cyclic reparenting).
bool isUnder(const std::vector<Entity>& entities, int id, int ancestorId);

// Delete the object at `idx` and everything below it. Not the sun.
void deleteEntity(EditorContext& ed, int idx);
// An offset copy of the object at `idx` that KEEPS its parent, selected.
void duplicateEntity(EditorContext& ed, int idx);
// The same over the whole selection (one object: the single versions). A copy
// whose parent was copied too hangs off that copy, not off the original.
void deleteSelection(EditorContext& ed);
void duplicateSelection(EditorContext& ed);

// "Unpack Prefab": the instance `id` belongs to -- and every other selected
// instance when `id` is part of the selection -- becomes ordinary objects.
void unpackPrefab(EditorContext& ed, int id);

// Make the camera on `id` the single Main Camera (-1: none). Nothing for an
// object without a camera.
void setMainCamera(EditorContext& ed, int id);

// A new `type` object under `parentId` (-1: at the root), at a world position
// and rotation, dressed like a new one from the toolbar (`newHalf` its size).
// Returns its id.
int spawnChild(EditorContext& ed, int parentId, EntityType type, const glm::vec3& worldPos,
               const glm::vec3& worldRot, const glm::vec3& newHalf);

// The hierarchy menu's "add ..." family, on the object at `idx`; each selects
// what it made.
void addEmptyChild(EditorContext& ed, int idx);
void addPrimitiveChild(EditorContext& ed, int idx, EntityType type, const glm::vec3& newHalf);
void addClothChild(EditorContext& ed, int idx, int which);   // 0 curtain, 1 flag, 2 banner
void addShotCamera(EditorContext& ed, int idx);
void addCockpitCamera(EditorContext& ed, int idx);
void addEmptyParent(EditorContext& ed, int idx);
void addVehicleLights(EditorContext& ed, int idx);

} // namespace sceneops
