#pragma once

#include <functional>

namespace fitzel { class Camera; class Renderer; class TerrainStreamer; }

// The two small windows about the view itself: "Stats" (frame rate, what is
// loaded and drawn, and the dials that trade one for the other -- move speed,
// view distance, the far plane) and "Camera" (first-person walk, the lens and
// the look angles). Editor only; each draws nothing while its `show` is false.
namespace viewui {

struct StatsState {
    bool& show;
    int&  scene;                          // 0 Nature, 1 Empty (build)
    std::function<void(int)> applyScene;  // ...switched to
    fitzel::Camera&          camera;
    fitzel::TerrainStreamer& streamer;
    fitzel::Renderer&        renderer;
    int   entityCount   = 0;
    int   selectedCount = 0;
    int&   viewRadius;                    // streamed terrain, chunks
    bool&  farPlaneAuto;
    float& farPlaneManual;                // metres, when auto is off
    bool&  resetLayout;                   // "Reset layout" asks for the default docks
};
void drawStats(const StatsState& s);

struct CameraState {
    bool&            show;
    fitzel::Camera&  camera;
    bool&            fpsMode;                 // first-person walk (Shift+F)
    std::function<void(bool)> setFirstPerson; // ...entered or left from the checkbox
};
void drawCamera(const CameraState& s);

} // namespace viewui
