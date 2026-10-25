#include "LensDrops.hpp"

#include <algorithm>
#include <cmath>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/rhi/Device.hpp>

#include "PostChain.hpp"

namespace {
using fitzel::rhi::BlendFactor;
constexpr int   kMaxDrops   = 64;
constexpr float kTrail      = 0.14f;   // thickness a running drop leaves behind it
constexpr float kHead       = 1.15f;   // thickness of a drop's head at its middle
}

bool LensDrops::init() {
    m_stampSh = fitzel::Shader::fromFiles("assets/shaders/lensstamp.vert", "assets/shaders/lensstamp.frag");
    m_drySh   = fitzel::Shader::fromFiles("assets/shaders/sky.vert", "assets/shaders/lensdry.frag");
    m_lensSh  = fitzel::Shader::fromFiles("assets/shaders/sky.vert", "assets/shaders/lens.frag");
    m_wipeSh  = fitzel::Shader::fromFiles("assets/shaders/sky.vert", "assets/shaders/lenswipe.frag");
    return m_stampSh.isValid() && m_drySh.isValid() && m_lensSh.isValid();
}

void LensDrops::splash(const LensSplash& s) {
    m_pending.push_back(s);
    m_active = true;
}

void LensDrops::drop(glm::vec2 pos, float r, float mass) {
    if (static_cast<int>(m_drops.size()) >= kMaxDrops) return;
    Drop d;
    d.pos = pos; d.r = r; d.mass = mass; d.pause = 0.0f;
    m_drops.push_back(d);
    m_active = true;
}

void LensDrops::clear() {
    m_pending.clear();
    m_drops.clear();
    m_trails.clear();
    m_active = false;
    m_clearFilm = true;
}

void LensDrops::resize(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == m_w && h == m_h) return;
    m_w = w; m_h = h;
    using RT = fitzel::RenderTarget;
    const int fw = std::max(1, w / 2), fh = std::max(1, h / 2);
    m_film[0] = RT(fw, fh, RT::Format::RGBA16F);
    m_film[1] = RT(fw, fh, RT::Format::RGBA16F);
    m_heads   = RT(fw, fh, RT::Format::RGBA16F);
    m_clearFilm = true;     // a film of another size is no film
}

std::shared_ptr<fitzel::Texture> LensDrops::stamp(const std::string& name,
                                                  fitzel::AssetDatabase& assets) {
    if (name.empty()) return nullptr;
    auto it = m_stamps.find(name);
    if (it != m_stamps.end()) return it->second;
    // Resolved once per NAME (a miss too), as the particle sprites are.
    std::shared_ptr<fitzel::Texture> t;
    for (const fitzel::AssetId& aid : assets.allAssets()) {
        if (assets.typeForId(aid) != fitzel::AssetType::Texture) continue;
        const auto* ent = assets.entry(aid);
        if (!ent || ent->absPath.filename().string() != name) continue;
        t = assets.loadTexture(ent->absPath.string());
        break;
    }
    return m_stamps.emplace(name, std::move(t)).first->second;
}

void LensDrops::quad(fitzel::Mesh& fsQuad, glm::vec2 centerUv, glm::vec2 halfUv, float angleRad,
                     int mode, float amount, glm::vec2 channel) {
    // centre: 0..1 with y down the view -> NDC; half sizes in shares of the
    // view's height -> NDC-y units (the shader squeezes x by the aspect)
    m_stampSh.setVec2("uCenter", {centerUv.x * 2.0f - 1.0f, 1.0f - centerUv.y * 2.0f});
    m_stampSh.setVec2("uHalf", halfUv * 2.0f);
    m_stampSh.setVec2("uRot", {std::cos(angleRad), std::sin(angleRad)});
    m_stampSh.setInt("uMode", mode);
    m_stampSh.setFloat("uAmount", amount);
    m_stampSh.setVec2("uChannel", channel);
    fsQuad.draw();
}

void LensDrops::wipe(float a0, float a1, glm::vec2 pivot, float r0, float r1) {
    if (m_active) m_wipes.push_back({a0, a1, pivot, r0, r1});
}

