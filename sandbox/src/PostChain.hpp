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
    PostChain() = default;
    ~PostChain();
    PostChain(const PostChain&)            = delete;
    PostChain& operator=(const PostChain&) = delete;

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
        // Tonemap curve: 0 ACES (fit), 1 AgX, 2 Khronos PBR Neutral. See
        // composite.frag; pathtrace::Grade::curve carries the same number.
        int   curve = 0;

        // Auto exposure, relative to the slider: `exposure` is what a frame at
        // the reference brightness gets, and the meter corrects away from it
        // by at most minEv..maxEv stops, easing at `adaptSpeed` (1/s).
        bool  autoExposure = false;
        float autoMinEv = -1.5f, autoMaxEv = 2.5f;
        float adaptSpeed = 1.5f;
        float dt = 1.0f / 60.0f;       // frame time, for the easing

        // Temporal anti-aliasing (taa.frag). The frame must have been drawn
        // with jitter(frame) folded into its projection, and beginMotion()
        // called after the opaque pass. curVP / prevVP are this and last
        // frame's view-projection WITHOUT the jitter; `jitterUV` is the offset
        // this frame was drawn with, in UV. `taaReset` drops the history (a
        // camera cut, a scene load).
        bool      taa = false;
        glm::mat4 curVP{1.0f};
        glm::mat4 prevVP{1.0f};
        glm::vec2 jitterUV{0.0f};
        bool      taaReset = false;

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
    // `sharpen` (0..1) applies contrast-adaptive sharpening when FXAA is off --
    // the partner of TAA, which resolves a little soft.
    void present(fitzel::Mesh& fsQuad, bool fxaaEnabled, float sharpen = 0.0f);

    // --- Temporal anti-aliasing ----------------------------------------------
    // The sub-pixel offset for frame `frame`, in NDC: fold it into the
    // projection (proj[2][0] += x, proj[2][1] += y) of every pass that draws
    // into the HDR buffer. Halton(2,3), eight steps -- evenly spread over the
    // pixel however many of them the history has seen.
    static glm::vec2 jitter(unsigned frame, int width, int height);

    // Bind a target that shares `hdr`'s depth and clear its colour, for
    // Renderer::renderMotion to draw into. Call after the opaque scene, before
    // run(); a frame that does not call it is resolved from depth alone.
    void beginMotion(const fitzel::RenderTarget& hdr);

    // The log2 luminance a sunlit daytime frame meters at: what auto exposure
    // holds the picture to. Measured, not derived: five daytime projects
    // (forest, desert, lake, road, dunes) metered between -3.43 and -1.54 with
    // most near -3, which is the look every existing scene was tuned under --
    // so a scene like them keeps its exposure and only the outliers move.
    static constexpr float kAutoReferenceLog2 = -3.0f;

    // The auto-exposure correction the composite applied a couple of frames
    // ago (1 with auto exposure off), and that frame's raw meter reading in
    // log2 luminance. Read back without stalling: the value is copied into a
    // ring of pixel buffers and mapped only once the card has long finished
    // with it. The path tracer's capture multiplies this in, so a render is
    // exposed as the viewport it was taken from.
    float autoExposureScale() const { return m_autoScale; }
    float meteredLog2()       const { return m_meterLog2; }

private:
    void readBackMeter(const Params& p);

    int m_w = 0, m_h = 0;

    fitzel::Shader m_ssao, m_ssaoBlur, m_bloomDown, m_bloomUp;
    fitzel::Shader m_composite, m_motionBlur, m_fxaa, m_meter, m_taa;

    // TAA: two resolves, ping-ponged (one is this frame's, the other the
    // history it reads), and the motion target, which borrows the HDR
    // buffer's depth -- raw GL, because a RenderTarget owns its own depth.
    fitzel::RenderTarget m_taaRT[2] = {{1, 1, fitzel::RenderTarget::Format::RGBA16F},
                                       {1, 1, fitzel::RenderTarget::Format::RGBA16F}};
    int      m_taaCur      = 0;
    bool     m_taaValid    = false;   // the other target holds a usable history
    unsigned m_motionFbo   = 0;
    unsigned m_motionTex   = 0;
    int      m_motionW = 0, m_motionH = 0;
    bool     m_motionThisFrame = false;

    // Exposure meter: two 1x1 targets, ping-ponged (the pass reads last
    // frame's value to ease from it), plus the read-back ring.
    fitzel::RenderTarget m_adapt[2] = {{1, 1, fitzel::RenderTarget::Format::RGBA16F},
                                       {1, 1, fitzel::RenderTarget::Format::RGBA16F}};
    int  m_adaptCur    = 0;
    bool m_adaptPrimed = false;
    // The 16x9 first stage of the meter (see meter.frag).
    fitzel::RenderTarget m_meterCells{16, 9, fitzel::RenderTarget::Format::RGBA16F};
    static constexpr int kMeterRing = 4;
    unsigned m_meterPbo[kMeterRing]   = {0, 0, 0, 0};
    void*    m_meterFence[kMeterRing] = {nullptr, nullptr, nullptr, nullptr}; // GLsync
    int   m_meterFrame = 0;   // next slot to write
    float m_autoScale  = 1.0f;
    float m_meterLog2  = 0.0f;

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
