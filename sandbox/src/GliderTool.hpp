#pragma once

#include <functional>
#include <string>

class Document;
class GliderComponent;
struct Entity;

// UI + one-click setup for the Glider (Wipeout-style hover racer), mirroring
// VehicleTool for the car. The flight integration (arcade hover sim, chase
// camera, G-key test-flight) lives in main, wired off the GliderComponent; this
// module only builds the component and renders its inspector / panel section.
namespace gliderui {

// Add a GliderComponent to `rootId` if missing and seed sensible defaults from
// the entity's bounding box (a ride height that clears the body). Safe to re-run.
// Returns a short human-readable report.
std::string autoSetup(Document& doc, int rootId);

// Inspector body for a GliderComponent on `root`: the tuning properties, grouped
// (Flight / Hover / Attitude / Follow camera), plus the drive hint.
void inspector(GliderComponent& gc, Entity& root, Document& doc);

// The "Scene gliders" section of the Glider panel: lists every glider entity and
// runs `makeGlider(entityId)` (autoSetup wrapped undoably by main) on the current
// selection. `selectedId` is the selected entity's id (-1 none). Returns an
// entity id the user clicked to select, or -1.
int panelSection(Document& doc, int selectedId,
                 const std::function<std::string(int)>& makeGlider);

} // namespace gliderui
