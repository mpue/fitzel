#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Terrain.hpp>

namespace fitzel { class AssetDatabase; }

// The most terrain texture layers the lit shader blends (see terrainLayerUnit).
// 6, not 8: the lit shader also declares shadow/env/material samplers, and macOS
// (Metal) caps a program at 16 active fragment samplers. 6 layers keeps the
// total at exactly 16.
inline constexpr int kMaxTerrainLayers = 6;

// Which texture unit layer `i` binds its albedo to. NOT 3 + i: the renderer
// binds the shadow cascade array on unit 7 for EVERY draw, terrain included, so
// the run steps over it -- 3, 4, 5, 6, 8, 9. Putting a sampler2D there landed it
// on top of a sampler2DArray, and the ground lost either that layer or its
// shadows, depending on what the driver made of the clash. It took five
// TEXTURED layers to reach, which is why it sat unnoticed; Renderer.hpp has
// described the layout with the gap in it ("3-6, 8-11") all along -- only the
// binding never skipped. main.cpp static_asserts the gap against the renderer's
// own constant, so moving that constant cannot quietly reopen this.
inline constexpr int terrainLayerUnit(int layer) {
    constexpr int kFirstUnit = 3, kCascadeArrayUnit = 7;
    const int unit = kFirstUnit + layer;
    return unit < kCascadeArrayUnit ? unit : unit + 1;
}
// ...and its normal map, kept high so it clears the shadow/env/IBL samplers.
inline constexpr int terrainLayerNormUnit(int layer) { return 18 + layer; }

// One terrain texture layer: its albedo texture and the height + slope band it
// covers. The shader blends every layer whose band contains a fragment's world
// height and surface slope, so overlapping bands cross-fade.
struct TerrainLayer {
    fitzel::AssetId                  texId;   // albedo texture asset GUID (save/load)
    std::shared_ptr<fitzel::Texture> tex;     // resolved albedo (bound per frame)
    fitzel::AssetId                  normId;  // optional normal-map asset GUID
    std::shared_ptr<fitzel::Texture> norm;    // resolved normal map (bound per frame)
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
    float gloss          = 0.05f; // sun-specular strength (0 = matte)
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
    // Resolve a texture asset id to a small preview GL id (0 until decoded). Backed
    // by the shared thumbnail cache in main; used to preview the layer textures.
    std::function<unsigned(fitzel::AssetId)> thumbFor;

    // The two hand-made layers on top of the generated ground, so "Reset terrain"
    // can actually put the world back rather than only the sliders. Neither is on
    // the undo stack -- sculpting and painting write their fields directly -- which
    // is why the reset asks first.
    fitzel::TerrainEditField&  sculpt;        // sculpted height deltas
    std::function<void()>      publishSculpt; // republish the snapshot after clearing
    fitzel::TerrainPaintField& paint;         // painted layer weights
    std::function<void()>      publishPaint;

    // The terrain is an entity now (a Terrain component), so a scene may have
    // none -- and then this panel has nothing to edit. `hasTerrain` says whether
    // the scene has ground; `addTerrain` puts one in (undoable), which is the
    // only thing the panel offers while there is none.
    bool                       hasTerrain = true;
    std::function<void()>      addTerrain;
};

void drawPanel(const PanelState& s);

} // namespace terrainui
