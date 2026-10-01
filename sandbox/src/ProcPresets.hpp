#pragma once

#include <string>
#include <vector>

#include "ProcGraph.hpp"
#include "SceneTypes.hpp"   // MaterialDef

// Ready-made procedural graphs to start from (the Procedural window's tiles).
// Each is an ordinary graph -- every node of it can be opened, changed,
// deleted -- so a preset is a worked example as much as a shortcut: the ring
// station shows a torus, copies round a hub, panels, a lattice and copies on
// points, all wired up and set to sensible numbers.
namespace procpreset {

struct Preset {
    const char* name;
    const char* tip;
};
const std::vector<Preset>& list();

// Build preset `i` (an index into list()). The materials it dresses its faces
// in are found in `materials` by name and made there when missing -- never
// changed when present, so a station repainted in the Materials panel stays
// repainted.
proc::Graph build(int i, std::vector<MaterialDef>& materials);

} // namespace procpreset