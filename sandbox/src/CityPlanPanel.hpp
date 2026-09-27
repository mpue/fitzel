#pragma once

#include <functional>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include "CitySystem.hpp"

class Command;
class RoadSet;

// The editor's "Town generator" panel: the UI over CitySystem. Pick a preset,
// put the town where the 3D cursor is, press "Lay streets" -- the grid goes into
// the road set, is built into the terrain, and the blocks fill with buildings.
// Everything after that (zoning, heights, colours) re-plans the buildings live.
//
// Only steppers, buttons and a checkbox or two: no value here is dragged (see
// ui::stepper). Every change is one undo step, laying streets included.
//
// The panel owns no state: what it edits lives in CitySystem, the undo bracket
// in main, like every other panel.
namespace citygenui {

struct PanelState {
    bool&        show;
    CitySystem&  cities;
    RoadSet&     roads;
    int&         sel;         // selected town
    glm::vec3    cursor;      // the 3D cursor ("Place at cursor")
    // The undo bracket: the towns as they were when the current edit began, and
    // whether one is in flight. main owns both; the panel resyncs them while idle.
    CitySystem::Snapshot& undoBefore;
    bool&                 editing;
    std::function<void(std::unique_ptr<Command>)> pushApplied;
    std::string&          status;   // last action's outcome
    // The project's prefabs by name, for the traffic's vehicle prefabs.
    std::function<std::vector<std::string>()> prefabNames = {};
};

void drawPanel(const PanelState& s);

} // namespace citygenui
