#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "fitzel/graphics/Shader.hpp"
#include "fitzel/graphics/CascadedShadowMap.hpp"
#include "fitzel/graphics/CubeShadowMap.hpp"
#include "fitzel/graphics/CubeRenderTarget.hpp"
#include "fitzel/graphics/Texture3D.hpp"

namespace fitzel {

class Camera;
class Mesh;
class Material;
class Texture;
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

// A world-space spot light: a cone shining along `direction`. `color` is HDR
// radiance (already scaled by intensity). Fed to lit-shader surfaces; unshadowed
// (headlights and the like don't need it). cosInner/cosOuter are the cosines of
// the inner (full-bright) and outer (fade-to-zero) cone half-angles.
struct SpotLight {
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f}; // normalized cone axis
    glm::vec3 color{1.0f};
    float     range    = 20.0f;
    float     cosInner = 0.95f;
    float     cosOuter = 0.86f;
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

// A submitted object's bounds in world space. Cached for a frame so the shadow
// passes, which ask about the same object up to twenty-four times (six cube
// faces times four shadowed lights), do not retransform eight corners each time.
struct WorldAabb {
    glm::vec3 lo{0.0f}, hi{0.0f};
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
    // The baked light grid: three volumes, one per colour channel, each holding
    // that channel's four spherical-harmonic coefficients in RGBA. Units 24-26
    // because the terrain's layer normal maps run to 23.
    static constexpr int kLightGridUnit = 24;
    // A copy of the opaque scene, taken between the opaque and the transparent
    // pass, so a refracting surface can look up what is BEHIND it and bend it.
    // Nothing else can: a transparent draw cannot read the target it is writing,
    // and the environment probe knows the sky and the neighbours but not the
    // wall two metres back. Only taken when the frame actually has something
    // refractive in it (see m_sceneCopy).
    static constexpr int kSceneCopyUnit = 27;

    // Cube-face resolution of the dynamic environment probe.
    static constexpr int kDefaultEnvProbeRes = 256;
    static constexpr int kMinEnvProbeRes     = 64;
    static constexpr int kMaxEnvProbeRes     = 1024;

    explicit Renderer(int shadowResolution = 2048, int cascades = 4);

    void setViewport(int width, int height);

    static constexpr int kMaxPointLights = 16;
    static constexpr int kMaxSpotLights  = 8;

    void begin(const Camera& camera, float aspect, const DirectionalLight& light);
    void setFog(const Fog& fog) { m_fog = fog; }
    void setExposure(float exposure) { m_exposure = exposure; }
    // Image-based lighting from an HDRI. Pass a valid, loaded EnvironmentIBL and
    // enabled = true to light lit-shader surfaces from it (replacing the flat
    // ambient); pass nullptr/false to fall back to the directional ambient.
    void setEnvironmentIBL(const EnvironmentIBL* ibl, bool enabled, float intensity) {
        m_ibl = ibl; m_iblEnabled = enabled; m_iblIntensity = intensity;
    }
    // Baked indirect light, sampled by world position (see LightGrid in the
    // sandbox and pathtrace::bakeProbes). Replaces the flat ambient term where
    // it covers: a probe under a bridge has no sky over it and says so, which
    // one ambient colour for the whole world cannot.
    //
    // Three textures rather than one because a colour channel's four
    // coefficients fit an RGBA texel exactly, and that is what lets the hardware
    // interpolate between neighbouring probes correctly -- packing all twelve
    // into one volume would blend across coefficient boundaries.
    //
    // Pass nullptr to switch back to the flat ambient. `lo`/`hi` are the grid's
    // world bounds.
    void setLightGrid(const Texture3D* r, const Texture3D* g, const Texture3D* b,
                      const glm::vec3& lo, const glm::vec3& hi, float intensity) {
        m_gridR = r; m_gridG = g; m_gridB = b;
        m_gridLo = lo; m_gridHi = hi; m_gridIntensity = intensity;
    }
    bool lightGridEnabled() const { return m_gridR && m_gridG && m_gridB; }

