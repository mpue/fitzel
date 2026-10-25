#include "FarTerrain.hpp"

#include <string>

#include <cmath>
#include <cstdio>

#include <fitzel/rhi/Device.hpp>

namespace {

// A level is re-centred once the eye has wandered this fraction of its size
// from the middle. An eighth keeps the finest ring (2 km) at least ~770 m past
// the eye in every direction, beyond the streamed square it has to frame.
constexpr float kRecentre = 0.125f;

// Snap an origin to a multiple of two cells: the samples then sit on the same
// world lattice before and after a re-centre, so the silhouette on the horizon
// does not shimmer every time a ring moves.
glm::vec2 snappedOrigin(const glm::vec2& centre, float cell, int grid) {
    const float half = cell * static_cast<float>(grid - 1) * 0.5f;
    const float step = cell * 2.0f;
    return glm::vec2(std::floor((centre.x - half) / step) * step,
                     std::floor((centre.y - half) / step) * step);
}

} // namespace

FarTerrain::FarTerrain() {
    for (int i = 0; i < kLevels; ++i) m_levels[i].cell = 8.0f * static_cast<float>(1 << i);
    m_thread  = std::thread([this] { worker(); });
    m_thread2 = std::thread([this] { worker(); });
}

FarTerrain::~FarTerrain() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_thread2.joinable()) m_thread2.join();
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    for (Level& l : m_levels)
        if (l.tex) gpu.destroy(l.tex);
    if (m_shadowFbo) gpu.destroy(m_shadowFbo);
    if (m_shadowTex) gpu.destroy(m_shadowTex);
    if (m_vao) gpu.destroy(m_vao);
    if (m_ibo) gpu.destroy(m_ibo);
    if (m_vbo) gpu.destroy(m_vbo);
}

bool FarTerrain::init() {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    m_shader = fitzel::Shader::fromFiles("assets/shaders/farterrain.vert",
                                         "assets/shaders/farterrain.frag");
    {
        std::string frag = fitzel::Shader::readSource("assets/shaders/farterrain.frag");
        const std::size_t eol = frag.find('\n');
        frag.insert(eol == std::string::npos ? frag.size() : eol + 1, "#define CUT_HOLE\n");
        m_mirrorShader = fitzel::Shader::fromSource(
            fitzel::Shader::readSource("assets/shaders/farterrain.vert"), frag);
    }
    if (!m_shader.isValid() || !m_mirrorShader.isValid()) {
        std::fprintf(stderr, "Failed to load farterrain shader\n");
        return false;
    }
    // One unit grid, shared by every ring: (i, j) / (kGrid - 1).
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>(kGrid) * kGrid * 2);
    for (int j = 0; j < kGrid; ++j)
        for (int i = 0; i < kGrid; ++i) {
            v.push_back(static_cast<float>(i) / (kGrid - 1));
            v.push_back(static_cast<float>(j) / (kGrid - 1));
        }
    std::vector<std::uint32_t> idx;
    idx.reserve(static_cast<std::size_t>(kGrid - 1) * (kGrid - 1) * 6);
    for (int j = 0; j < kGrid - 1; ++j)
        for (int i = 0; i < kGrid - 1; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(j * kGrid + i);
            const std::uint32_t b = a + 1, c = a + kGrid, d = c + 1;
            // Split along the diagonal that alternates, so a slope cut by the
            // grid does not show one preferred direction of facets.
            if (((i + j) & 1) == 0) idx.insert(idx.end(), {a, c, b, b, c, d});
            else                    idx.insert(idx.end(), {a, c, d, a, d, b});
        }
    m_indexCount = static_cast<int>(idx.size());

    m_vbo = gpu.createBuffer(fitzel::rhi::BufferKind::Vertex, fitzel::rhi::BufferUsage::Static,
                             v.size() * sizeof(float), v.data());
    m_ibo = gpu.createBuffer(fitzel::rhi::BufferKind::Index, fitzel::rhi::BufferUsage::Static,
                             idx.size() * sizeof(std::uint32_t), idx.data());
    const fitzel::rhi::VertexAttribute grid{0, m_vbo, fitzel::rhi::VertexFormat::Float2, 0,
                                            2 * sizeof(float)};
    fitzel::rhi::VertexArrayDesc va;
    va.attributes     = &grid;
    va.attributeCount = 1;
    va.indexBuffer    = m_ibo;
    m_vao = gpu.createVertexArray(va);

    fitzel::rhi::TextureDesc td;
    td.format  = fitzel::rhi::Format::RG32F;
    td.width   = kGrid;
    td.height  = kGrid;
    td.sampler = fitzel::rhi::SamplerDesc::linearClamp();
    for (Level& l : m_levels) l.tex = gpu.createTexture(td);
    m_ready = true;
    return true;
}

