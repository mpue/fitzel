#pragma once

#include <string>
#include <vector>

#include "RoadSide.hpp"

struct Entity;
namespace prefab { struct Prefab; }

// Stamping PREFABS along a road: the same "every N metres, this far across, this
// rotation/scale/height" rule the side objects use, but the result is real
// entities -- one prefab instance per station, in one undoable step.
//
// Why entities and not derived instances like RoadSide's: a prefab is an entity
// subtree. Its scripts, lights, particles and physics components only mean
// anything as scene objects, and flattening it to a pile of transforms would
// silently drop exactly the parts that made it a prefab rather than a model. So
// this tool STAMPS: it places once, the placements are ordinary objects, and they
// no longer follow the road when the road moves. That is the trade -- the derived
// path (RoadSide.hpp) is still there for anything that should stay glued to the
// centreline.
//
// The placement itself is deliberately NOT reimplemented here: the settings are
// converted into a roadside::Line and walked by roadside::generate, so a prefab
// lands exactly where the same numbers would have put a model.
namespace roadprefab {

// What the panel edits and main keeps. Metric, like a roadside::Line.
struct Settings {
    std::string path;              // .fprefab file ("" = nothing picked yet)
    std::string name;              // its display name, for the picker's preview
    bool  onRoad  = true;          // on the carriageway (offset from the middle)
                                   // vs. beside it (offset beyond the edge)
    int   side    = roadside::Side::Right;
    float offset  = 0.0f;          // metres, see onRoad
    float spacing = 20.0f;         // metres between instances
    float yaw     = 0.0f;          // extra turn, degrees (0 = along the road)
    float scale   = 1.0f;          // uniform scale applied to the whole subtree
    float lift    = 0.0f;          // metres above the surface it stands on
};

// The placement rule `s` describes. `model` is set to a placeholder because
// roadside::generate skips a line without one -- nothing here resolves a model.
roadside::Line asLine(const Settings& s);

// Scale one instantiated prefab subtree about its root, in place: child offsets,
// box extents and model scales all multiply. Leaves the root's own position
// alone (it is the anchor the instance was placed at).
void scaleInstance(std::vector<Entity>& inst, float scale);

// One prefab instance per entry of `at` -- each carrying the yaw, scale and
// height the Settings asked for, since that is what produced them -- with ids
// minted from `entityCounter`. The
// returned entities are in parent-before-child order (ready for AddEntitiesCmd)
// and every instance ROOT still has parent -1, so the caller can drop them all
// under one group. Each root sits exactly on its placement point -- a prefab has
// no "base of the model" to lift by, its own layout decides that, which is what
// Settings::lift is for.
std::vector<Entity> stamp(const prefab::Prefab& p,
                          const std::vector<roadside::Instance>& at,
                          int& entityCounter);

} // namespace roadprefab
