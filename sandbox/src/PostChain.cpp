#include "PostChain.hpp"

#include <algorithm>
#include <cstdio>

#include <glad/gl.h>

namespace {
// Every pass here is a fullscreen triangle pair through the same trivial vertex
// shader; only the fragment stage differs. Named once so the list below reads as
// what it is: seven fragment programs over one quad.
constexpr const char* kVert = "assets/shaders/sky.vert";
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
    };
    for (const Load& l : loads) {
        *l.dst = fitzel::Shader::fromFiles(kVert, l.frag);
        if (!l.dst->isValid()) {
            std::fprintf(stderr, "Failed to load %s shader\n", l.what);
            return false;
        }
    }
    return true;
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

    m_bloom.clear();
    int lw = std::max(1, w / 2), lh = std::max(1, h / 2);
    for (int i = 0; i < 5 && lw >= 8 && lh >= 8; ++i) {
        m_bloom.emplace_back(lw, lh, RT::Format::RGBA16F);
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
    }
    m_result = nullptr;
}

void PostChain::run(const fitzel::RenderTarget& hdr, const Params& p,
                    fitzel::Mesh& fsQuad) {
    // Fullscreen passes: no depth, no culling, no blending unless a pass asks
    // for it (the bloom upsample does).
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

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
            hdr.bindColorTexture(0);
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
    hdr.bindColorTexture(0);   m_composite.setInt("uHdr", 0);
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
}

void PostChain::present(fitzel::Mesh& fsQuad, bool fxaaEnabled) {
    if (!m_result) return;   // run() has not produced anything yet
    m_fxaa.bind();
    m_result->bindColorTexture(0);
    m_fxaa.setInt("uImage", 0);
    // The source is one pane, not the window.
    m_fxaa.setVec2("uTexel", {1.0f / m_w, 1.0f / m_h});
    m_fxaa.setInt("uEnabled", fxaaEnabled ? 1 : 0);
    fsQuad.draw();
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}