bool FarTerrain::ready() const {
    return m_ready && enabled && m_present && m_levels[0].valid;
}

void FarTerrain::queue(int level, const glm::vec2& origin) {
    Level& l = m_levels[level];
    l.busy   = true;
    l.wanted = origin;
    l.stamp  = m_stamp;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // A newer request for the same ring supersedes one still waiting.
        for (auto it = m_jobs.begin(); it != m_jobs.end();)
            it = (it->level == level) ? m_jobs.erase(it) : it + 1;
        m_jobs.push_back({level, origin, l.cell, m_settings, m_stamp});
    }
    m_cv.notify_one();
}

void FarTerrain::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop) return;
            job = m_jobs.front();
            m_jobs.pop_front();
        }
        Result r;
        r.level  = job.level;
        r.origin = job.origin;
        r.stamp  = job.stamp;
        r.data.resize(static_cast<std::size_t>(kGrid) * kGrid * 2);
        for (int j = 0; j < kGrid; ++j) {
            if (m_stop) return;
            for (int i = 0; i < kGrid; ++i) {
                const float x = job.origin.x + i * job.cell;
                const float z = job.origin.y + j * job.cell;
                const std::size_t o = (static_cast<std::size_t>(j) * kGrid + i) * 2;
                r.data[o]     = fitzel::terrainHeight(job.settings, x, z);
                r.data[o + 1] = fitzel::terrainMoisture(job.settings, x, z);
            }
        }
        std::lock_guard<std::mutex> lk(m_mutex);
        m_results.push_back(std::move(r));
    }
}

void FarTerrain::update(const glm::vec3& eye, const fitzel::TerrainSettings& settings,
                        bool terrainPresent, const glm::vec2& nearMin,
                        const glm::vec2& nearMax) {
    if (!m_ready) return;
    m_present = terrainPresent;
    m_nearMin = nearMin;
    m_nearMax = nearMax;
    if (!enabled || !terrainPresent) return;

    if (!m_settingsValid || settings != m_settings) {
        m_settings      = settings;
        m_settingsValid = true;
        ++m_stamp;                                   // everything in flight is stale
        for (Level& l : m_levels) l.busy = false;
    }

    // Finished fields: upload the ones still wanted.
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    std::vector<Result> done;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        done.swap(m_results);
    }
    for (Result& r : done) {
        Level& l = m_levels[r.level];
        if (r.stamp != m_stamp) continue;            // settings moved on
        gpu.updateTexture(l.tex, fitzel::rhi::TextureRegion{}, fitzel::rhi::Format::RG32F,
                          r.data.data());
        l.origin = r.origin;
        l.valid  = true;
        ++m_fieldGen;
        if (r.origin == l.wanted) l.busy = false;
    }

    // Re-centre rings the eye has walked away from (or that were never filled,
    // or whose field predates the settings).
    const glm::vec2 e(eye.x, eye.z);
    for (int i = 0; i < kLevels; ++i) {
        Level& l = m_levels[i];
        const glm::vec2 centre = l.origin + glm::vec2(l.size() * 0.5f);
        const bool far   = glm::length(e - centre) > l.size() * kRecentre;
        const bool stale = !l.valid || (l.stamp != m_stamp && !l.busy);
        if (l.busy && l.stamp == m_stamp) continue;  // one in flight, current
        if (far || stale) queue(i, snappedOrigin(e, l.cell, kGrid));
    }
}

