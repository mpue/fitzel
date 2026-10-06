#include "CloudShadow.hpp"

#include <cmath>
#include <cstdio>

#include <glad/gl.h>

#include "FrameRender.hpp"

CloudShadow::~CloudShadow() {
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    if (m_tex) glDeleteTextures(1, &m_tex);
}

void CloudShadow::disable() {
    cloudShadowInfo().on = false;
}

void CloudShadow::render(const Params& p, const std::function<void()>& drawQuad) {
    if (!m_tried) {
        m_tried  = true;
        m_shader = fitzel::Shader::fromFiles("assets/shaders/sky.vert",
                                             "assets/shaders/cloudshadow.frag");
        if (!m_shader.isValid()) std::fprintf(stderr, "Failed to load cloudshadow shader\n");
        glGenTextures(1, &m_tex);
        glBindTexture(GL_TEXTURE_2D, m_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kRes, kRes, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &m_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (!m_shader.isValid() || p.sunDir.y < 0.03f) { disable(); return; }

    // The map's corner, snapped to its texels: a map that slid with the eye by
    // fractions of a texel would make every shadow edge crawl as you walk.
    const float texel = kSize / kRes;
    const glm::vec2 origin(std::floor((p.eye.x - kSize * 0.5f) / texel) * texel,
                           std::floor((p.eye.z - kSize * 0.5f) / texel) * texel);

    // Nothing read back from GL: a glGet waits for everything queued so far
    // under the driver's threaded optimisation, and this runs at the top of
    // every frame. It leaves the frame's convention behind instead -- the
    // default framebuffer, depth test on, blending off, back faces culled --
    // and whatever draws next binds its own target and viewport (the shadow
    // cascades, here).
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, kRes, kRes);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    m_shader.bind();
    m_shader.setFloat("uTime", p.time);
    m_shader.setFloat("uCoverage", p.coverage);
    m_shader.setFloat("uCloudDensity", p.density);
    m_shader.setFloat("uCloudScale", p.scale);
    m_shader.setFloat("uCloudSpeed", p.speed);
    m_shader.setFloat("uCloudBottom", p.bottom);
    m_shader.setFloat("uCloudTop", p.top);
    m_shader.setVec2("uOrigin", origin);
    m_shader.setFloat("uSize", kSize);
    m_shader.setFloat("uRefY", p.groundY);
    m_shader.setVec3("uSunDir", p.sunDir);
    drawQuad();

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Left on its own unit for the whole frame: nothing else binds 31.
    glActiveTexture(GL_TEXTURE0 + kUnit);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glActiveTexture(GL_TEXTURE0);

    CloudShadowInfo& ci = cloudShadowInfo();
    ci.on     = true;
    ci.origin = origin;
    ci.size   = kSize;
    ci.refY   = p.groundY;
    ci.sunDir = p.sunDir;
}

bool CloudShadow::readback(std::vector<float>& out) const {
    if (!m_tex || !cloudShadowInfo().on) return false;
#ifdef __EMSCRIPTEN__
    // Only the path tracer reads the clouds back, and it is editor-only.
    (void)out;
    return false;
#else
    std::vector<unsigned char> px(static_cast<std::size_t>(kRes) * kRes);
    GLint prevAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, prevAlign);
    out.resize(px.size());
    for (std::size_t i = 0; i < px.size(); ++i) out[i] = static_cast<float>(px[i]) / 255.0f;
    return true;
#endif
}
