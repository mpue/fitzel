#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Terrain.hpp>

namespace fitzel { class AssetDatabase; }

// The most terrain texture layers the lit shader blends (units 3..3+N-1).
// 6, not 8: the lit shader also declares shadow/env/material samplers, and macOS
// (Metal) caps a program at 16 active fragment samplers. 6 layers keeps the
// total at exactly 16.
inline constexpr int kMaxTerrainLayers = 6;

// One terrain texture layer: its albedo texture and the height + slope band it
// covers. The shader blends every layer whose band contains a fragment's world
// height and surface slope, so overlapping bands cross-fade.
struct TerrainLayer {
    fitzel::AssetId                  texId;   // texture asset GUID (save/load)
    std::shared_ptr<fitzel::Texture> tex;     // resolved albedo (bound per frame)
    std::string                      name;
    float heightStart = -1000.0f;  // world Y where the layer begins
    float heightEnd   =  1000.0f;  // ..and ends
    float slopeStart  =  0.0f;      // surface slope in degrees (0 flat .. 90 vertical)
    float slopeEnd    =  90.0f;
    float scale       =  0.08f;     // triplanar texture scale (world units)
};

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
    std::vector<TerrainLayer> layers; // texture layers (empty -> flat base colour)
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
    fitzel::AssetDatabase&   assetDb;      // for the per-layer texture picker
};

void drawPanel(const PanelState& s);

} // namespace terrainui
