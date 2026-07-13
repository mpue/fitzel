#pragma once

#include <functional>

#include <fitzel/world/Terrain.hpp>

#include "TerrainPanel.hpp" // TerrainLook / TerrainLayer (paint targets the layers)

// The editor's "Terrain Paint" panel: pick one of the terrain texture layers and
// brush it onto the ground, overriding the automatic height/slope blend. Paint is
// stored in a TerrainPaintField owned by main and baked into the terrain vertices;
// the viewport brush application (mouse picking, dabs, cursor ring) stays in main.
// Only the first four textured layers are paintable (one vertex vec4 of weights).
namespace paintui {

struct PanelState {
    bool& show;
    bool& paintMode;

    // Other viewport brushes -- switched off when paint grabs the left button.
    bool& grassPaintMode;
    bool& roadEditMode;
    bool& treePaintMode;
    bool& flowerPaintMode;
    bool& sculptMode;
    bool& scatterMode;

    const TerrainLook& look;   // layer list, for naming the paint targets
    int&   layer;              // paint slot (0..3) = which textured layer to paint
    float& radius;
    float& strength;
    bool&  erase;              // paint vs revert-to-auto

    fitzel::TerrainPaintField& work;
    fitzel::TerrainStreamer&   streamer;
    std::function<void()>      publish; // snapshot `work` as the live paint layer
};

void drawPanel(const PanelState& s);

} // namespace paintui
