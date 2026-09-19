#pragma once

#include <fitzel/physics/Physics.hpp>

#include "SceneTypes.hpp" // Entity, LoadedModel

// The rigid collider an entity gets in Play. One function for the two places
// that make them -- the scene's start and a script's spawn -- which used to be
// two switches that had drifted apart: a spawned ramp came out a box.
//
// A modelled mesh (MeshComponent) collides as the shape that is drawn, not as
// the primitive it started from:
//  - static (mass 0): its own triangles, in world space. Exact, concave
//    included -- an arch can be walked through, an L stands on its foot. A
//    static body is never moved after it is made, so baking the transform in
//    costs nothing.
//  - dynamic: the convex hull of its corners. Jolt cannot simulate a moving
//    triangle soup, and a hull is what an imported model gets as well.
// Where neither can be built (a flat plane has no hull) it falls back to the
// entity type's own shape, fitted to its half-extents.
//
// `lm` is the entity's loaded model, or null; `mass` 0 means static.
fitzel::PhysicsBodyId addEntityBody(fitzel::PhysicsWorld& w, const Entity& e,
                                    float mass, const LoadedModel* lm);
