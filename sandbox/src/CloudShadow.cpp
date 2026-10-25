#include "CloudShadow.hpp"

#include <cmath>
#include <cstdio>

#include <fitzel/rhi/Device.hpp>

#include "FrameRender.hpp"

CloudShadow::~CloudShadow() {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    if (m_fbo) gpu.destroy(m_fbo);
    if (m_tex) gpu.destroy(m_tex);
}

void CloudShadow::disable() {
    cloudShadowInfo().on = false;
}

void CloudShadow::render(const Params& p, const std::function<void()>& drawQuad) {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    if (!m_tried) {
        m_tried  = true;
        m_shader = fitzel::Shader::fromFiles("assets/shaders/sky.vert",
                                             "assets/shaders/cloudshadow.frag");
        if (!m_shader.isValid()) std::fprintf(stderr, "Failed to load cloudshadow shader\n");
        fitzel::rhi::TextureDesc d;
        d.format  = fitzel::rhi::Format::R8;
        d.width   = kRes;
        d.height  = kRes;
        d.sampler = fitzel::rhi::SamplerDesc::linearClamp();
        m_tex = gpu.createTexture(d);
        fitzel::rhi::FramebufferDesc fb;
        fb.color[0] = fitzel::rhi::Attachment{m_tex};
        m_fbo = gpu.createFramebuffer(fb);
    }
    if (!m_shader.isValid() || p.sunDir.y < 0.03f) { disable(); return; }

    // The map's corner, snapped to its texels: a map that slid with the eye by
    // fractions of a texel would make every shadow edge crawl as you walk.
    const float texel = kSize / kRes;
    const glm::vec2 origin(std::floor((p.eye.x - kSize * 0.5f) / texel) * texel,
                           std::floor((p.eye.z - kSize * 0.5f) / texel) * texel);

    // Nothing saved and restored: this runs at the top of every frame, before
    // anything has state worth keeping. It leaves the frame's convention behind instead -- the
    // default framebuffer, depth test on, blending off, back faces culled --
    // and whatever draws next binds its own target and viewport (the shadow
    // cascades, here).
    gpu.bindFramebuffer(m_fbo);
    gpu.setViewport(0, 0, kRes, kRes);
    gpu.setDepthTest(false);
    gpu.setBlendEnabled(false);
    gpu.setCullEnabled(false);
    gpu.setDepthWrite(false);

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

    gpu.setDepthWrite(true);
    gpu.setDepthTest(true);
    gpu.setCullEnabled(true);
    gpu.bindFramebuffer({});

    // Left on its own unit for the whole frame: nothing else binds 31.
    gpu.bindTexture(kUnit, m_tex);

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
    fitzel::rhi::device().readTexture(m_tex, 0, -1, fitzel::rhi::Format::R8, px.data());
    out.resize(px.size());
    for (std::size_t i = 0; i < px.size(); ++i) out[i] = static_cast<float>(px[i]) / 255.0f;
    return true;
#endif
}
