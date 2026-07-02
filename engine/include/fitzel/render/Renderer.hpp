#pragma once

#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "fitzel/graphics/Shader.hpp"
#include "fitzel/graphics/CascadedShadowMap.hpp"
#include "fitzel/graphics/CubeShadowMap.hpp"
#include "fitzel/graphics/CubeRenderTarget.hpp"

namespace fitzel {

class Camera;
class Mesh;
class Material;
class EnvironmentIBL;

struct DirectionalLight {
    glm::vec3 direction{0.5f, 1.0f, 0.35f}; // points *towards* the light
    glm::vec3 color{1.0f, 0.97f, 0.9f};
    glm::vec3 ambient{0.30f, 0.33f, 0.38f}; // sky/fill light
};

// A world-space point light with distance falloff. `color` is HDR radiance
// (already scaled by intensity). Fed to lit-shader surfaces (terrain, roads,
// entities); vegetation shaders are unaffected for now.
struct PointLight {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float     range = 12.0f;
    bool      castShadows = false; // opt-in omnidirectional shadow
    float     shadowBias  = 0.003f; // normalized depth bias for the shadow cube
};

// Atmospheric fog: exponential height fog + aerial perspective. `color` is the
// distance haze (match it to the sky horizon); `sunColor` tints the in-scatter
// when looking toward the sun.
struct Fog {
    glm::vec3 color{0.70f, 0.82f, 0.95f};
    glm::vec3 sunColor{1.0f, 0.75f, 0.5f};
    float     density       = 0.006f;
    float     heightFalloff = 0.03f;
    float     height        = 0.0f;
};

// A high-level renderer that drives cascaded shadow mapping and a forward lit
// pass. The app submits (mesh, material, model) tuples between begin() and
// end(); the renderer renders all cascades, then the lit scene, feeding the
// shadow uniforms to each material's shader automatically.
//
// Materials' shaders are expected to declare the shadow/lighting uniforms the
// renderer sets (see sandbox/assets/shaders/lit.frag for the contract).
class Renderer {
public:
    // The texture unit the cascade depth array is bound to. Materials must not
    // use this unit for their own textures.
    static constexpr int kShadowMapUnit = 7;
    // Texture units 12..15 hold the point-light shadow cubemaps (units 3-6,8-11
    // are terrain textures, 7 is the cascade array).
    static constexpr int kPointShadowUnit  = 12;
    static constexpr int kMaxShadowedPoints = 4;
    // The dynamic environment-probe cubemap for reflective materials. Always
    // bound (unit 2) so its samplerCube never aliases a 2D sampler's unit.
    static constexpr int kEnvProbeUnit = 2;
    // Image-based-lighting cubemaps from an HDRI (units 16/17; desktop GL gives
    // >=32 image units). Always bound (probe cube as a fallback) to avoid unit-0
    // aliasing; the shader only uses them when uUseIBL == 1.
    static constexpr int kIrradianceUnit = 16;
    static constexpr int kPrefilterUnit  = 17;

    explicit Renderer(int shadowResolution = 2048, int cascades = 4);

    void setViewport(int width, int height);

    static constexpr int kMaxPointLights = 16;

    void begin(const Camera& camera, float aspect, const DirectionalLight& light);
    void setFog(const Fog& fog) { m_fog = fog; }
    void setExposure(float exposure) { m_exposure = exposure; }
    // Image-based lighting from an HDRI. Pass a valid, loaded EnvironmentIBL and
    // enabled = true to light lit-shader surfaces from it (replacing the flat
    // ambient); pass nullptr/false to fall back to the directional ambient.
    void setEnvironmentIBL(const EnvironmentIBL* ibl, bool enabled, float intensity) {
        m_ibl = ibl; m_iblEnabled = enabled; m_iblIntensity = intensity;
    }
    // Point lights for this frame (applied to lit-shader surfaces in every pass).
    void setPointLights(const std::vector<PointLight>& lights) { m_pointLights = lights; }
    // Render omnidirectional shadow cubemaps for the shadow-casting point lights
    // (up to kMaxShadowedPoints). Call after submit() and before renderScene().
    void preparePointShadows();
    // `castsPointShadow` false keeps a mesh out of the point-light shadow cubes
    // (e.g. the ground, which should receive but not cast omni shadows).
    // `reflective` true marks a mesh as an environment-probe surface: it is
    // excluded from the probe render (so it doesn't reflect its own interior).
    void submit(const Mesh& mesh, const Material& material, const glm::mat4& model,
                bool castsPointShadow = true, bool reflective = false);

    // Render the scene (opaque queue minus reflective surfaces + the sky drawn
    // by `drawSky`) into the environment-probe cubemap from `pos`. Call after
    // submit()/prepareShadows() and before the lit passes so those passes sample
    // a fresh probe. The probe is bound automatically in renderScene().
    using SkyDrawer = std::function<void(const glm::mat4& invViewProj,
                                         const glm::vec3& eye)>;
    void prepareEnvProbe(const glm::vec3& pos, const SkyDrawer& drawSky);

    void end(); // convenience: prepareShadows() + one lit pass from the camera

    // Multi-pass building blocks (for reflection/refraction etc.):
    // render the shadow cascades once from the stored camera + light. The
    // optional callback is invoked per cascade with that cascade's light-space
    // matrix, so the app can add its own casters (e.g. instanced trees).
    using ShadowCaster = std::function<void(const glm::mat4& lightSpace)>;
    void prepareShadows(const ShadowCaster& extra = {});
    // Render the submitted opaque queue with an explicit view/projection,
    // eye position and world-space clip plane into the currently bound target
    // (does not clear). Pass kNoClip to disable clipping.
    void renderScene(const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& eye, const glm::vec4& clipPlane,
                     bool tonemap = true, bool skipReflective = false);

    // A clip plane that keeps every fragment (effectively no clipping).
    static const glm::vec4 kNoClip;

    CascadedShadowMap&       shadows()       { return m_csm; }
    const CascadedShadowMap& shadows() const { return m_csm; }

    // Frustum-culling stats from the most recent renderScene() call.
    int lastDrawn()  const { return m_lastDrawn; }
    int lastCulled() const { return m_lastCulled; }

private:
    struct Renderable {
        const Mesh*     mesh;
        const Material* material;
        glm::mat4       model;
        bool            castsPointShadow;
        bool            reflective;
    };

    CascadedShadowMap m_csm;
    Shader            m_depthShader;
    Shader            m_cubeDistShader;             // point-shadow distance pass
    std::vector<CubeShadowMap> m_pointShadows;      // one per shadowed point light
    int               m_shadowedCount = 0;
    // Environment probe, ping-ponged: lit passes sample m_envRead (last frame's
    // capture) while prepareEnvProbe() renders into m_envWrite, then they swap.
    // Double-buffering avoids sampling the cubemap that is the current target.
    CubeRenderTarget  m_envA{128};
    CubeRenderTarget  m_envB{128};
    CubeRenderTarget* m_envRead  = &m_envA;
    CubeRenderTarget* m_envWrite = &m_envB;
    std::vector<Renderable> m_queue;

    const Camera*    m_camera = nullptr;
    float            m_aspect = 1.0f;
    DirectionalLight m_light;
    std::vector<PointLight> m_pointLights;
    Fog              m_fog;
    const EnvironmentIBL* m_ibl = nullptr;
    bool             m_iblEnabled  = false;
    float            m_iblIntensity = 1.0f;
    float            m_exposure = 1.0f;
    int              m_vpWidth   = 1;
    int              m_vpHeight  = 1;
    int              m_lastDrawn  = 0;
    int              m_lastCulled = 0;
};

} // namespace fitzel
