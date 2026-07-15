#pragma once

#include <glm/glm.hpp>

#include <fitzel/render/Renderer.hpp> // DirectionalLight, Fog

// Shared vocabulary for the scene layers that draw themselves.
//
// The render loop draws the world several times per frame -- once per env-probe
// face, once reflected in the water, once for real -- and each pass differs only in
// where the camera is. Everything else (sun, fog, weather, clock) is the same. This
// header is what lets a layer be handed that difference instead of reaching back
// into main()'s locals for it.

// Everything a scene layer needs to know about the frame it is drawing into.
// Generalizes what the vegetation has always taken; the name is neutral because
// rain and spray want the same thing and shouldn't have to say "Veg" to get it.
struct FrameContext {
    glm::mat4 viewProj{1.0f};
    glm::vec3 camPos{0.0f};
    double    time    = 0.0;  // seconds since start -- the shader animation clock
    float     weather = 0.0f; // 0 = clear .. 1 = full storm
    glm::vec3 lightDir{0.0f}, lightColor{0.0f}, ambient{0.0f};
    glm::vec3 fogColor{0.0f}, fogSunColor{0.0f};
    float     fogDensity = 0.0f, fogHeightFalloff = 0.0f, fogHeight = 0.0f;
};

// Build the context for one pass. `viewProj` and `camPos` are the pass's own (the
// reflected view for the water mirror, the real one for the main pass); the rest is
// the frame's and is the same for every pass.
//
// One place that knows how a context is assembled, so a pass can't quietly disagree
// with the others about the sun -- which is exactly what two hand-built copies drift
// into.
inline FrameContext makeFrameContext(const glm::mat4& viewProj, const glm::vec3& camPos,
                                     double time, float weather,
                                     const fitzel::DirectionalLight& light,
                                     const fitzel::Fog& fog) {
    FrameContext c;
    c.viewProj         = viewProj;
    c.camPos           = camPos;
    c.time             = time;
    c.weather          = weather;
    c.lightDir         = light.direction;
    c.lightColor       = light.color;
    c.ambient          = light.ambient;
    c.fogColor         = fog.color;
    c.fogSunColor      = fog.sunColor;
    c.fogDensity       = fog.density;
    c.fogHeightFalloff = fog.heightFalloff;
    c.fogHeight        = fog.height;
    return c;
}

// A scene layer that draws itself with its own shader into whatever target is
// currently bound, and reads *only* `ctx`. That fence is the point: it makes
// reaching back into main()'s locals impossible rather than merely discouraged, and
// it is what lets the same layer serve the main pass, the water mirror and a probe
// face without knowing which one it is in.
//
// This is a contract, not a dispatch mechanism -- there is no list to iterate. The
// frame's order (probe -> reflection -> main -> grass -> water -> rain -> spray ->
// post) is fixed by what each pass needs from the one before it, so the render loop
// calls these by name. What the base buys is that `override` catches a layer whose
// signature drifts, and that "renders itself" is a stated kind rather than a habit.
//
// Deliberately NOT implemented by:
//  - subsystems that submit meshes to fitzel::Renderer (roads, skids, entities,
//    cars): submit() is already their abstraction, and it hands them the shared lit
//    pass, shadows, frustum culling and transparency sorting for free.
//  - the sky, drawn several times a frame with a different view and tonemap flag.
//  - the post chain (SSAO/composite/FXAA): an ordered pipeline over render targets,
//    not a layer in a scene.
class RendererBase {
public:
    virtual ~RendererBase() = default;

    // Draw one frame of this layer. The target is already bound. Must leave GL
    // state as it found it -- depth test, depth mask, blending, cull face.
    virtual void draw(const FrameContext& ctx) = 0;
};
