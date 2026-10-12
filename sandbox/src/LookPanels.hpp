#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "PostLook.hpp"
#include "VolumetricFog.hpp"
#include "WeatherPreset.hpp"

namespace fitzel { class AssetDatabase; class EnvironmentIBL; class Renderer; }

// The panels that set how the picture looks: "Sky & atmosphere" (time of day,
// the cloud deck and its layers, the height haze and the world's volumetric
// fog), "Post processing" (the post chain, one section per effect, each with
// its own switch) and "Shadows & reflections" (the renderer's cascade split,
// contact shadows, SSR and the reflection probe). Out of main() with the
// settings they edit. Editor only; each draws nothing while its `show` is false.
namespace lookui {

struct SkyPanelState {
    bool&  show;
    float& timeOfDay;
    bool&  timePaused;
    float& dayLength;
    weather::Sky&            sky;
    VolumetricFog::Settings& volFog;
    glm::vec3                cameraPos;         // "Centre on camera"
    std::function<float(float, float)> groundAt; // "Sit on ground"
};
void drawSkyPanel(const SkyPanelState& s);

struct PostPanelState {
    bool&     show;
    PostLook& look;
    bool&     postFx;            // the viewport's Post FX switch (Toolbar)
    bool      playMode = false;  // Play always shows the look: the switch is off
    float     autoExposureScale = 1.0f; // what auto exposure is doing now (PostChain)
};
void drawPostPanel(const PostPanelState& s);

struct ShadowsPanelState {
    bool&             show;
    PostLook&         look;      // contact shadows, SSR, the probe's size and rate
    fitzel::Renderer& renderer;  // cascade split, the probe's cubes
};
void drawShadowsPanel(const ShadowsPanelState& s);

// "Environment": image-based lighting from an HDRI panorama picked from the
// asset library (.hdr / .exr textures that are not a material's maps).
struct EnvironmentPanelState {
    bool&                   show;
    fitzel::EnvironmentIBL& environment;
    fitzel::AssetDatabase&  assetDb;
    std::string&            hdriLoaded;    // the panorama's library path ("" = none)
    std::string&            hdriAbsPath;   // ...and its file (the offline renderer's)
    bool&                   iblEnabled;
    bool&                   iblSkybox;     // draw it as the sky background
    float&                  iblIntensity;
};
void drawEnvironmentPanel(const EnvironmentPanelState& s);

// "Water": the lake's level, swell, ripples, foam and optics.
struct WaterPanelState {
    bool&      show;
    float&     level;
    float&     waveHeight;     // Gerstner swell amplitude
    float&     waveChoppy;
    float&     rippleStrength;
    float&     rippleScale;
    float&     foamWidth;
    float&     reflectivity;   // max mirror strength (Fresnel cap)
    float&     clarity;        // higher = clearer (less depth tint)
    float&     ior;            // index of refraction (Fresnel + bend)
    glm::vec3& tint;
};
void drawWaterPanel(const WaterPanelState& s);

} // namespace lookui
