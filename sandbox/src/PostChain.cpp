#include "PostChain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include <fitzel/rhi/Device.hpp>

#include "FrameRender.hpp"   // applySunShadows

namespace rhi = fitzel::rhi;

namespace {
// Every pass here is a fullscreen triangle pair through the same trivial vertex
// shader; only the fragment stage differs. Named once so the list below reads as
// what it is: seven fragment programs over one quad.
constexpr const char* kVert = "assets/shaders/sky.vert";

// IEEE half -> float, for the meter's read-back (it comes back in the target's
// own format; see readBackMeter).
float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    float v;
    if (exp == 0)        v = std::ldexp(static_cast<float>(man), -24);            // subnormal
    else if (exp == 31)  v = man ? std::numeric_limits<float>::quiet_NaN()
                                 : std::numeric_limits<float>::infinity();
    else                 v = std::ldexp(static_cast<float>(man | 0x400u),
                                        static_cast<int>(exp) - 25);
    return sign ? -v : v;
}
}

bool PostChain::init() {
    struct Load { fitzel::Shader* dst; const char* frag; const char* what; };
    const Load loads[] = {
        {&m_ssao,       "assets/shaders/ssao.frag",       "ssao"},
        {&m_ssaoBlur,   "assets/shaders/ssaoblur.frag",   "ssao blur"},
        {&m_bloomDown,  "assets/shaders/bloomdown.frag",  "bloom down"},
        {&m_bloomUp,    "assets/shaders/bloomup.frag",    "bloom up"},
        {&m_composite,  "assets/shaders/composite.frag",  "composite"},
        {&m_motionBlur, "assets/shaders/motionblur.frag", "motion blur"},
        {&m_fxaa,       "assets/shaders/fxaa.frag",       "fxaa"},
        {&m_meter,      "assets/shaders/meter.frag",      "exposure meter"},
        {&m_taa,        "assets/shaders/taa.frag",        "taa resolve"},
    };
    for (const Load& l : loads) {
        *l.dst = fitzel::Shader::fromFiles(kVert, l.frag);
        if (!l.dst->isValid()) {
            std::fprintf(stderr, "Failed to load %s shader\n", l.what);
            return false;
        }
    }
    rhi::Device& gpu = rhi::device();
    for (rhi::BufferHandle& pbo : m_meterPbo) {
        // Room for four floats: the browser reads the pixel as RGBA32F
        // (readBackMeter), the desktop as RGBA16F into the first half.
        pbo = gpu.createBuffer(rhi::BufferKind::Readback, rhi::BufferUsage::Stream,
                               4 * sizeof(float));
    }
    return true;
}

PostChain::~PostChain() {
    rhi::Device& gpu = rhi::device();
    if (m_prevDepthTex) gpu.destroy(m_prevDepthTex);
    if (m_prevDepthFbo) gpu.destroy(m_prevDepthFbo);
    if (m_motionTex) gpu.destroy(m_motionTex);
    if (m_motionFbo) gpu.destroy(m_motionFbo);
    for (rhi::FenceHandle& f : m_meterFence)
        if (f) { gpu.destroy(f); f = {}; }
    for (rhi::BufferHandle& b : m_meterPbo)
        if (b) { gpu.destroy(b); b = {}; }
}

void PostChain::resize(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == m_w && h == m_h) return;
    m_w = w; m_h = h;

    using RT = fitzel::RenderTarget;
    m_ssaoRT     = RT(std::max(1, w / 2), std::max(1, h / 2));
    m_ssaoBlurRT = RT(std::max(1, w / 2), std::max(1, h / 2));
    m_postRT     = RT(w, h, RT::Format::RGBA8);
    m_mbRT       = RT(w, h, RT::Format::RGBA8);
    m_taaRT[0]   = RT(w, h, RT::Format::RGBA16F);
    m_taaRT[1]   = RT(w, h, RT::Format::RGBA16F);
    m_taaValid   = false;   // a history of another size is no history
    m_historyValid = false;

    m_bloom.clear();
    int lw = std::max(1, w / 2), lh = std::max(1, h / 2);
    for (int i = 0; i < 5 && lw >= 8 && lh >= 8; ++i) {
        m_bloom.emplace_back(lw, lh, RT::Format::RGBA16F);
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
    }
    m_result = nullptr;
}

