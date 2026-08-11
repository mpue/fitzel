#pragma once

#include <vector>

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

// Line the field up behind the start/finish line.
//
// `playerCraftId` is the entity the player will fly (-1 if unknown), so it gets a
// slot of its own; every opponent marked as entered takes the others. With
// `applyParticipation` the craft that are not in this session are deactivated --
// that is for Play, which snapshots the scene and puts it back afterwards. The
// editor's preview passes false and only moves things, so a look at the grid
// cannot quietly deactivate half the scene and then save it.
//
// Returns the number of craft placed, or 0 when the scene has no start/finish
// line or no built road to measure along (there is nothing to line up against).
int lineUp(std::vector<Entity>& entities, const RoadSystem& road,
           int playerCraftId, bool applyParticipation);

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
