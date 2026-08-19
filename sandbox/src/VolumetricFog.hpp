#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>

namespace fitzel { class CascadedShadowMap; }

// Volumetric fog: a placed body of mist that the scene is marched through.
//
// The engine already has fog -- the exponential height haze in lit.frag, applied
// per pixel from a closed form. It is cheap and it is the right thing for aerial
// perspective, but it is global and it has no shape: every surface at the same
// distance gets the same amount of it, nothing can be foggier than its
// neighbour, and the sun cannot be blocked on the way in. So there is no valley
// mist, no bank drifting across the track, and no shaft cutting through a gap.
//
// This is the other half. A box of participating medium, its density taken from
// an animated 3D noise field, marched front to back with the sun sampled through
// the same cascades the surfaces use. Everything the closed form cannot do lives
// here -- and everything it does well is still done there, so the two are meant
// to be used together: haze for the horizon, this for the mist you drive into.
//
// IT OWNS ITS OWN RESOURCES, as PostChain does and for the same reason: two
// shaders, one render target and one baked 3D noise texture that nothing else in
// the program touches. What a caller has to say is only what changes -- the
// authored Settings, and where the eye is this frame.
class VolumetricFog {
public:
    VolumetricFog() = default;
    ~VolumetricFog();

    VolumetricFog(const VolumetricFog&)            = delete;
    VolumetricFog& operator=(const VolumetricFog&) = delete;

    // What the scene author sets. Saved with the project, because a mist bank is
    // part of the world someone built, not a preference of the machine it runs
    // on -- the machine only gets a say in `steps` and `resScale`.
    struct Settings {
        bool enabled = false;

        // --- The volume -----------------------------------------------------
        // An axis-aligned box, given as a centre and full extents because that is
        // how it is placed and resized by hand. No rotation: a mist bank has no
        // visible orientation to get wrong, and a rotated box would cost every
        // sample a matrix it does not otherwise need.
        glm::vec3 center{0.0f, 15.0f, 0.0f};
        glm::vec3 size{600.0f, 40.0f, 600.0f};
        // Keep the box centred on the eye in X/Z. For ground mist over a whole
        // track, which wants a volume that is everywhere rather than one big
        // enough to be everywhere -- a 4 km box marched in 40 steps is a step
        // every hundred metres, and the structure disappears between them.
        //
        // On by default, and that is what makes switching the fog on a single
        // click: a fixed box has to be found before it can be seen, and a scene
        // whose world does not happen to sit at the origin would answer the
        // first click with nothing at all.
        bool  followCamera = true;
        float edge          = 0.25f; // outer fraction of each half-extent that fades
        float heightFalloff = 0.8f;  // how fast it thins toward the top (0 = even)

        // --- The medium -----------------------------------------------------
        float     density  = 0.08f;   // extinction per metre where the noise is solid
        glm::vec3 color{0.80f, 0.86f, 0.95f};
        float     coverage = 0.30f;   // how much of the volume has any fog in it

        // --- The noise ------------------------------------------------------
        float     noiseScale = 0.010f; // world metres -> noise space (smaller = bigger banks)
        float     detail     = 0.45f;  // how hard the worley band breaks the shape up
        float     warp       = 0.35f;  // domain warp: the swirl inside a bank
        glm::vec3 wind{2.0f, 0.0f, 0.6f}; // metres per second the field drifts

        // --- Lighting -------------------------------------------------------
        float anisotropy       = 0.55f; // forward scattering: the glow around the sun
        float sunIntensity     = 1.0f;  // scales Params::sunColor, which is already HDR
        float ambientIntensity = 1.0f;  // ..and Params::ambient, the sky/haze radiance
        bool  shafts     = true;  // sample the sun cascades (god rays through the mist)
        bool  selfShadow = true;  // short march toward the sun (depth inside a bank)

        // --- What it costs ---------------------------------------------------
        int steps    = 40;  // samples along the ray
        int resScale = 2;   // 1 = full res, 2 = half, 3 = third, 4 = quarter

        // Editor only: draw the box as a wireframe in the viewport, because a
        // volume you cannot see is a volume you cannot place.
        bool showVolume = false;
    };

    // Where the eye is, and what is lighting the frame. Everything here changes
    // per frame or per pane; nothing here is authored.
    struct Params {
        glm::mat4 viewProj{1.0f};
        glm::vec3 camPos{0.0f};
        glm::vec3 camFwd{0.0f, 0.0f, -1.0f};
        float     time = 0.0f;
        glm::vec3 sunDir{0.0f, 1.0f, 0.0f};   // points *towards* the sun
        // The sun's HDR radiance, not its tint: the fog is lit by the same light
        // the surfaces are, so it dims at dusk, reddens at sunset and goes out
        // with a deactivated Sun entity without anything here saying so.
        glm::vec3 sunColor{1.0f};
        // What lights the fog where the sun does not reach. The scene's haze
        // colour is the right thing to pass: it is already "what distance looks
        // like" in this sky, so the marched fog and the closed-form haze agree
        // about the colour of the air instead of being two different greys.
        glm::vec3 ambient{0.3f};
    };

    // Load the shaders and bake the noise. False means a shader failed to
    // compile; unlike the post chain that is survivable (the frame simply has no
    // volumetric fog in it), so the caller may carry on -- render() then does
    // nothing.
    bool init();
    bool ready() const { return m_ok; }

    // March the volume for this frame and blend it into `hdr`, which must be the
    // HDR scene buffer with its depth attached as a texture. Leaves `hdr` bound
    // and the depth/blend state as it found it.
    //
    // Call it AFTER everything that writes depth (scene, water, vegetation,
    // particles) and BEFORE the post chain: the fog is part of the picture bloom
    // blooms and the tonemap tonemaps, not something painted over the finished
    // frame.
    void render(const fitzel::RenderTarget& hdr, const Settings& s, const Params& p,
                fitzel::Mesh& fsQuad, const fitzel::CascadedShadowMap* shadows);

    // The box actually marched, which is the authored one unless it follows the
    // camera. The editor draws its wireframe from this, so what is shown is what
    // is marched rather than a second guess at it.
    static void worldBox(const Settings& s, const glm::vec3& camPos,
                         glm::vec3& lo, glm::vec3& hi);

private:
    void resize(int w, int h);

    bool m_ok = false;
    int  m_w = 0, m_h = 0;

    fitzel::Shader       m_march, m_upsample;
    fitzel::RenderTarget m_fogRT{1, 1};   // replaced by the first resize()
    std::uint32_t        m_noiseTex = 0;  // GL_TEXTURE_3D, baked once in init()
};