glm::vec2 PostChain::jitter(unsigned frame, int width, int height) {
    auto halton = [](unsigned i, unsigned base) {
        float f = 1.0f, r = 0.0f;
        for (unsigned n = i; n > 0; n /= base) {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(n % base);
        }
        return r;
    };
    const unsigned i = (frame % 8u) + 1u;   // skip index 0, which is (0, 0)
    // [-0.5, 0.5) of a pixel, in NDC (a pixel is 2/size of it).
    return glm::vec2((halton(i, 2) - 0.5f) * 2.0f / static_cast<float>(std::max(width, 1)),
                     (halton(i, 3) - 0.5f) * 2.0f / static_cast<float>(std::max(height, 1)));
}

void PostChain::beginMotion(const fitzel::RenderTarget& hdr) {
    rhi::Device& gpu = rhi::device();
    if (!m_motionTex || m_motionW != m_w || m_motionH != m_h) {
        if (m_motionTex) gpu.destroy(m_motionTex);
        rhi::TextureDesc d;
        d.format  = rhi::Format::RGBA16F;
        d.width   = m_w;
        d.height  = m_h;
        d.sampler = rhi::SamplerDesc::nearestClamp();
        m_motionTex = gpu.createTexture(d);
        m_motionW = m_w;
        m_motionH = m_h;
    }
    rhi::FramebufferDesc fb;
    fb.color[0] = rhi::Attachment{m_motionTex};
    // The lit pass's own depth, so the motion is written only where the moving
    // surface is actually the one in front. Re-attached every frame: the HDR
    // target is reallocated whenever the pane changes size.
    fb.depth = rhi::Attachment{hdr.depthTexture()};
    if (!m_motionFbo) m_motionFbo = gpu.createFramebuffer(fb);
    else gpu.setAttachments(m_motionFbo, fb);
    gpu.bindFramebuffer(m_motionFbo);
    gpu.setViewport(0, 0, m_w, m_h);
    gpu.setClearColor(glm::vec4(0.0f));
    gpu.clear(rhi::ClearColor);
    m_motionThisFrame = true;
}

