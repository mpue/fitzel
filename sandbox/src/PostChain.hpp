#pragma once

#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>

// The post chain: everything between the lit HDR buffer and the finished image.
// SSAO and its denoise, the bloom pyramid, the composite (tonemap, god rays,
// depth of field, colour grade), the radial speed blur, and FXAA on the way out.
//
// IT OWNS ITS OWN RESOURCES, and that is the point of it being a class. These
// seven shaders and five intermediate targets are used by nothing else in the
// program; kept as locals next to everything else, moving the passes anywhere
// meant carrying a dozen references along, so they never moved. Owned here, the
// interface is what a caller actually has to say: one HDR buffer in, the frame's
// settings, and the pane it is drawing.
//
// What stays OUTSIDE: binding the destination and setting the viewport. Only the
// caller knows about panes, presentation mode and which half of the screen this
// is -- see present().
class PostChain {
public:
    // Load the shaders. False means one failed to compile, and the caller should
    // treat that as fatal: a frame without a composite pass is a black screen.
    bool init();

    // (Re)create the intermediates for an image of this size. Cheap to call
    // every frame -- it returns immediately unless the size actually changed,
    // which is what makes it safe to drive straight off the pane size.
    void resize(int w, int h);

    int width()  const { return m_w; }
    int height() const { return m_h; }

    // Everything the passes read that is not the image itself. Grouped rather
    // than passed as twenty arguments, so adding a knob does not re-write every
    // call site.
    struct Params {
        glm::mat4 proj{1.0f};        // this pane's projection (SSAO unprojects with it)
        glm::mat4 viewProj{1.0f};    // ...and its full VP, for the sun and the blur anchor
        glm::vec3 camPos{0.0f};
        float     nearPlane = 0.1f, farPlane = 600.0f;
        float     aspect = 1.0f;

        // Sun, for the god rays and the flare: the direction it comes FROM and
        // the colour it has after the sky has had its say.
        glm::vec3 sunDir{0.0f, 1.0f, 0.0f};
        glm::vec3 sunCol{1.0f};

        float ssaoRadius = 0.5f, ssaoBias = 0.02f, ssaoPower = 1.0f, ssaoStrength = 1.0f;
        float bloomThreshold = 1.0f, bloomKnee = 0.5f;
        float bloomIntensity = 0.35f, rayIntensity = 0.5f;
        float dofNear = 25.0f, dofFar = 140.0f, dofMax = 0.0f;
        float exposure = 1.0f;
        float hueShift = 0.0f, saturation = 1.0f, valueGain = 1.0f;
        float warmth = 0.0f, contrast = 1.0f;

        // Radial speed blur: how strong, and the world point it stays sharp
        // around (the craft being followed). Invalid = no blur this pane.
        float     blurStrength = 0.0f;   // already scaled by the craft's speed
        glm::vec3 blurAnchor{0.0f};
        bool      blurAnchorValid = false;
    };

    // Run the chain over `hdr` (colour + sampleable depth). Leaves the finished
    // low-dynamic-range image in an internal target, ready for present().
    void run(const fitzel::RenderTarget& hdr, const Params& p, fitzel::Mesh& fsQuad);

    // Draw that image into whatever target and viewport are bound NOW, filtered
    // by FXAA (or copied straight through with it off). Separate from run()
    // because the destination is the caller's business: the editor's viewport
    // image, the screen, or one half of either.
    void present(fitzel::Mesh& fsQuad, bool fxaaEnabled);

private:
    int m_w = 0, m_h = 0;

    fitzel::Shader m_ssao, m_ssaoBlur, m_bloomDown, m_bloomUp;
    fitzel::Shader m_composite, m_motionBlur, m_fxaa;

    // Created at 1x1 and replaced by the first resize(): a render target needs a
    // live GL context and a size, and this object knows neither until the window
    // and the viewport panel do.
    fitzel::RenderTarget m_ssaoRT{1, 1}, m_ssaoBlurRT{1, 1};
    fitzel::RenderTarget m_postRT{1, 1}, m_mbRT{1, 1};
    // The bloom pyramid: [0] is half res, each level halves again. Levels stop
    // before they get degenerate, so a small viewport gets a shorter pyramid.
    std::vector<fitzel::RenderTarget> m_bloom;
    // Which of the two the finished image ended up in (the blur is skipped when
    // it would not be visible).
    const fitzel::RenderTarget* m_result = nullptr;
};
