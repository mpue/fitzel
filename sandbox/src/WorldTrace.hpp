#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "PathTrace.hpp"

class CloudShadow;
class FarTerrain;
class Motes;
class ParticleSystem;
class RainRenderer;
class RiverSystem;
class SpraySystem;
class VegetationSystem;
class Wildlife;
namespace fitzel { struct TerrainSettings; }

// Everything the viewport draws that the render queue does not carry, put into
// a harvested scene so the path tracer renders it too.
//
// pathcapture::capture() reads Renderer::submissions(), and that is most of a
// scene -- terrain, roads, bridges, splines, the city, every model -- but not
// all of it. The forest, the flowers, the water, the far mountains, the birds,
// the particles, the rain and the sky are each drawn by their own system with
// their own shader, and the geometry of most of them exists only inside that
// shader: a tree is five floats and a vertex shader, a lake is a displaced grid,
// the sky is a ray march. None of that can be read back as triangles.
//
// So it is rebuilt here, on the CPU, from the SAME data the shaders are fed --
// the forest field's placements, the tree species' own LOD meshes, the river
// runs, the creature instances of this frame -- and transformed the way the
// shader transforms it. The rule GrassTrace set: regenerate from the source,
// never write a second placement that merely looks like the first.
//
// What does not survive the trip, said here once rather than rediscovered:
//   * wind: every sway, bend and flutter is posed at rest (the grass is the
//     exception -- GrassTrace poses it at the still's instant);
//   * the lake's waves: the surface is flat, a true mirror where the viewport
//     ripples;
//   * volumetric fog volumes, and the impostors' baked lighting (the far forest
//     is traced as the real meshes instead, which is better, not worse).
//
// GL lives here (texture readbacks, the sky capture) exactly as it does in
// PathTraceCapture; the tracer itself stays GL-free.
namespace worldtrace {

// What to put in, and how far out. Radii are from the camera.
struct Options {
    bool  trees      = true;
    float treeRadius = 1600.0f;   // the forest's own edge by default
    bool  flowers    = true;
    float flowerRadius = 60.0f;
    bool  water      = true;      // the lake and the rivers
    bool  farTerrain = true;
    float farRadius  = 16000.0f;  // the far rings' reach
    bool  creatures  = true;      // birds, butterflies, fish
    bool  particles  = true;      // emitters, spray, pollen
    bool  rain       = true;
    bool  clouds     = true;      // their shadow on the ground
    bool  sky        = true;      // the viewport's sky as what the camera sees
};

// The systems to read from, each optional: a null pointer is a system the
// scene does not have (or a harness that did not build one).
struct Sources {
    const VegetationSystem* veg       = nullptr;
    const RiverSystem*      rivers    = nullptr;
    const Wildlife*         wildlife  = nullptr;
    const ParticleSystem*   particles = nullptr;
    const SpraySystem*      spray     = nullptr;
    const Motes*            motes     = nullptr;
    float                   motesRadius = 18.0f;
    const RainRenderer*     rain      = nullptr;
    const CloudShadow*      clouds    = nullptr;
    const fitzel::TerrainSettings* terrain = nullptr;

    // The near terrain the streamer draws (FarTerrain::nearRect): the far
    // rings leave a hole there, as they do in the viewport.
    bool      farTerrainOn = false;
    glm::vec2 nearMin{0.0f}, nearMax{0.0f};
    // ...and its look (snow line, tree line, canopy, the ecology), which the
    // traced far ground is coloured by exactly as farterrain.frag colours it.
    const FarTerrain* farLook = nullptr;
    // The moisture the meadow's colour follows (FarTerrain::fineTexture, an
    // RG32F grid with moisture in G, and fineRect: origin x/z, cell, samples).
    // 0: none -- the meadow keeps its one fixed lushness.
    unsigned  moistTex = 0;
    glm::vec4 moistRect{0.0f};

    // The lake: waterLevel <= -999 means the scene has none.
    float     waterLevel   = -1000.0f;
    glm::vec3 waterColor{0.08f, 0.24f, 0.30f};   // sRGB
    float     waterClarity = 1.0f;
    float     waterIor     = 1.33f;

    // The moment the still is taken, and the light the unlit things take their
    // colour from (the rain, the pollen).
    float     time    = 0.0f;
    float     weather = 0.0f;
    glm::vec3 ambient{0.2f};
    glm::vec3 sunColor{1.0f};
    glm::vec3 sunDir{0.0f, 1.0f, 0.0f};

    // Draws the viewport's background (the sky shader, or the HDRI skybox) for
    // an inverse view-projection and an eye, untonemapped. Empty: no backdrop.
    std::function<void(const glm::mat4& invViewProj, const glm::vec3& eye)> drawSky;
    // The background IS the panorama the scene is lit by, which the tracer
    // already has at full resolution -- capturing it again would only blur it.
    bool skyIsLightingHdri = false;
};

// Add the world to `scene`. `camRight`/`camUp` face the particles and the
// rain the way the viewport's billboards face; `fovDeg` sizes what the viewport
// draws at a fixed size in PIXELS (spray, rain, pollen). `preview` cuts the
// radii for the live viewport preview, which re-harvests on every edit.
// Returns a line per thing added and per thing left out.
void append(pathtrace::Scene& scene, const Sources& src, const Options& opt,
            const glm::vec3& eye, const glm::vec3& camRight, const glm::vec3& camUp,
            float fovDeg, bool preview, std::vector<std::string>& notes);

} // namespace worldtrace