void PostChain::run(const fitzel::RenderTarget& hdr, const Params& p,
                    fitzel::Mesh& fsQuad) {
    rhi::Device& gpu = rhi::device();
    // Fullscreen passes: no depth, no culling, no blending unless a pass asks
    // for it (the bloom upsample does).
    gpu.setDepthTest(false);
    gpu.setDepthWrite(false);
    gpu.setCullEnabled(false);
    gpu.setBlendEnabled(false);

    // --- Temporal anti-aliasing: the jittered frame into the running history.
    //     Everything after reads the resolve instead of the raw frame -- bloom
    //     and the meter too, or they would flicker with the jitter. -----------
    const fitzel::RenderTarget* scene = &hdr;
    if (p.taa) {
        const int next = 1 - m_taaCur;
        m_taaRT[next].bind();
        m_taa.bind();
        hdr.bindColorTexture(0);              m_taa.setInt("uCur", 0);
        m_taaRT[m_taaCur].bindColorTexture(1); m_taa.setInt("uHistory", 1);
        hdr.bindDepthTexture(2);              m_taa.setInt("uDepth", 2);
        gpu.bindTexture(3, m_motionTex);       m_taa.setInt("uMotion", 3);
        m_taa.setInt("uHasMotion", (m_motionThisFrame && m_motionTex) ? 1 : 0);
        m_taa.setMat4("uInvCurVP", glm::inverse(p.curVP));
        m_taa.setMat4("uPrevVP", p.prevVP);
        m_taa.setVec2("uTexel", {1.0f / m_w, 1.0f / m_h});
        m_taa.setVec2("uJitter", p.jitterUV);
        m_taa.setFloat("uBlend", 0.1f);
        m_taa.setInt("uReset", (!m_taaValid || p.taaReset) ? 1 : 0);
        fsQuad.draw();
        m_taaCur   = next;
        m_taaValid = true;
        scene      = &m_taaRT[m_taaCur];
    } else {
        m_taaValid = false;   // what is in there now would be stale when it returns
    }
    m_motionThisFrame = false;

    // --- SSAO: occlusion from the HDR depth buffer (half-res) ---------------
    // Skipped while it would be multiplied by nothing: the composite mixes it in
    // by uAoStrength, so what the target still holds is never seen.
    if (p.ssaoStrength > 0.0f) {
        m_ssaoRT.bind();
        gpu.clear(rhi::ClearColor);
        m_ssao.bind();
        hdr.bindDepthTexture(0);
        m_ssao.setInt("uDepth", 0);
        m_ssao.setMat4("uProjection", p.proj);
        m_ssao.setMat4("uInvProjection", glm::inverse(p.proj));
        m_ssao.setFloat("uRadius", p.ssaoRadius);
        m_ssao.setFloat("uBias", p.ssaoBias);
        m_ssao.setFloat("uPower", p.ssaoPower);
        // Distance fade: depth precision toward the horizon isn't worth an AO term,
        // and the far field reads better hazy than dirty.
        m_ssao.setFloat("uFadeStart", 60.0f);
        m_ssao.setFloat("uFadeEnd", 160.0f);
        fsQuad.draw();

        // --- SSAO denoise: depth-aware blur, so the dither resolves without
        //     dragging occlusion across silhouettes ----------------------------
        m_ssaoBlurRT.bind();
        gpu.clear(rhi::ClearColor);
        m_ssaoBlur.bind();
        m_ssaoRT.bindColorTexture(0);  m_ssaoBlur.setInt("uAO", 0);
        hdr.bindDepthTexture(1);       m_ssaoBlur.setInt("uDepth", 1);
        m_ssaoBlur.setVec2("uTexel", {1.0f / m_ssaoRT.width(), 1.0f / m_ssaoRT.height()});
        m_ssaoBlur.setFloat("uNear", p.nearPlane);
        m_ssaoBlur.setFloat("uFar", p.farPlane);
        m_ssaoBlur.setInt("uOrtho", p.ortho ? 1 : 0);
        m_ssaoBlur.setFloat("uDepthSigma", 0.4f);
        fsQuad.draw();
    }

    // --- Bloom pyramid: threshold + downsample, then tent-upsample the levels
    //     back on top of each other (additive) -----------------------------
    // The sun rays march the same pyramid, so it is only skipped with both off.
    if (p.bloomIntensity > 0.0f || p.rayIntensity > 0.0f) {
        for (std::size_t i = 0; i < m_bloom.size(); ++i) {
            m_bloom[i].bind();
            gpu.clear(rhi::ClearColor);
            m_bloomDown.bind();
            if (i == 0) {
                scene->bindColorTexture(0);
                m_bloomDown.setVec2("uSrcTexel", {1.0f / m_w, 1.0f / m_h});
            } else {
                m_bloom[i - 1].bindColorTexture(0);
                m_bloomDown.setVec2("uSrcTexel", {1.0f / m_bloom[i - 1].width(),
                                                  1.0f / m_bloom[i - 1].height()});
            }
            m_bloomDown.setInt("uSrc", 0);
            m_bloomDown.setInt("uFirstPass", i == 0 ? 1 : 0);
            m_bloomDown.setFloat("uThreshold", p.bloomThreshold);
            m_bloomDown.setFloat("uKnee", p.bloomKnee);
            fsQuad.draw();
        }
        if (m_bloom.size() > 1) {
            gpu.setBlendEnabled(true);
            // each level adds onto the larger one
            gpu.setBlendFunc(rhi::BlendFactor::One, rhi::BlendFactor::One);
            m_bloomUp.bind();
            m_bloomUp.setInt("uSrc", 0);
            m_bloomUp.setFloat("uRadius", 1.0f);
            for (std::size_t i = m_bloom.size(); i-- > 1;) {
                m_bloom[i - 1].bind();
                m_bloom[i].bindColorTexture(0);
                m_bloomUp.setVec2("uSrcTexel", {1.0f / m_bloom[i].width(),
                                                1.0f / m_bloom[i].height()});
                fsQuad.draw();
            }
            gpu.setBlendEnabled(false);
        }
    }

    // --- Exposure meter: ease the 1x1 adapted luminance towards this frame --
    // Runs whether or not auto exposure is on, so switching it on starts from
    // where the eye already is instead of from a snap.
    {
        // Stage 0: every cell of a 16x9 grid reduces its part of the frame.
        m_meterCells.bind();
        m_meter.bind();
        m_meter.setInt("uStage", 0);
        scene->bindColorTexture(0);           m_meter.setInt("uSrc", 0);
        fsQuad.draw();
        // Stage 1: one pixel adds the cells up and eases towards them.
        const int prev = m_adaptCur, next = 1 - m_adaptCur;
        m_adapt[next].bind();
        m_meter.setInt("uStage", 1);
        m_meterCells.bindColorTexture(0);  m_meter.setInt("uSrc", 0);
        m_adapt[prev].bindColorTexture(1); m_meter.setInt("uPrev", 1);
        // Frame-rate independent easing; the first frame takes the reading
        // outright, or the picture would fade in from black.
        const float blend = m_adaptPrimed
            ? 1.0f - std::exp(-std::max(p.dt, 0.0f) * std::max(p.adaptSpeed, 0.01f))
            : 1.0f;
        m_meter.setFloat("uBlend", blend);
        fsQuad.draw();
        m_adaptCur    = next;
        m_adaptPrimed = true;
        readBackMeter(p);
    }

    // --- Composite: bloom + god rays + lens flare + tonemap -----------------
    // Project the sun to screen space for the rays/flare.
    const glm::vec4 sunClip = p.viewProj * glm::vec4(p.camPos + p.sunDir * 3000.0f, 1.0f);
    glm::vec2 sunUV(0.5f);
    float sunOnScreen = 0.0f;
    if (sunClip.w > 1e-4f) {
        sunUV = glm::vec2(sunClip) / sunClip.w * 0.5f + 0.5f;
        if (sunUV.x > -0.3f && sunUV.x < 1.3f &&
            sunUV.y > -0.3f && sunUV.y < 1.3f && p.sunDir.y > -0.05f) {
            sunOnScreen = 1.0f;
        }
    }

    m_postRT.bind();
    gpu.clear(rhi::ClearColor | rhi::ClearDepth);
    gpu.setDepthTest(false);
    gpu.setDepthWrite(false);
    gpu.setCullEnabled(false);
    m_composite.bind();
    scene->bindColorTexture(0);   m_composite.setInt("uHdr", 0);
    hdr.bindDepthTexture(1);   m_composite.setInt("uDepth", 1);
    m_composite.setFloat("uNear", p.nearPlane);
    m_composite.setFloat("uFar", p.farPlane);
    m_composite.setInt("uOrtho", p.ortho ? 1 : 0);
    m_composite.setFloat("uFocusNear", p.dofNear);
    m_composite.setFloat("uFocusFar", p.dofFar);
    m_composite.setFloat("uDofMax", p.dofMax);
    m_ssaoBlurRT.bindColorTexture(2);
    m_composite.setInt("uAO", 2);
    m_composite.setFloat("uAoStrength", p.ssaoStrength);
    // The bloom pyramid's top level: already thresholded and blurred, so the
    // composite only samples it (for the glow and the god rays).
    if (!m_bloom.empty()) m_bloom[0].bindColorTexture(3);
    m_composite.setInt("uBloomTex", 3);
    m_composite.setVec2("uTexel", {1.0f / m_w, 1.0f / m_h});
    m_composite.setFloat("uAspect", p.aspect);
    m_composite.setFloat("uExposure", p.exposure);
    m_composite.setVec2("uSunUV", sunUV);
    m_composite.setFloat("uSunOnScreen", sunOnScreen);
    m_composite.setVec3("uSunColor", p.sunCol);
    m_composite.setFloat("uBloom", p.bloomIntensity);
    m_composite.setFloat("uRays", p.rayIntensity);
    m_composite.setFloat("uHueShift", p.hueShift);
    m_composite.setFloat("uSaturation", p.saturation);
    m_composite.setFloat("uValue", p.valueGain);
    m_composite.setFloat("uWarmth", p.warmth);
    m_composite.setFloat("uContrast", p.contrast);
    m_composite.setFloat("uSplit", p.split);
    m_composite.setFloat("uVibrance", p.vibrance);
    m_composite.setInt("uCurve", p.curve);
    applySunShadows(m_composite, p.shadows, p.camPos, p.viewForward);
    m_composite.setFloat("uAoSunlitShare", p.shadows ? p.aoSunlitShare : 1.0f);
    m_composite.setVec3("uAoSunDir", p.sunDir);
    m_composite.setMat4("uInvViewProj", glm::inverse(p.viewProj));
    m_composite.setFloat("uVignette", p.vignette);
    m_composite.setFloat("uGrain", p.grain);
    m_composite.setFloat("uComic", p.comic);
    m_composite.setFloat("uComicLevels", p.comicLevels);
    m_composite.setFloat("uComicLine", p.comicLine);
    m_composite.setFloat("uComicDetail", p.comicDetail);
    m_composite.setFloat("uComicHalftone", p.comicHalftone);
    // The depth under the comic's ink was drawn at the jittered point.
    m_composite.setVec2("uComicJitter", p.taa ? p.jitterUV : glm::vec2(0.0f));
    // The shader hashes it as an integer (pcg3d), so any count will do.
    m_composite.setInt("uFrame", static_cast<int>(p.frame & 0x7fffffffu));
    m_adapt[m_adaptCur].bindColorTexture(4);
    m_composite.setInt("uAdapt", 4);
    m_composite.setInt("uAutoExposure", p.autoExposure ? 1 : 0);
    m_composite.setFloat("uAutoRef", kAutoReferenceLog2);
    m_composite.setFloat("uAutoMinEv", std::min(p.autoMinEv, 0.0f));
    m_composite.setFloat("uAutoMaxEv", std::max(p.autoMaxEv, 0.0f));
    fsQuad.draw();
    m_result = &m_postRT;

    // --- Radial speed blur, centred on the followed craft -------------------
    // The world streaks outward past a craft that stays sharp. Only while
    // something is being followed and moving; a free camera gets none.
    if (p.blurAnchorValid && p.blurStrength > 0.002f) {
        glm::vec2 center(0.5f, 0.5f);
        const glm::vec4 cc = p.viewProj * glm::vec4(p.blurAnchor, 1.0f);
        if (cc.w > 1e-4f) center = glm::vec2(cc) / cc.w * 0.5f + 0.5f;

        m_mbRT.bind();
        gpu.clear(rhi::ClearColor | rhi::ClearDepth);
        m_motionBlur.bind();
        m_postRT.bindColorTexture(0);  m_motionBlur.setInt("uImage", 0);
        hdr.bindDepthTexture(1);       m_motionBlur.setInt("uDepth", 1);
        m_motionBlur.setVec2("uCenter", center);
        m_motionBlur.setFloat("uAmount", std::min(p.blurStrength, 0.5f));
        m_motionBlur.setInt("uSamples", 16);
        m_motionBlur.setFloat("uNear", p.nearPlane);
        m_motionBlur.setFloat("uFar", p.farPlane);
        m_motionBlur.setInt("uOrtho", p.ortho ? 1 : 0);
        fsQuad.draw();
        m_result = &m_mbRT;
    }

    keepHistory(hdr, scene);
}

