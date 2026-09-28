#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <fitzel/physics/Physics.hpp>

#include "SceneTypes.hpp"   // Entity, LoadedModel

class RoadSet;
class CitySystem;
class SplineSystem;
class ModelLibrary;

// What Play builds its physics world from, besides the terrain's heightfield
// (which follows the player, see main's refitTerrainCollision) and the
// character: the scene's static world, and the entities' own bodies. All of it
// dies with the world when Play stops. Runtime: the player builds it the same way.
namespace playworld {

// A knockable road side object (post, bollard): a dynamic body made at Play
// start and drawn from its live transform, so a car bowls it over. Holds what
// drawing needs: the body, which model, at what scale. Rebuilt every Play; the
// road's derived static instances take over again in the editor.
struct SidePost {
    fitzel::PhysicsBodyId body;
    int                   modelId;
    float                 scale;
};

// The scene's static world as colliders:
//  - every enabled road: its graded triangle mesh, a box per side object
//    (rails and kerbs static, knockable lines dynamic -- those go into `posts`)
//    and a static box per solid piece of its roadside city;
//  - the towns' buildings, a static box per solid part;
//  - fences, walls, track and bridges, a static box per short run of path.
// A hidden road is not there to be driven on either. `resolveModel` turns a
// side batch's model reference into its loaded model (null: skipped).
void addStaticWorld(fitzel::PhysicsWorld& physics, const RoadSet& roads,
                    const CitySystem& towns, const SplineSystem& splines,
                    const std::function<LoadedModel*(const std::string&)>& resolveModel,
                    std::vector<SidePost>& posts);

// The entities' rigid bodies: every active entity with a PhysicsComponent,
// except the ones something else moves -- opponents (kinematic along the road),
// the towns' traffic drivers (their own driven box) and soft bodies (their
// particles are their physics). `bodies` maps entity id -> body.
void addEntityBodies(fitzel::PhysicsWorld& physics, const std::vector<Entity>& entities,
                     ModelLibrary& models, std::map<int, fitzel::PhysicsBodyId>& bodies);

} // namespace playworld