    // Point lights for this frame (applied to lit-shader surfaces in every pass).
    void setPointLights(const std::vector<PointLight>& lights) { m_pointLights = lights; }
    // Spot lights for this frame (applied to lit-shader surfaces in every pass).
    void setSpotLights(const std::vector<SpotLight>& lights) { m_spotLights = lights; }
    // Render omnidirectional shadow cubemaps for the shadow-casting point lights
    // (up to kMaxShadowedPoints). Call after submit() and before renderScene().
    void preparePointShadows();
    // `castsPointShadow` false keeps a mesh out of the point-light shadow cubes
    // (e.g. the ground, which should receive but not cast omni shadows).
    // `reflective` true marks a mesh as an environment-probe surface: it is
    // excluded from the probe render (so it doesn't reflect its own interior).
    // `opacity` < 1 marks the mesh transparent: it is drawn after the opaque
    // queue, back-to-front, with alpha blending and depth writes disabled, and
    // the lit shader multiplies its output alpha by it. `forceTransparent` puts
    // the mesh in that same blended queue even when opacity == 1 (for materials
    // whose transparency lives in a texture alpha channel, not the scalar).
    void submit(const Mesh& mesh, const Material& material, const glm::mat4& model,
                bool castsPointShadow = true, bool reflective = false,
                float opacity = 1.0f, bool forceTransparent = false);

    // Render the scene (opaque queue minus reflective surfaces + the sky drawn
    // by `drawSky`) into the environment-probe cubemap from `pos`. Call after
    // submit()/prepareShadows() and before the lit passes so those passes sample
    // a fresh probe. The probe is bound automatically in renderScene().
    //
    // Cost is amortized: one cube face per call, so a sweep takes six frames.
    // The first sweep after construction or a resolution change is done in one
    // go, because until a cube has been filled once there is nothing sensible to
    // sample. `pos` is only read at the start of a sweep -- it usually tracks
    // the camera, and letting it drift mid-sweep would seam the cube.
    using SkyDrawer = std::function<void(const glm::mat4& invViewProj,
                                         const glm::vec3& eye)>;
    void prepareEnvProbe(const glm::vec3& pos, const SkyDrawer& drawSky);

    // Cube-face resolution of that probe. Setting it reallocates both cubes (the
    // old contents are gone -- the next capture fills them, and until then the
    // lit shader's guard treats an unwritten probe as black), so call it when the
    // setting changes, not per frame. Needs a current GL context. Clamped to
    // [kMinEnvProbeRes, kMaxEnvProbeRes] and rounded down to a power of two, both
    // because the mip chain wants one and because a probe is a quality dial, not
    // a free-form number.
    void setEnvProbeResolution(int res);
    int  envProbeResolution() const { return m_envA.resolution(); }

    // How many of the probe's six cube faces prepareEnvProbe() may refresh in one
    // call, at most. 1 is the cheapest and the reason a probe is affordable at
    // all; 6 means the cube is never more than a frame old. The actual rate is
    // picked per call from how fast the viewpoint is moving, up to this cap --
    // see prepareEnvProbe(), which is where the trade is explained.
    void setEnvProbeMaxFaces(int faces);
    int  envProbeMaxFaces() const { return m_envMaxFaces; }

    // Bind the probe's cubemap to `unit` for sampling, and report its coarsest
    // mip level. renderScene() already does this for the lit shader; this is for
    // an APP pass that draws its own reflective surface without going through the
    // queue -- the sandbox's river water, which is a shader of its own because it
    // is at a different height every ten metres and the planar reflection the
    // lake uses assumes exactly one.
    void  bindEnvProbe(std::uint32_t unit) const;
    float envProbeMaxLod() const;

    void end(); // convenience: prepareShadows() + one lit pass from the camera

    // Multi-pass building blocks (for reflection/refraction etc.):
    // render the shadow cascades once from the stored camera + light. The
    // optional callback is invoked per cascade with that cascade's light-space
    // matrix, so the app can add its own casters (e.g. instanced trees).
    //
    // It is also told WHICH cascade this is and how far that cascade reaches, in
    // metres from the eye. A caster that is expensive per instance wants both:
    // the near cascade covers twenty metres and has no use for a forest two
    // hundred metres out, and the far one spreads its texels so thinly that a
    // tree's shadow lands on one of them. Without those two numbers every
    // cascade is handed the same whole scene and the near ones pay for detail
    // that the far ones cannot show.
    using ShadowCaster =
        std::function<void(const glm::mat4& lightSpace, int cascade, float cascadeFar)>;
    void prepareShadows(const ShadowCaster& extra = {});