// Keep this frame for the next one's reflections: its depth (always a copy --
// the HDR target is cleared and redrawn before the next lit pass reads it) and
// its resolved colour (the TAA target already holds it; without TAA it is
// copied into one of those targets, which is idle then).
void PostChain::keepHistory(const fitzel::RenderTarget& hdr,
                            const fitzel::RenderTarget* scene) {
    rhi::Device& gpu = rhi::device();
    if (!m_prevDepthTex || m_prevDepthW != m_w || m_prevDepthH != m_h ||
        m_prevDepthStencil != hdr.hasStencil()) {
        // The same format as the HDR target's depth: a depth blit converts
        // nothing, and refuses to try.
        m_prevDepthStencil = hdr.hasStencil();
        rhi::TextureDesc d;
        d.format  = m_prevDepthStencil ? rhi::Format::Depth32FStencil8 : rhi::Format::Depth32F;
        d.width   = m_w;
        d.height  = m_h;
        d.sampler = rhi::SamplerDesc::nearestClamp();
        const rhi::TextureHandle old = m_prevDepthTex;
        m_prevDepthTex = gpu.createTexture(d);
        // Depth only: no colour attachment, so nothing is drawn or read but depth.
        rhi::FramebufferDesc fb;
        fb.depth = rhi::Attachment{m_prevDepthTex};
        if (!m_prevDepthFbo) m_prevDepthFbo = gpu.createFramebuffer(fb);
        else gpu.setAttachments(m_prevDepthFbo, fb);
        if (old) gpu.destroy(old);
        m_prevDepthW = m_w;
        m_prevDepthH = m_h;
    }
    const rhi::Rect all{0, 0, m_w, m_h};
    gpu.blit(hdr.framebuffer(), all, m_prevDepthFbo, all, rhi::BlitDepth, rhi::Filter::Nearest);
    if (scene == &hdr) {
        gpu.blit(hdr.framebuffer(), all, m_taaRT[0].framebuffer(), all, rhi::BlitColor,
                 rhi::Filter::Nearest);
        m_historyColor = m_taaRT[0].colorTexture();
    } else {
        m_historyColor = scene->colorTexture();
    }
    gpu.bindFramebuffer({});
    m_historyValid = true;
}

