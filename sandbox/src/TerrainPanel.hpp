#pragma once

#include <glm/glm.hpp>

#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Terrain.hpp>

// Slope/height-driven terrain palette, fed to the lit shader as material params.
// Owned by main (as `look`); the shared definition lives here so the terrain
// editor panel can be compiled in its own translation unit.
struct TerrainLook {
    glm::vec3 sand{0.76f, 0.70f, 0.48f};
    glm::vec3 grass{0.23f, 0.42f, 0.16f};
    glm::vec3 rock{0.38f, 0.34f, 0.30f};
    glm::vec3 snow{0.92f, 0.94f, 0.98f};
    float snowLevel      = 16.0f;
    float rockSlope      = 0.62f; // flatter than this -> rock
    float slopeSharpness = 0.14f;
    float detailScale    = 0.35f; // micro-detail frequency
    float detailStrength = 1.5f;  // normal-perturbation strength
};

// The editor's "Terrain" panel: terrain-generator parameters + the slope-material
// look. It operates on state owned by main (the streamer, camera, and the
// vegetation/road invalidation flags), threaded in by reference. Draws nothing
// when `show` is false.
namespace terrainui {

struct PanelState {
    bool&                    show;
    fitzel::TerrainSettings& uiSettings;
    fitzel::TerrainStreamer& streamer;
    fitzel::Camera&          camera;
    TerrainLook&             look;
    float&                   texScale;
    float&                   normalStrength;
    bool&                    grassDirty;   // set true to regrow vegetation
    glm::vec2&               treeCenter;   // reset to force a tree regrow
    bool&                    roadDirty;    // set true to re-drape roads
};

void drawPanel(const PanelState& s);

} // namespace terrainui