    // The same, fitted to a DIFFERENT eye than the one begin() was given, and
    // without disturbing the submitted queue. For split screen: cascades are cut
    // to a view frustum, so a second player looking somewhere else needs its own
    // set -- fitting both panes to player one's frustum leaves the second one
    // with shadows that fade out a few metres ahead, or none at all once the two
    // are far apart. It costs a second cascade pass over the queue, which is why
    // it is a separate call and not the default.
    void prepareShadowsFor(const Camera& camera, float aspect,
                           const ShadowCaster& extra = {});
    // Render the submitted opaque queue with an explicit view/projection,
    // eye position and world-space clip plane into the currently bound target
    // (does not clear). Pass kNoClip to disable clipping.
    void renderScene(const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& eye, const glm::vec4& clipPlane,
                     bool tonemap = true, bool skipReflective = false);

    // A clip plane that keeps every fragment (effectively no clipping).
    static const glm::vec4 kNoClip;

    // Viewport shading mode, uploaded to every lit-shader draw as `uShade`:
    // 0 the material in full, 1 solid, 2 solid lit, 3 wireframe. See lit.frag,
    // which is where the modes actually are -- this only carries the number, so
    // that an app switching modes does not have to reach into every material it
    // ever built. Anything a game renders leaves it at 0.
    void setShadingMode(int mode) { m_shadingMode = mode; }
    int  shadingMode() const      { return m_shadingMode; }

    // Whether the sun casts at all. Off still CLEARS the cascades -- a cleared
    // depth map reads as "nothing between here and the sun", so the scene comes
    // out simply unshadowed rather than black -- and skips the queue replay,
    // which is the part that costs: four passes over every submitted mesh.
    void setShadowsEnabled(bool v) { m_shadowsEnabled = v; }
    bool shadowsEnabled() const    { return m_shadowsEnabled; }

    CascadedShadowMap&       shadows()       { return m_csm; }
    const CascadedShadowMap& shadows() const { return m_csm; }

    // Frustum-culling stats from the most recent renderScene() call.
    int lastDrawn()  const { return m_lastDrawn; }
    int lastCulled() const { return m_lastCulled; }

    // What the cascade pass replayed, summed over every cascade of the most
    // recent prepareShadows(). The number that matters is the TRIANGLES: a
    // cascade is culled on four planes only, so the far one covers the whole
    // shadowed range and redraws almost everything loaded. Four modest-looking
    // draw counts can still be millions of triangles a frame.
    int       shadowDraws() const { return m_shadowDraws; }
    long long shadowTris()  const { return m_shadowTris; }

    // What the frame submitted, for a consumer that has to render the scene a
    // SECOND way.
    //
    // The queue is the only place that knows the whole drawn world in one list.
    // Terrain chunks, roads, bridges, loops, decals, splines, city towers and
    // every entity mesh arrive here from a dozen unrelated systems, and by the
    // time they do, all of them are the same thing: a mesh, a surface and a
    // place. An offline renderer that harvested those systems one by one would
    // have to be extended every time a new one is written, and would quietly
    // omit whatever nobody remembered; harvesting the queue cannot, because the
    // queue is what "the scene" means to the picture on screen.
    //
    // Valid between submit() and the next begin(). The pointers are the caller's
    // own meshes and materials -- borrowed, not owned.
    struct Submission {
        const Mesh*     mesh;
        const Material* material;
        glm::mat4       model;
        float           opacity;
        bool            reflective;
        // The caller's forceTransparent: this material's transparency lives in
        // its texture's alpha channel rather than in the scalar. Carried
        // because it is the ONLY thing that separates a map whose alpha means
        // transparency from one whose alpha means nothing -- and a great many
        // opaque materials ship an atlas with an alpha channel in it.
        bool            textureAlphaIsTransparency;
    };
    std::vector<Submission> submissions() const;

