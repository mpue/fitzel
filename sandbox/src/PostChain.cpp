#include "PostChain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include <glad/gl.h>

#include "FrameRender.hpp"   // applySunShadows

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
    glGenBuffers(kMeterRing, m_meterPbo);
    for (unsigned pbo : m_meterPbo) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, 4 * sizeof(std::uint16_t), nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

PostChain::~PostChain() {
    if (m_prevDepthTex) glDeleteTextures(1, &m_prevDepthTex);
    if (m_prevDepthFbo) glDeleteFramebuffers(1, &m_prevDepthFbo);
    if (m_motionTex) glDeleteTextures(1, &m_motionTex);
    if (m_motionFbo) glDeleteFramebuffers(1, &m_motionFbo);
    for (void*& f : m_meterFence)
        if (f) { glDeleteSync(reinterpret_cast<GLsync>(f)); f = nullptr; }
    if (m_meterPbo[0]) glDeleteBuffers(kMeterRing, m_meterPbo);
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
    if (!m_motionFbo) glGenFramebuffers(1, &m_motionFbo);
    if (!m_motionTex || m_motionW != m_w || m_motionH != m_h) {
        if (!m_motionTex) glGenTextures(1, &m_motionTex);
        glBindTexture(GL_TEXTURE_2D, m_motionTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_w, m_h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_motionW = m_w;
        m_motionH = m_h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_motionFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_motionTex, 0);
    // The lit pass's own depth, so the motion is written only where the moving
    // surface is actually the one in front. Re-attached every frame: the HDR
    // target is reallocated whenever the pane changes size.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           hdr.depthTexture(), 0);
    glViewport(0, 0, m_w, m_h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_motionThisFrame = true;
}

void PostChain::run(const fitzel::RenderTarget& hdr, const Params& p,
                    fitzel::Mesh& fsQuad) {
    // Fullscreen passes: no depth, no culling, no blending unless a pass asks
    // for it (the bloom upsample does).
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

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
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_motionTex);
        m_taa.setInt("uMotion", 3);
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
    m_ssaoRT.bind();
    glClear(GL_COLOR_BUFFER_BIT);
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
    glClear(GL_COLOR_BUFFER_BIT);
    m_ssaoBlur.bind();
    m_ssaoRT.bindColorTexture(0);  m_ssaoBlur.setInt("uAO", 0);
    hdr.bindDepthTexture(1);       m_ssaoBlur.setInt("uDepth", 1);
    m_ssaoBlur.setVec2("uTexel", {1.0f / m_ssaoRT.width(), 1.0f / m_ssaoRT.height()});
    m_ssaoBlur.setFloat("uNear", p.nearPlane);
    m_ssaoBlur.setFloat("uFar", p.farPlane);
    m_ssaoBlur.setFloat("uDepthSigma", 0.4f);
    fsQuad.draw();

    // --- Bloom pyramid: threshold + downsample, then tent-upsample the levels
    //     back on top of each other (additive) -----------------------------
    for (std::size_t i = 0; i < m_bloom.size(); ++i) {
        m_bloom[i].bind();
        glClear(GL_COLOR_BUFFER_BIT);
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
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);   // each level adds onto the larger one
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
        glDisable(GL_BLEND);
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
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    m_composite.bind();
    scene->bindColorTexture(0);   m_composite.setInt("uHdr", 0);
    hdr.bindDepthTexture(1);   m_composite.setInt("uDepth", 1);
    m_composite.setFloat("uNear", p.nearPlane);
    m_composite.setFloat("uFar", p.farPlane);
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
    // Small and cycling: the hash in the shader loses its randomness on big
    // inputs, and a period of 61 frames is invisible.
    m_composite.setFloat("uFrameSeed", static_cast<float>(p.frame % 61u) * 1.618f);
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
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        m_motionBlur.bind();
        m_postRT.bindColorTexture(0);  m_motionBlur.setInt("uImage", 0);
        hdr.bindDepthTexture(1);       m_motionBlur.setInt("uDepth", 1);
        m_motionBlur.setVec2("uCenter", center);
        m_motionBlur.setFloat("uAmount", std::min(p.blurStrength, 0.5f));
        m_motionBlur.setInt("uSamples", 16);
        m_motionBlur.setFloat("uNear", p.nearPlane);
        m_motionBlur.setFloat("uFar", p.farPlane);
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
    if (!m_prevDepthFbo) glGenFramebuffers(1, &m_prevDepthFbo);
    if (!m_prevDepthTex || m_prevDepthW != m_w || m_prevDepthH != m_h) {
        if (!m_prevDepthTex) glGenTextures(1, &m_prevDepthTex);
        glBindTexture(GL_TEXTURE_2D, m_prevDepthTex);
        // The same format as the HDR target's depth: a depth blit converts
        // nothing, and refuses to try.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, m_w, m_h, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, m_prevDepthFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               m_prevDepthTex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        m_prevDepthW = m_w;
        m_prevDepthH = m_h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, hdr.framebuffer());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_prevDepthFbo);
    glBlitFramebuffer(0, 0, m_w, m_h, 0, 0, m_w, m_h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (scene == &hdr) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_taaRT[0].framebuffer());
        glBlitFramebuffer(0, 0, m_w, m_h, 0, 0, m_w, m_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        m_historyColor = m_taaRT[0].colorTexture();
    } else {
        m_historyColor = scene->colorTexture();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_historyValid = true;
}

// Copy this frame's meter pixel into a ring of pixel buffers and map the copies
// the card has finished. A plain glReadPixels here would stall the CPU on the
// whole frame queued in front of it, every frame, for one number.
//
// "Finished" is checked, not assumed: with vsync off the driver lets the CPU
// run several frames ahead, and mapping a buffer the card has not reached yet
// waits for it -- the first version (map whatever was written two frames ago)
// put the frame's entire GPU time into the CPU's submit zone. Each copy carries
// a fence; a slot is mapped only once its fence has signalled, and a slot whose
// copy is still in flight is not overwritten (the meter reports a frame later).
void PostChain::readBackMeter(const Params& p) {
    if (!m_meterPbo[0]) return;
    auto sync = [this](int i) -> GLsync& {
        return reinterpret_cast<GLsync&>(m_meterFence[i]);
    };

    // Harvest every copy that has landed, oldest first, so the value kept is
    // the newest one available.
    for (int k = 0; k < kMeterRing; ++k) {
        const int i = (m_meterFrame + k) % kMeterRing;   // oldest slot first
        GLsync& f = sync(i);
        if (!f) continue;
        const GLenum r = glClientWaitSync(f, 0, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) continue;
        glDeleteSync(f);
        f = nullptr;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_meterPbo[i]);
        if (const auto* h = static_cast<const std::uint16_t*>(
                glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4 * sizeof(std::uint16_t),
                                 GL_MAP_READ_BIT))) {
            const float v[2] = {halfToFloat(h[0]), halfToFloat(h[1])};
            if (std::isfinite(v[0]) && std::isfinite(v[1])) {
                m_meterLog2 = v[1];
                // composite.frag's autoExposure(), including its 0.7.
                const float ev = std::clamp((kAutoReferenceLog2 - v[0]) * 0.7f,
                                            std::min(p.autoMinEv, 0.0f),
                                            std::max(p.autoMaxEv, 0.0f));
                m_autoScale = p.autoExposure ? std::exp2(ev) : 1.0f;
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }

    // This frame's copy, into the next slot -- unless that slot's previous
    // copy is still in flight, in which case this frame simply is not read.
    const int write = m_meterFrame % kMeterRing;
    if (!sync(write)) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_meterPbo[write]);
        // HALF_FLOAT, the target's own format: asked for FLOAT, the driver
        // converts on the way out and that conversion is synchronous -- the
        // stall this whole ring exists to avoid came straight back.
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_HALF_FLOAT, nullptr);   // m_adapt[cur] is bound
        sync(write) = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        m_meterFrame = (m_meterFrame + 1) % kMeterRing;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void PostChain::present(fitzel::Mesh& fsQuad, bool fxaaEnabled, float sharpen) {
    if (!m_result) return;   // run() has not produced anything yet
    m_fxaa.bind();
    m_result->bindColorTexture(0);
    m_fxaa.setInt("uImage", 0);
    m_fxaa.setFloat("uSharpen", sharpen);
    // The source is one pane, not the window.
    m_fxaa.setVec2("uTexel", {1.0f / m_w, 1.0f / m_h});
    m_fxaa.setInt("uEnabled", fxaaEnabled ? 1 : 0);
    fsQuad.draw();
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}