void FarTerrain::draw(const FrameContext& ctx, const glm::mat4& view,
                      const glm::mat4& proj, bool mirror) {
    if (!ready()) return;
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    // The caller's cull state comes back as culling on: every pass that draws
    // this culls.
    gpu.setCullEnabled(false);      // a mirrored view flips winding; a grid has no back
    gpu.setDepthTest(true);
    gpu.setDepthWrite(true);
    gpu.setDepthFunc(fitzel::rhi::CompareOp::Less);

    const fitzel::Shader& sh = mirror ? m_mirrorShader : m_shader;
    sh.bind();
    sh.setMat4("uViewProj", proj * view);
    sh.setVec3("uCamPos", ctx.camPos);
    sh.setVec3("uLightDir", ctx.lightDir);
    sh.setVec3("uLightColor", ctx.lightColor);
    sh.setVec3("uAmbient", ctx.ambient);
    sh.setVec3("uFogColor", ctx.fogColor);
    sh.setVec3("uFogSunColor", ctx.fogSunColor);
    sh.setFloat("uFogDensity", ctx.fogDensity);
    sh.setFloat("uFogHeightFalloff", ctx.fogHeightFalloff);
    sh.setFloat("uFogHeight", ctx.fogHeight);
    sh.setFloat("uOvercast", ctx.overcast);
    sh.setVec3("uOvercastColor", ctx.overcastColor);
    sh.setFloat("uTime", static_cast<float>(ctx.time));
    sh.setFloat("uSnowLevel", snowLevel);
    sh.setFloat("uTreeLine", treeLine);
    sh.setFloat("uWaterLevel", waterLevel);
    sh.setVec3("uGrassTint", grassTint);
    sh.setVec3("uCanopy", canopy);
    applyCloudShadow(sh);
    ecology::forEachUniform(
        eco, [&](const char* n, int v) { sh.setInt(n, v); },
        [&](const char* n, float v) { sh.setFloat(n, v); });
    sh.setInt("uCutHole", mirror ? 1 : 0);
    // Not in the mirror: the water's reflection has no water drawn over it,
    // and a sunk bed flipped over the plane would rise into the reflected sky.
    sh.setVec3("uOceanCut", glm::vec3(ctx.camPos.x, ctx.camPos.z, mirror ? 0.0f : oceanReach));
    sh.setFloat("uGrid", static_cast<float>(kGrid));
    sh.setInt("uHeight", 0);
    sh.setInt("uCoarse", 1);

    gpu.bindVertexArray(m_vao);
    // Finest first: where two rings overlap at their seam the finer one is
    // already in the depth buffer and the coarser, pushed back a hair, loses.
    for (int i = 0; i < kLevels; ++i) {
        const Level& l = m_levels[i];
        if (!l.valid) continue;
        // What this ring leaves to the one inside it: the streamed square for
        // the finest, the next finer ring otherwise. Shrunk by a cell and a half
        // so the ring's own boundary vertices stay put and the two surfaces
        // overlap rather than leave a gap.
        glm::vec2 hmin = m_nearMin, hmax = m_nearMax;
        if (i > 0 && m_levels[i - 1].valid) {
            hmin = m_levels[i - 1].origin;
            hmax = hmin + glm::vec2(m_levels[i - 1].size());
        }
        const float m = l.cell * 1.5f;
        const glm::vec4 hole(hmin + m, hmax - m);
        // The next coarser ring, for shadows cast from beyond this one's edge.
        const Level& c = m_levels[std::min(i + 1, kLevels - 1)];

        gpu.bindTexture(0, l.tex);
        gpu.bindTexture(1, c.valid ? c.tex : l.tex);
        sh.setVec2("uOrigin", l.origin);
        sh.setFloat("uSize", l.size());
        sh.setVec2("uCoarseOrigin", c.valid ? c.origin : l.origin);
        sh.setFloat("uCoarseSize", c.valid ? c.size() : l.size());
        sh.setVec4("uHole", (hmax.x > hmin.x) ? hole : glm::vec4(1e9f, 1e9f, -1e9f, -1e9f));
        sh.setFloat("uSink", 40.0f + l.cell * 4.0f);
        sh.setFloat("uCell", l.cell);
        gpu.setPolygonOffset(true, 1.0f, static_cast<float>(i) * 8.0f);
        gpu.drawIndexed(fitzel::rhi::Primitive::Triangles, m_indexCount,
                        fitzel::rhi::IndexType::U32);
    }
    gpu.setPolygonOffsetEnabled(false);
    gpu.bindVertexArray({});
    gpu.bindTexture(1, {});
    gpu.bindTexture(0, {});
    gpu.setCullEnabled(true);
}

