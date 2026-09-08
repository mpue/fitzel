#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct Entity;
class RoadSystem;
class FinishLineComponent;

// The starting grid: what kind of session a scene is, who is in it, and where
// everyone stands when the lights go out.
//
// A scene says which it is on its START/FINISH LINE, because that is the object
// which defines the race -- there is one per circuit, it already carries the lap
// count and the Ready/Set/Go samples, and a race without a line is not a race.
// The alternative, a scene-wide setting somewhere in the project file, would put
// the lap count and the mode in two different places.
//
// Placement moves the ENTITIES rather than computing positions at Play. That is
// deliberate: an opponent seeds its race distance by projecting where it stands
// onto the road, so moving the craft IS the placement, and the same call serves
// the editor's preview button and the start of a race. What you line up is what
// you get.
namespace racegrid {

// Which session a scene holds.
enum Mode { Race = 0, TimeTrial = 1 };

// What the grid needs of a road: a line to measure along, the heights that go
// with it, and whether it closes.
//
// Taken as DATA rather than as a RoadSystem because a RoadSystem is a lot of
// engine -- shaders, materials, textures, a GL context -- and none of it says
// anything about where a craft stands. Handing over the centreline instead is
// what lets the grid be measured in a harness with no window open (gridcheck),
// and it is the honest description of the dependency: the grid needs a line.
// An empty one is a scene with no road, which authored markers do not need.
struct Track {
    const std::vector<glm::vec2>* line   = nullptr;
    const std::vector<float>*     y      = nullptr;
    bool                          closed = false;
};

// Line the field up behind the start/finish line.
//
// `playerCraftId` is the entity the player will fly (-1 if unknown), so it gets a
// slot of its own; every opponent marked as entered takes the others. With
// `applyParticipation` the craft that are not in this session are deactivated --
// that is for Play, which snapshots the scene and puts it back afterwards. The
// editor's preview passes false and only moves things, so a look at the grid
// cannot quietly deactivate half the scene and then save it.
//
// `playerCraftId2` is split screen's second player (-1 when there is only one).
// It gets the slot NEXT to player one rather than a place in the field: two
// people at one machine start beside each other or the pane that shows the
// slower one is a picture of an empty track. A craft flown by player two is not
// an AI racer even if it carries an Opponent component -- it is taken out of the
// field here and left active, whatever its tick says.
//
// Returns the number of craft placed, or 0 when the scene has no start/finish
// line or no built road to measure along (there is nothing to line up against).
int lineUp(std::vector<Entity>& entities, const Track& track,
           int playerCraftId, bool applyParticipation, int playerCraftId2 = -1);

// The same, measured along a built road -- what the game and the editor call.
int lineUp(std::vector<Entity>& entities, const RoadSystem& road,
           int playerCraftId, bool applyParticipation, int playerCraftId2 = -1);

// Build the field the grid asks for: every marker that names a prefab and is
// needed for THIS race makes its own rival (see GridPositionComponent).
//
// This is what turns the start screen's field size into a number the circuit
// obeys rather than a ceiling on what its author happened to park there. It runs
// only in Play, because it puts craft into the scene and it is Play's snapshot
// that takes them out again afterwards.
//
// `rivalsWanted` is the start screen's count, -1 for "fill the grid". Rivals the
// scene already holds are counted first, so a circuit with two hand-placed craft
// asked for five gets three built.
//
// `playerCraftId` is the craft the player will fly, or -1. It takes a marker of
// its own -- the one that reserves it, else one DRAWN at random -- and that
// marker builds nothing. The draw is pinned onto the marker as it happens, so
// the line-up that follows puts the player exactly where the draw said.
//
// `spawn(prefabName, position, yawDegrees)` makes the instance and returns its
// root entity id, or <= 0 if it could not. It may add to `entities` (that is the
// point), so nothing here holds an Entity* across the call.
//
// Returns how many craft were built.
int populate(std::vector<Entity>& entities, int rivalsWanted, int playerCraftId,
             const std::function<int(const std::string&, const glm::vec3&, float)>& spawn,
             int playerCraftId2 = -1);

// The grid marker that reserves the player's slot ("Player starts here"), or -1
// when no marker does.
//
// It is the one slot the grid does NOT build a rival on, and therefore the one
// place the player's own craft belongs -- which is what lets a circuit be played
// without going through the start screen that would normally have brought one.
int playerMarker(const std::vector<Entity>& entities);

// The prefab that marker names, or "" (no marker, or one that names nothing).
std::string playerMarkerPrefab(const std::vector<Entity>& entities);

// Is this scene a race (as opposed to a time trial)? False when it has no
// start/finish line at all.
bool isRace(const std::vector<Entity>& entities);

#ifndef FITZEL_PLAYER
// The Start/Finish inspector: the mode switch, the grid tunables, and the roster
// of the scene's opponents with a tick each. Returns true when something changed.
// Editor-only, same split as roadbridge::panel.
bool inspector(FinishLineComponent& fl, std::vector<Entity>& entities,
               const RoadSystem& road);
#endif

} // namespace racegrid
