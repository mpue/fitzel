#pragma once

#include <functional>

#include <glm/glm.hpp>

#include <fitzel/world/Terrain.hpp>

// The editor's "Terrain Sculpt" panel: the brush-tool controls for the manual
// deformation layer (raise/lower/smooth/flatten/erode/stamp/noise). It edits the
// live TerrainEditField owned by main; the viewport brush application (mouse
// picking, per-frame dabs, cursor ring) stays in main, since it is wired into the
// camera and input. Draws nothing when `show` is false.
namespace sculptui {

struct PanelState {
    bool& show;
    bool& sculptMode;

    // The other viewport brushes -- switched off when sculpt grabs the left button
    // so only one tool owns the LMB at a time.
    bool& grassPaintMode;
    bool& roadEditMode;
    bool& treePaintMode;
    bool& flowerPaintMode;
    bool& paintMode;

    // Brush parameters (owned by main, shared with the viewport application code).
    int&   tool;          // 0 raise 1 lower 2 smooth 3 flatten 4 erode 5 stamp 6 noise 7 carve
    float& radius;
    float& strength;
    float& flattenHeight; // flatten target (grabbed from the surface on press)
    int&   stampShape;    // 0 dome 1 cone 2 plateau 3 crater 4 ridge 5 range
    float& stampHeight;
    float& stampRot;      // radians (orients ridge / range)
    float& noiseFreq;
    float& carveDepth;    // valley depth for the carve tool (Alt raises a ridge)

    fitzel::TerrainEditField& work;      // live, main-thread-only edit field
    fitzel::TerrainStreamer&  streamer;
    bool&                     grassDirty; // set true to regrow grass after a change
    std::function<void()>     publish;    // snapshot `work` as the live terrain layer
};

void drawPanel(const PanelState& s);

} // namespace sculptui
