#pragma once

#include <functional>

#include <glm/glm.hpp>

#include "PostLook.hpp"
#include "VolumetricFog.hpp"
#include "WeatherPreset.hpp"

namespace fitzel { class Renderer; }

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

} // namespace lookui