// Copy this frame's meter pixel into a ring of read-back buffers and read the
// copies the card has finished. A plain readPixels here would stall the CPU on
// the whole frame queued in front of it, every frame, for one number.
//
// "Finished" is checked, not assumed: with vsync off the driver lets the CPU
// run several frames ahead, and reading a buffer the card has not reached yet
// waits for it -- the first version (map whatever was written two frames ago)
// put the frame's entire GPU time into the CPU's submit zone. Each copy carries
// a fence; a slot is read only once its fence has signalled, and a slot whose
// copy is still in flight is not overwritten (the meter reports a frame later).
void PostChain::readBackMeter(const Params& p) {
    if (!m_meterPbo[0]) return;
    rhi::Device& gpu = rhi::device();
    // WebGL2 guarantees RGBA/FLOAT as a read format of a float target (with
    // EXT_color_buffer_float), not the target's own HALF_FLOAT; the desktop
    // reads the target's own half floats (see the copy below).
    const bool web = gpu.caps().web;

    // Harvest every copy that has landed, oldest first, so the value kept is
    // the newest one available.
    for (int k = 0; k < kMeterRing; ++k) {
        const int i = (m_meterFrame + k) % kMeterRing;   // oldest slot first
        rhi::FenceHandle& f = m_meterFence[i];
        if (!f) continue;
        if (!gpu.fenceSignaled(f, 0)) continue;
        gpu.destroy(f);
        f = {};
        // After the fence above the copy costs no wait.
        float v[2];
        if (web) {
            float rgba[4] = {};
            gpu.readBuffer(m_meterPbo[i], 0, sizeof(rgba), rgba);
            v[0] = rgba[0];
            v[1] = rgba[1];
        } else {
            std::uint16_t h[4] = {};
            gpu.readBuffer(m_meterPbo[i], 0, sizeof(h), h);
            v[0] = halfToFloat(h[0]);
            v[1] = halfToFloat(h[1]);
        }
        if (std::isfinite(v[0]) && std::isfinite(v[1])) {
            m_meterLog2 = v[1];
            // composite.frag's autoExposure(), including its 0.7.
            const float ev = std::clamp((kAutoReferenceLog2 - v[0]) * 0.7f,
                                        std::min(p.autoMinEv, 0.0f),
                                        std::max(p.autoMaxEv, 0.0f));
            m_autoScale = p.autoExposure ? std::exp2(ev) : 1.0f;
        }
    }

    // This frame's copy, into the next slot -- unless that slot's previous
    // copy is still in flight, in which case this frame simply is not read.
    const int write = m_meterFrame % kMeterRing;
    if (!m_meterFence[write]) {
        // RGBA16F, the target's own format: asked for floats, the driver
        // converts on the way out and that conversion is synchronous -- the
        // stall this whole ring exists to avoid came straight back.
        gpu.readPixelsToBuffer(m_adapt[m_adaptCur].framebuffer(), 0, rhi::Rect{0, 0, 1, 1},
                               web ? rhi::Format::RGBA32F : rhi::Format::RGBA16F,
                               m_meterPbo[write], 0);
        m_meterFence[write] = gpu.createFence();
        m_meterFrame = (m_meterFrame + 1) % kMeterRing;
    }
}

void PostChain::present(fitzel::Mesh& fsQuad, bool fxaaEnabled, float sharpen,
                        bool upscale) {
    if (!m_result) return;   // run() has not produced anything yet
    rhi::Device& gpu = rhi::device();
    m_fxaa.bind();
    m_result->bindColorTexture(0);
    m_fxaa.setInt("uImage", 0);
    m_fxaa.setFloat("uSharpen", sharpen);
    // The source is one pane, not the window.
    m_fxaa.setVec2("uTexel", {1.0f / m_w, 1.0f / m_h});
    m_fxaa.setInt("uEnabled", fxaaEnabled ? 1 : 0);
    m_fxaa.setInt("uUpscale", upscale ? 1 : 0);
    fsQuad.draw();
    gpu.setDepthWrite(true);
    gpu.setDepthTest(true);
    gpu.setCullEnabled(true);
}
