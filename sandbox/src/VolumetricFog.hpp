#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>

#include "FogMedium.hpp"

namespace fitzel { class CascadedShadowMap; }

// Volumetric fog: placed bodies of mist that the scene is marched through.
//
// The engine already has fog -- the exponential height haze in lit.frag, applied
// per pixel from a closed form. It is cheap and it is the right thing for aerial
// perspective, but it is global and it has no shape: every surface at the same
// distance gets the same amount of it, nothing can be foggier than its
// neighbour, and the sun cannot be blocked on the way in. So there is no valley
// mist, no bank sitting in one archway, and no shaft cutting through a gap.
//
// This is the other half. A BOX of participating medium, its density taken from
// an animated 3D noise field, marched front to back with the sun sampled through
// the same cascades the surfaces use.
//
// It renders a LIST of them, and that list is the point of the design. A fog
// volume is a thing you PLACE, so most of them come from VolumetricFogComponent
// on an entity -- hang one on an Empty, scale it with the gizmo, and the box you
// see selected is the box that gets marched. The scene-wide volume in Settings
// is one more entry in the same list, for the case a placed box is wrong for:
// mist over a whole track, which wants to follow the camera rather than stand
// somewhere.
//
// Each volume is drawn as its own PROXY BOX rather than as a fullscreen pass,
// and that is what makes many small ones affordable: a volume covering a tenth
// of the screen costs a tenth of the fill. They accumulate into one buffer back
// to front, so overlapping volumes read as one body of air rather than as
// whichever was drawn last.
//
// IT OWNS ITS OWN RESOURCES, as PostChain does and for the same reason: two
// shaders, a render target, a proxy cube and one baked 3D noise texture that
// nothing else in the program touches.
class VolumetricFog {
public:
    VolumetricFog() = default;
    ~VolumetricFog();

    VolumetricFog(const VolumetricFog&)            = delete;
    VolumetricFog& operator=(const VolumetricFog&) = delete;

    // What the air is made of, shared verbatim by the component and the
    // scene-wide box -- see FogMedium.hpp for why it lives in a header of its
    // own rather than in here.
    using Medium = FogMedium;

    // One box of that medium, in world space. `model` maps the unit cube
    // (-0.5..0.5 on every axis) onto it, which is how it arrives from an entity:
    // the same translate*rotate*scale the gizmo and the selection wireframe are
    // built from, so the box marched and the box drawn cannot disagree.
    struct Volume {
        glm::mat4 model{1.0f};
        Medium    medium;
    };

    // The scene-wide volume. Saved with the project like the rest of the sky,
    // and separate from the component because it answers a different question:
    // not "there is mist HERE" but "the air in this world is thick".
    struct Settings {
        bool enabled = false;

        glm::vec3 center{0.0f, 15.0f, 0.0f};
        glm::vec3 size{600.0f, 40.0f, 600.0f};
        // Keep the box centred on the eye in X/Z. For ground mist over a whole
        // track, which wants a volume that is everywhere rather than one big
        // enough to be everywhere -- a 4 km box marched in 40 steps is a step
        // every hundred metres, and the structure disappears between them.
        bool followCamera = true;

        Medium medium;

        // What the whole PASS costs, not this volume: one buffer serves every
        // volume in the frame, so its resolution cannot belong to one of them.
        int resScale = 2;   // 1 = full res, 2 = half, 3 = third, 4 = quarter

        // Editor only: draw the scene-wide box as a wireframe. Component volumes
        // need no such flag -- selecting the entity already outlines it.
        bool showVolume = false;
    };

    // How many volumes one frame may march. The NEAREST ones win. A cap rather
    // than a distance fade because what a volume costs is fill, and fill is what
    // runs out when a level gets mist per archway.
    static constexpr int kMaxVolumes = 24;

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

    // Load the shaders, bake the noise, build the proxy cube. False means one of
    // those failed; unlike the post chain that is survivable (the frame simply
    // has no volumetric fog in it), so the caller may carry on -- render() then
    // does nothing.
    bool init();
    bool ready() const { return m_ok; }

    // March every volume and blend the result into `hdr`, which must be the HDR
    // scene buffer with its depth attached as a texture. `volumes` may arrive in
    // any order; sorting and the scene-wide volume from `s` happen on a copy
    // inside, so what the caller passes is left exactly as it was. Leaves `hdr`
    // bound and the depth/blend state as it found it.
    //
    // Call it AFTER everything that writes depth (scene, water, vegetation,
    // particles) and BEFORE the post chain: the fog is part of the picture bloom
    // blooms and the tonemap tonemaps, not something painted over the finished
    // frame.
    void render(const fitzel::RenderTarget& hdr, const std::vector<Volume>& volumes,
                const Settings& s, const Params& p, fitzel::Mesh& fsQuad,
                const fitzel::CascadedShadowMap* shadows);

    // The scene-wide box's corners, which are the authored ones unless it
    // follows the camera. The editor draws its wireframe from this, so what is
    // shown is what is marched rather than a second guess at it.
    static void worldBox(const Settings& s, const glm::vec3& camPos,
                         glm::vec3& lo, glm::vec3& hi);

private:
    void resize(int w, int h);
    void drawVolume(const Volume& v, const Params& p, float lightStep);

    bool m_ok = false;
    int  m_w = 0, m_h = 0;

    fitzel::Shader       m_march, m_upsample;
    fitzel::Mesh         m_box;            // unit proxy cube, one draw per volume
    fitzel::RenderTarget m_fogRT{1, 1};    // replaced by the first resize()
    std::uint32_t        m_noiseTex = 0;   // GL_TEXTURE_3D, baked once in init()
    // The frame's draw list: the caller's volumes plus the scene-wide one,
    // sorted and capped. A member so a frame with thirty of them still does not
    // allocate.
    std::vector<Volume>  m_draw;
};
