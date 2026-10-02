#pragma once

#include <functional>
#include <vector>

#include <fitzel/physics/Physics.hpp>

#include "SceneTypes.hpp" // Entity, LoadedModel

// The rigid collider an entity gets in Play. One function for the two places
// that make them -- the scene's start and a script's spawn -- which used to be
// two switches that had drifted apart: a spawned ramp came out a box.
//
// A modelled mesh (MeshComponent) and an imported model collide as the shape
// that is drawn, not as the primitive or the box around it:
//  - static (mass 0): their own triangles. Exact, concave included -- an arch
//    can be walked through, an L stands on its foot, a hall can be walked
//    into. A model's cut-out surfaces (leaves, grass, decals) are left out,
//    and all copies of one model share a single mesh (see LoadedModel::meshTris).
//  - dynamic: the convex hull of their corners. Jolt cannot simulate a moving
//    triangle soup.
// Where neither can be built (a flat plane has no hull, a model that is only
// leaves has no triangles) it falls back to the hull, then to the entity
// type's own shape, fitted to its half-extents.
//
// `lm` is the entity's loaded model, or null; `mass` 0 means static.
fitzel::PhysicsBodyId addEntityBody(fitzel::PhysicsWorld& w, const Entity& e,
                                    float mass, const LoadedModel* lm);

// A model group: the Model entity with no model of its own that a structured
// import puts its parts under.
bool isModelGroup(const Entity& e);

// Static Physics on a model group: every model part below it collides as its
// own triangles, so a whole building is solid with one component instead of
// one per wall. A part with a Physics component of its own keeps it (a barrel
// in the hall can still roll), and so does everything below that part; a part
// that is only leaves or decals is left out. `modelOf` answers a part's loaded
// model. Returns how many bodies were made.
int addGroupBodies(fitzel::PhysicsWorld& w, const std::vector<Entity>& entities,
                   const Entity& root,
                   const std::function<const LoadedModel*(const Entity&)>& modelOf);
