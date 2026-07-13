#pragma once

#include <functional>
#include <random>
#include <vector>

#include <glm/glm.hpp>

namespace fitzel { class TerrainStreamer; }
class Document;
class ModelLibrary;
struct Entity;

// The editor's "Scatter" panel + placement logic: distribute imported models
// over the terrain -- rocks, props, whole thickets -- with a circular 3D brush
// or along the road. Placements are ordinary Model entities grouped under a
// root Empty ("Scattered"), so they select/move/save like everything else, and
// each stamp is one undoable step. The viewport brush application (mouse
// picking, dabs, cursor ring) stays in main, like the other brushes.
namespace scatterui {

// One entry of the scatter palette: an imported model, toggled on with a pick
// weight (its relative probability within a stamp).
struct Source {
    int   modelId = -1;
    bool  enabled = false;
    float weight  = 1.0f;
};

// Brush + placement parameters (owned by main, edited by the panel).
struct Settings {
    std::vector<Source> sources;   // synced to the model library by the panel

    // Brush.
    float radius     = 8.0f;
    int   perStamp   = 6;          // placement target per stamp
    float minSpacing = 2.0f;       // reject placements closer than this
    float clumping   = 0.0f;       // 0 spread evenly .. 1 huddle in clusters

    // Randomize.
    float scaleMin = 0.8f, scaleMax = 1.3f;
    bool  randomYaw  = true;
    float tiltJitter = 4.0f;       // random lean, degrees
    float alignSlope = 0.0f;       // 0 upright .. 1 fully follow the terrain normal
    float sink       = 0.10f;      // metres embedded into the ground (hides gaps)

    // Placement filters.
    float maxSlope    = 35.0f;     // reject ground steeper than this (degrees)
    float heightMin   = -1000.0f, heightMax = 1000.0f;
    bool  avoidWater  = true;
    bool  addCollider = false;     // give placements a static physics body

    // Roadside mode ("Place along road" button).
    float roadStep   = 8.0f;       // metres between stations along the road
    float roadOffset = 1.5f;       // metres beyond the road edge
    int   roadSide   = 2;          // 0 left, 1 right, 2 both, 3 alternate
};

// Build the entities one brush stamp at world XZ `center` places: weighted
// source pick, dart throwing against `minSpacing` (also vs `occupied`, the XZ
// of already-scattered neighbours) and the slope/height/water filters. The
// returned entities have no id/parent/name-suffix yet -- the caller assigns
// those and pushes one AddEntitiesCmd.
std::vector<Entity> buildStamp(const Settings& s, ModelLibrary& models,
                               const fitzel::TerrainStreamer& streamer,
                               glm::vec2 center, float waterLevel,
                               const std::vector<glm::vec2>& occupied,
                               std::mt19937& rng);

// Same, but along the road: one placement candidate every `roadStep` metres of
// the centreline, offset `roadOffset` beyond the road half-width on the
// configured side(s). Without random yaw, objects face along the road (fences,
// lamp posts). One click populates the whole road.
std::vector<Entity> buildRoadside(const Settings& s, ModelLibrary& models,
                                  const fitzel::TerrainStreamer& streamer,
                                  const std::vector<glm::vec2>& centerline,
                                  float roadHalfWidth, float waterLevel,
                                  const std::vector<glm::vec2>& occupied,
                                  std::mt19937& rng);

// Ids of the scatter group's children within `radius` of `c` (the erase brush).
std::vector<int> collectInBrush(const Document& doc, int groupId,
                                glm::vec2 c, float radius);

struct PanelState {
    bool& show;
    bool& scatterMode;

    // The other viewport brushes -- switched off when scatter grabs the LMB.
    bool& grassPaintMode;
    bool& roadEditMode;
    bool& treePaintMode;
    bool& flowerPaintMode;
    bool& sculptMode;
    bool& paintMode;

    bool&         brushErase;     // stamp vs erase (shared with the other brushes)
    Settings&     cfg;
    ModelLibrary& models;
    int           scatteredCount; // children of the "Scattered" group
    bool          roadAvailable;  // a road centreline exists to place along
    std::function<void()> placeRoadside; // "Place along road" click
    std::function<void()> clearAll;      // undoable delete of every scattered object
};

void drawPanel(const PanelState& s);

} // namespace scatterui