void LensDrops::step(float dt, int w, int h, fitzel::Mesh& fsQuad, fitzel::AssetDatabase& assets) {
    if (!m_active) { m_wipes.clear(); return; }
    resize(w, h);
    dt = std::clamp(dt, 0.0f, 0.1f);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    auto rnd = [&](float a, float b) { return a + (b - a) * U(m_rng); };
    const float invAspect = static_cast<float>(m_h) / static_cast<float>(m_w);

    // --- the drops: run, catch, run on -- slower as they thin out --------------
    std::vector<Drop> settled;
    for (Drop& d : m_drops) {
        if (d.still) continue;
        d.pause -= dt;
        if (d.pause > 0.0f) continue;
        const float v = rnd(0.045f, 0.16f) * std::sqrt(d.r / 0.008f) *
                        std::clamp(d.mass / 0.11f, 0.35f, 1.0f);
        const glm::vec2 from = d.pos;
        d.pos.y += v * dt;
        d.pos.x += rnd(-0.002f, 0.002f) * invAspect;   // a slight wander
        d.mass  -= v * dt;
        m_trails.push_back({from, d.pos, d.r});
        if (U(m_rng) < dt * 1.3f) d.pause = rnd(0.15f, 0.9f);
        if (d.mass <= 0.0f || d.pos.y > 1.05f) { d.still = true; settled.push_back(d); }
    }
    m_drops.erase(std::remove_if(m_drops.begin(), m_drops.end(), [](const Drop& d) { return d.still; }),
                  m_drops.end());

    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    gpu.setDepthTest(false);
    gpu.setDepthWrite(false);
    gpu.setCullEnabled(false);

    // --- the film: dry it (into the other target), then add what came ----------
    const int nxt = 1 - m_cur;
    if (m_clearFilm) {
        for (auto& f : m_film) {
            f.bind();
            gpu.setClearColor(glm::vec4(0.0f));
            gpu.clear(fitzel::rhi::ClearColor);
        }
        m_clearFilm = false;
    }
    m_film[nxt].bind();
    gpu.setBlendEnabled(false);
    m_drySh.bind();
    m_film[m_cur].bindColorTexture(0);
    m_drySh.setInt("uFilm", 0);
    m_drySh.setFloat("uWetKeep", std::exp(-dt / std::max(m_wetTau, 0.01f)));
    m_drySh.setFloat("uDryGain", m_dryGain);
    fsQuad.draw();

    gpu.setBlendEnabled(true);
    gpu.setBlendFunc(BlendFactor::One, BlendFactor::One);
    m_stampSh.bind();
    m_stampSh.setFloat("uInvAspect", invAspect);
    m_stampSh.setInt("uTex", 0);
    for (const LensSplash& s : m_pending) {
        auto tex = stamp(s.image, assets);
        if (tex) tex->bind(0);
        const glm::vec2 half(s.size * 0.5f * s.aspect, s.size * 0.5f);
        quad(fsQuad, s.pos, half, glm::radians(s.angle), tex ? 0 : (s.round ? 1 : 3), s.amount,
             s.dry ? glm::vec2(0, 1) : glm::vec2(1, 0));
        if (s.dry) continue;
        // what gathers at its lower half and runs
        int n = s.drops >= 0 ? s.drops : 1 + static_cast<int>(s.size * 6.0f + U(m_rng) * 2.0f);
        for (int i = 0; i < n && static_cast<int>(m_drops.size()) < kMaxDrops; ++i) {
            Drop d;
            d.pos   = s.pos + glm::vec2(rnd(-0.28f, 0.28f) * s.size * invAspect, rnd(0.0f, 0.22f) * s.size);
            d.r     = s.size * rnd(0.022f, 0.045f) * std::clamp(s.amount, 0.4f, 1.5f);
            d.mass  = rnd(0.06f, 0.4f) * (d.r / 0.008f);
            d.pause = rnd(0.2f, 1.2f);
            m_drops.push_back(d);
        }
    }
    m_pending.clear();
    // the wet streaks behind the running drops
    for (const Trail& t : m_trails) {
        glm::vec2 a = t.from, b = t.to;
        glm::vec2 dPix((b.x - a.x) / invAspect, b.y - a.y);     // in height units
        const float len = glm::length(dPix);
        if (len < 1e-5f) continue;
        const float ang = std::atan2(-dPix.x, -dPix.y);          // local +y along it (y up in NDC)
        if (m_sweep) {
            // film *= 1 - what the drop takes with it
            gpu.setBlendFunc(BlendFactor::Zero, BlendFactor::OneMinusSrcColor);
            quad(fsQuad, (a + b) * 0.5f, {t.r * 0.95f, len * 0.5f + t.r * 0.3f}, ang, 2, 0.9f,
                 glm::vec2(1, 0));
            gpu.setBlendFunc(BlendFactor::One, BlendFactor::One);
        }
        quad(fsQuad, (a + b) * 0.5f, {t.r * 0.3f, len * 0.5f + t.r * 0.15f}, ang, 2, kTrail,
             glm::vec2(1, 0));
    }
    m_trails.clear();
    // drops that have stopped: their bead stays on the glass and dries there
    for (const Drop& d : settled)
        quad(fsQuad, d.pos, {d.r, d.r * 1.1f}, 0.0f, 1, kHead * 0.8f, glm::vec2(1, 0));
    // a wiper blade: the film under the sector it swept is (nearly) gone, and so are
    // the drops running there
    if (!m_wipes.empty() && m_wipeSh.isValid()) {
        gpu.setBlendFunc(BlendFactor::Zero, BlendFactor::SrcColor);
        m_wipeSh.bind();
        m_wipeSh.setFloat("uAspect", static_cast<float>(m_w) / static_cast<float>(std::max(m_h, 1)));
        m_wipeSh.setVec2("uKeep", {0.02f, 0.3f});
        for (const Wipe& wp : m_wipes) {
            m_wipeSh.setVec2("uPivot", wp.pivot);
            m_wipeSh.setFloat("uA0", wp.a0);
            m_wipeSh.setFloat("uA1", wp.a1);
            m_wipeSh.setFloat("uR0", wp.r0);
            m_wipeSh.setFloat("uR1", wp.r1);
            fsQuad.draw();
            const float aspect = static_cast<float>(m_w) / static_cast<float>(std::max(m_h, 1));
            const float lo = std::min(wp.a0, wp.a1) - 0.02f, hi = std::max(wp.a0, wp.a1) + 0.02f;
            m_drops.erase(std::remove_if(m_drops.begin(), m_drops.end(), [&](const Drop& d) {
                const glm::vec2 v((d.pos.x - wp.pivot.x) * aspect, d.pos.y - wp.pivot.y);
                const float r = glm::length(v), a = std::atan2(v.x, -v.y);
                return r >= wp.r0 && r <= wp.r1 && a >= lo && a <= hi;
            }), m_drops.end());
        }
        gpu.setBlendFunc(BlendFactor::One, BlendFactor::One);
        m_wipes.clear();
    }
    m_cur = nxt;

    // --- the heads of the running drops, fresh every frame ------------------------
    m_heads.bind();
    gpu.setClearColor(glm::vec4(0.0f));
    gpu.clear(fitzel::rhi::ClearColor);
    for (const Drop& d : m_drops)
        quad(fsQuad, d.pos, {d.r, d.r * 1.15f}, 0.0f, 1, kHead, glm::vec2(1, 0));
    gpu.setBlendEnabled(false);
}

