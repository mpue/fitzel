#include "GlassDrops.hpp"

#include <algorithm>
#include <cmath>

#include <fitzel/graphics/Texture.hpp>
#include <fitzel/rhi/Device.hpp>

#include "Component.hpp"
#include "LensDrops.hpp"
#include "SceneGraph.hpp"

namespace {
constexpr int   kMaxFilm   = 2048;   // texels along a pane's longer side
constexpr float kBeadTexel = 7.0f;   // texels across an average bead
}

struct GlassDrops::Pane {
    LensDrops       sim;
    fitzel::Texture tex;             // the composed film (RGBA16F)
    fitzel::rhi::FramebufferHandle fbo;
    int             w = 0, h = 0;    // its size
    float           acc = 0.0f;      // beads owed
    bool            seen = false;
    GlassDropsView  view;
    ~Pane() { fitzel::rhi::device().destroy(fbo); }
};

GlassDrops::GlassDrops() = default;
GlassDrops::~GlassDrops() = default;

void GlassDrops::clear() { m_panes.clear(); }

const GlassDropsView* GlassDrops::of(int id) const {
    auto it = m_panes.find(id);
    if (it == m_panes.end() || !it->second->view.tex) return nullptr;
    return &it->second->view;
}

void GlassDrops::update(const std::vector<Entity>& entities, float dt, fitzel::Mesh& fsQuad,
                        fitzel::AssetDatabase& assets) {
    const bool any = std::any_of(entities.begin(), entities.end(), [](const Entity& e) {
        return e.activeInHierarchy && e.components.get<GlassDropsComponent>();
    });
    if (!any) { m_panes.clear(); return; }
    for (auto& [id, p] : m_panes) p->seen = false;

    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    const fitzel::rhi::FramebufferHandle prevFbo = gpu.boundFramebuffer();
    const fitzel::rhi::Rect              prevVp  = gpu.viewport();
    const fitzel::rhi::RenderState       prevState = gpu.renderState();

    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    auto rnd = [&](float a, float b) { return a + (b - a) * U(m_rng); };
    dt = std::clamp(dt, 0.0f, 0.1f);

    for (const Entity& e : entities) {
        const auto* gc = e.components.get<GlassDropsComponent>();
        if (!gc || !e.activeInHierarchy) continue;

        // The pane: the box's thinnest side is the glass; of the other two, the
        // one nearer upright runs down it.
        const glm::mat4 m = scenegraph::compose(e.center, e.rotation, glm::vec3(1.0f));
        const glm::vec3 ax[3] = {glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])),
                                 glm::normalize(glm::vec3(m[2]))};
        int thin = 0;
        for (int i = 1; i < 3; ++i) if (e.half[i] < e.half[thin]) thin = i;
        int up = thin == 0 ? 1 : 0, across = 3 - thin - up;
        if (std::abs(ax[across].y) > std::abs(ax[up].y)) std::swap(up, across);
        const glm::vec3 upDir = ax[up].y < 0.0f ? -ax[up] : ax[up];
        const glm::vec3 acrossDir = ax[across];
        const float W = 2.0f * e.half[across], H = 2.0f * e.half[up];
        if (W <= 1e-4f || H <= 1e-4f) continue;

        const float size = std::max(gc->dropSize, 1e-4f);
        const float perUnit = std::min(kBeadTexel / size, kMaxFilm / std::max(W, H));
        const int fw = std::max(8, static_cast<int>(W * perUnit));
        const int fh = std::max(8, static_cast<int>(H * perUnit));

        auto& slot = m_panes[e.id];
        if (!slot) {
            slot = std::make_unique<Pane>();
            if (!slot->sim.init()) { m_panes.erase(e.id); continue; }
        }
        Pane& p = *slot;
        p.seen = true;
        if (p.w != fw || p.h != fh) {
            // RGBA16F in place of blank()'s usual RGBA8: thickness runs past 1,
            // and its slope is what bends the light.
            p.tex = fitzel::Texture::blank(fw, fh, fitzel::rhi::Format::RGBA16F);
            fitzel::rhi::FramebufferDesc fb;
            fb.color[0] = fitzel::rhi::Attachment{p.tex.id()};
            if (!p.fbo) p.fbo = gpu.createFramebuffer(fb);
            else gpu.setAttachments(p.fbo, fb);
            gpu.bindFramebuffer(p.fbo);
            p.sim.clear();
            p.w = fw; p.h = fh;
        }
        // Water: nothing much left behind when it is gone.
        p.sim.setDrying(gc->dryTime / std::log(3.0f), 0.015f);
        p.sim.setSweep(true);

        // What landed this frame. Sizes in shares of the pane's height.
        p.acc += gc->rate * dt;
        const int n = std::min(static_cast<int>(p.acc), 400);
        p.acc -= std::floor(p.acc);
        for (int i = 0; i < n; ++i) {
            const glm::vec2 pos(U(m_rng), U(m_rng));
            const float d = size * rnd(0.45f, 1.5f) / H;
            LensSplash s;
            s.pos = pos; s.round = true; s.drops = 0;
            if (U(m_rng) < gc->running) {
                // A bead that met enough others to be too heavy to hold on: it
                // runs, and takes the beads below with it.
                const float r = d * rnd(0.7f, 1.1f);
                p.sim.drop(pos, r, rnd(0.15f, 1.0f));
            } else {
                s.size = d; s.aspect = rnd(0.85f, 1.15f); s.amount = rnd(0.5f, 1.0f);
                p.sim.splash(s);
            }
        }

        // The simulation works in a film half the size it is told.
        p.sim.step(dt, fw * 2, fh * 2, fsQuad, assets);
        gpu.bindFramebuffer(p.fbo);
        gpu.setViewport(0, 0, fw, fh);
        if (p.sim.active()) {
            p.sim.compose(fsQuad);
        } else {
            gpu.setClearColor(glm::vec4(0.0f));
            gpu.clear(fitzel::rhi::ClearColor);
        }

        GlassDropsView& v = p.view;
        v.tex    = &p.tex;
        v.origin = e.center - acrossDir * (0.5f * W) + upDir * (0.5f * H);
        v.du     = acrossDir / W;
        v.dv     = -upDir / H;
        v.texel  = glm::vec2(1.0f / fw, 1.0f / fh);
        v.height = 0.35f * size;
        v.reach  = 30.0f * size;
    }
    for (auto it = m_panes.begin(); it != m_panes.end();)
        it = it->second->seen ? std::next(it) : m_panes.erase(it);

    gpu.bindFramebuffer(prevFbo);
    gpu.setViewport(prevVp);
    gpu.setRenderState(prevState);
}
