#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "PostLook.hpp"
#include "VolumetricFog.hpp"
#include "WeatherPreset.hpp"

namespace fitzel { class AssetDatabase; class EnvironmentIBL; class Renderer; }

// The two panels that set how the picture looks: "Sky & atmosphere" (time of
// day, the cloud deck and its layers, the height haze and the world's
// volumetric fog, then the post chain -- exposure and tonemap, bloom, rays, AO,
// shadows, reflections, depth of field, motion blur, anti-aliasing, split
// screen) and "Colour grade". Out of main() with the settings they edit.
// Editor only; each draws nothing while its `show` is false.
namespace lookui {

struct SkyPanelState {
    bool&  show;
    float& timeOfDay;
    bool&  timePaused;
    float& dayLength;
    weather::Sky&            sky;
    VolumetricFog::Settings& volFog;
    PostLook&                look;
    bool&                    splitScreen;
    fitzel::Renderer&        renderer;          // cascade split, the probe's cubes
    float                    autoExposureScale; // what auto exposure is doing now (PostChain)
    glm::vec3                cameraPos;         // "Centre on camera"
    std::function<float(float, float)> groundAt; // "Sit on ground"
};

void drawSkyPanel(const SkyPanelState& s);
void drawGradePanel(bool& show, PostLook& look);

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