void FarTerrain::renderSunShadow(const glm::vec3& sunDir, const glm::vec3& eye,
                                 const std::function<void()>& drawQuad) {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    CloudShadowInfo& ci = cloudShadowInfo();
    if (!m_shadowTried) {
        m_shadowTried  = true;
        m_shadowShader = fitzel::Shader::fromFiles("assets/shaders/sky.vert",
                                                   "assets/shaders/mtnshadow.frag");
        if (!m_shadowShader.isValid()) std::fprintf(stderr, "Failed to load mtnshadow shader\n");
        fitzel::rhi::TextureDesc td;
        td.format  = fitzel::rhi::Format::R32F;
        td.width   = kShadowRes;
        td.height  = kShadowRes;
        td.sampler = fitzel::rhi::SamplerDesc::linearClamp();
        m_shadowTex = gpu.createTexture(td);
        fitzel::rhi::FramebufferDesc fb;
        fb.color[0] = fitzel::rhi::Attachment{m_shadowTex};
        m_shadowFbo = gpu.createFramebuffer(fb);
    }
    if (!enabled || !ready() || !m_shadowShader.isValid() || sunDir.y <= 0.0f) {
        ci.mtnOn = false;
        return;
    }
    // Snapped to a coarse step, so walking about neither crawls the shadow's
    // edges nor re-marches the map every frame.
    const float step = 400.0f;
    const glm::vec2 origin(std::floor((eye.x - kShadowSize * 0.5f) / step) * step,
                           std::floor((eye.z - kShadowSize * 0.5f) / step) * step);
    const bool redo = origin != m_shadowOrigin || m_fieldGen != m_shadowGen ||
                      glm::dot(glm::normalize(sunDir), m_shadowSun) < 0.999999f;
    if (redo) {
        m_shadowOrigin = origin;
        m_shadowGen    = m_fieldGen;
        m_shadowSun    = glm::normalize(sunDir);

        const fitzel::rhi::FramebufferHandle prevFbo = gpu.boundFramebuffer();
        const fitzel::rhi::Rect vp = gpu.viewport();
        const bool depth = gpu.renderState().depth.test;
        const bool blend = gpu.renderState().blend.enabled;
        const bool cull  = gpu.renderState().raster.cull;
        gpu.bindFramebuffer(m_shadowFbo);
        gpu.setViewport(0, 0, kShadowRes, kShadowRes);
        gpu.setDepthTest(false);
        gpu.setBlendEnabled(false);
        gpu.setCullEnabled(false);
        gpu.setDepthWrite(false);

        m_shadowShader.bind();
        static const char* const kTex[kLevels]  = {"uL0", "uL1", "uL2", "uL3", "uL4"};
        static const char* const kRect[kLevels] = {"uR0", "uR1", "uR2", "uR3", "uR4"};
        for (int i = 0; i < kLevels; ++i) {
            const Level& l = m_levels[i];
            gpu.bindTexture(i, l.valid ? l.tex : fitzel::rhi::TextureHandle{});
            m_shadowShader.setInt(kTex[i], i);
            m_shadowShader.setVec4(kRect[i],
                                   glm::vec4(l.origin, l.size(), l.valid ? 1.0f : 0.0f));
        }
        m_shadowShader.setFloat("uGrid", static_cast<float>(kGrid));
        m_shadowShader.setVec2("uOrigin", origin);
        m_shadowShader.setFloat("uSize", kShadowSize);
        m_shadowShader.setVec3("uSunDir", m_shadowSun);
        drawQuad();
        for (int i = kLevels - 1; i >= 0; --i) gpu.bindTexture(i, {});

        gpu.setDepthWrite(true);
        if (depth) gpu.setDepthTest(true);
        if (blend) gpu.setBlendEnabled(true);
        if (cull)  gpu.setCullEnabled(true);
        gpu.bindFramebuffer(prevFbo);
        gpu.setViewport(vp);
    }
    // On its own unit for the whole frame; nothing else binds 29.
    gpu.bindTexture(kShadowUnit, m_shadowTex);
    ci.mtnOn     = true;
    ci.mtnOrigin = origin;
    ci.mtnSize   = kShadowSize;
}