    // The frame's lighting, as it was handed in. Same purpose as submissions():
    // a second renderer needs the sun the first one used, not one reconstructed
    // from the scene file and hoped to match.
    const DirectionalLight&        light()        const { return m_light; }
    const std::vector<PointLight>& pointLights()  const { return m_pointLights; }
    const std::vector<SpotLight>&  spotLights()   const { return m_spotLights; }
    const Fog&                     fog()          const { return m_fog; }
    float                          exposure()     const { return m_exposure; }
    bool                           iblEnabled()   const { return m_iblEnabled && m_ibl; }
    float                          iblIntensity() const { return m_iblIntensity; }

private:
    struct Renderable {
        const Mesh*     mesh;
        const Material* material;
        glm::mat4       model;
        bool            castsPointShadow;
        bool            reflective;
        float           opacity;
        bool            forceTransparent;
        // What the SHADOW passes see of this surface, resolved once here at
        // submit() because the depth passes bind no material at all. A pane of
        // glass that is clear to the eye and solid to the sun is the same
        // object described two ways, and only one of them is right.
        float           castCoverage;  // 0 = the light passes straight through
        int             castAlphaMode; // 0 opaque, 1 cutout, 2 blend
        float           castCutoff;    // cutout threshold (mode 1)
        const Texture*  castTex;       // alpha source for modes 1 and 2
    };

    // The opaque scene, copied out of whatever target is bound just before the
    // transparent pass. Grown to the viewport on demand and reused; 0 until some
    // frame has a refracting surface in it.
    std::uint32_t     m_sceneCopy      = 0;
    int               m_sceneCopyW     = 0, m_sceneCopyH = 0;
    int               m_sceneCopyX     = 0, m_sceneCopyY = 0;
    void captureSceneCopy();

    CascadedShadowMap m_csm;
    int               m_shadowDraws    = 0;   // see shadowDraws()
    long long         m_shadowTris     = 0;
    bool              m_shadowsEnabled = true;
    int               m_shadingMode    = 0;
    Shader            m_depthShader;
    Shader            m_cubeDistShader;             // point-shadow distance pass
    // World bounds of m_queue, rebuilt at the start of each shadow pass.
    void buildCullBounds();
    // Upload one caster's coverage to the bound depth shader. Both shadow
    // passes need it, and a caster they disagreed about would show up as a
    // shadow the sun casts but the point light does not.
    void uploadCoverage(const Shader& shader, const Renderable& r,
                        float dither) const;
    std::vector<WorldAabb> m_cullBounds;

    std::vector<CubeShadowMap> m_pointShadows;      // one per shadowed point light
    int               m_shadowedCount = 0;
    // Environment probe, ping-ponged: lit passes sample m_envRead (last frame's
    // capture) while prepareEnvProbe() renders into m_envWrite, then they swap.
    // Double-buffering avoids sampling the cubemap that is the current target.
    // 256 a side by default: at 128 a reflected building is a handful of texels
    // and a wet road shows coloured mush. The cost of raising it is fill, not
    // draw calls -- the same six scene passes either way -- so it buys more than
    // it costs until the faces get big.
    CubeRenderTarget  m_envA{kDefaultEnvProbeRes};
    CubeRenderTarget  m_envB{kDefaultEnvProbeRes};
    // Where the running sweep is: which face goes next, the position it started
    // from, and whether a complete cube exists yet to sample.
    int               m_envFace   = 0;
    int               m_envMaxFaces = 3;
    glm::vec3         m_envSweepPos{0.0f};
    // Where the probe was asked for LAST call, which is how the sweep rate reads
    // the viewpoint's speed without the renderer having to be told a delta time.
    glm::vec3         m_envLastPos{0.0f};
    bool              m_envHasLast = false;
    bool              m_envPrimed = false;
    CubeRenderTarget* m_envRead  = &m_envA;
    CubeRenderTarget* m_envWrite = &m_envB;
    std::vector<Renderable> m_queue;

    const Camera*    m_camera = nullptr;
    float            m_aspect = 1.0f;
    DirectionalLight m_light;
    std::vector<PointLight> m_pointLights;
    std::vector<SpotLight>  m_spotLights;
    Fog              m_fog;
    const Texture3D* m_gridR = nullptr;
    const Texture3D* m_gridG = nullptr;
    const Texture3D* m_gridB = nullptr;
    glm::vec3        m_gridLo{0.0f}, m_gridHi{1.0f};
    float            m_gridIntensity = 1.0f;
    // A 1x1x1 black volume, bound whenever there is no grid. Same reason the
    // env probe is always bound: an unbound sampler3D reads unit 0, which is
    // whatever 2D texture the material happened to leave there.
    mutable Texture3D m_gridFallback;

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