void LensDrops::apply(PostChain& post, fitzel::Mesh& fsQuad) {
    if (!m_active || !post.result() || m_w != post.width() || m_h != post.height()) return;
    const fitzel::RenderTarget* src = post.result();
    if (m_out.width() != m_w || m_out.height() != m_h)
        m_out = fitzel::RenderTarget(m_w, m_h, fitzel::RenderTarget::Format::RGBA8);
    m_out.bind();
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    gpu.setDepthTest(false);
    gpu.setBlendEnabled(false);
    m_lensSh.bind();
    src->bindColorTexture(0);       m_lensSh.setInt("uImage", 0);
    m_film[m_cur].bindColorTexture(1); m_lensSh.setInt("uFilm", 1);
    m_heads.bindColorTexture(2);    m_lensSh.setInt("uHeads", 2);
    m_lensSh.setVec2("uTexel", {1.0f / m_w, 1.0f / m_h});
    m_lensSh.setVec2("uFilmTexel", {1.0f / m_film[m_cur].width(), 1.0f / m_film[m_cur].height()});
    m_lensSh.setVec3("uAbsorb", m_absorb);
    m_lensSh.setVec3("uDryTint", m_dryTint);
    fsQuad.draw();
    post.setResult(&m_out);
}

void LensDrops::compose(fitzel::Mesh& fsQuad) {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    gpu.setDepthTest(false);
    gpu.setDepthWrite(false);
    gpu.setCullEnabled(false);
    gpu.setBlendEnabled(false);
    // the film as it is (the drying pass, drying nothing)
    m_drySh.bind();
    m_film[m_cur].bindColorTexture(0);
    m_drySh.setInt("uFilm", 0);
    m_drySh.setFloat("uWetKeep", 1.0f);
    m_drySh.setFloat("uDryGain", 0.0f);
    fsQuad.draw();
    // and the heads over it
    gpu.setBlendEnabled(true);
    gpu.setBlendFunc(BlendFactor::One, BlendFactor::One);
    m_stampSh.bind();
    m_stampSh.setFloat("uInvAspect", static_cast<float>(m_h) / static_cast<float>(std::max(m_w, 1)));
    m_stampSh.setInt("uTex", 0);
    for (const Drop& d : m_drops)
        quad(fsQuad, d.pos, {d.r, d.r * 1.15f}, 0.0f, 1, kHead, glm::vec2(1, 0));
    gpu.setBlendEnabled(false);
}
