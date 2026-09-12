#include "FarTerrain.hpp"

#include <cmath>
#include <cstdio>

#include <glad/gl.h>

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
    for (Level& l : m_levels)
        if (l.tex) glDeleteTextures(1, &l.tex);
    if (m_ibo) glDeleteBuffers(1, &m_ibo);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

bool FarTerrain::init() {
    m_shader = fitzel::Shader::fromFiles("assets/shaders/farterrain.vert",
                                         "assets/shaders/farterrain.frag");
    if (!m_shader.isValid()) {
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

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)),
                 v.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glGenBuffers(1, &m_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(idx.size() * sizeof(std::uint32_t)),
                 idx.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);

    for (Level& l : m_levels) {
        glGenTextures(1, &l.tex);
        glBindTexture(GL_TEXTURE_2D, l.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, kGrid, kGrid, 0, GL_RG, GL_FLOAT,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
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
    std::vector<Result> done;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        done.swap(m_results);
    }
    for (Result& r : done) {
        Level& l = m_levels[r.level];
        if (r.stamp != m_stamp) continue;            // settings moved on
        glBindTexture(GL_TEXTURE_2D, l.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGrid, kGrid, GL_RG, GL_FLOAT,
                        r.data.data());
        l.origin = r.origin;
        l.valid  = true;
        if (r.origin == l.wanted) l.busy = false;
    }
    glBindTexture(GL_TEXTURE_2D, 0);

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
    const GLboolean cull = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);        // a mirrored view flips winding; a grid has no back
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

    m_shader.bind();
    m_shader.setMat4("uViewProj", proj * view);
    m_shader.setVec3("uCamPos", ctx.camPos);
    m_shader.setVec3("uLightDir", ctx.lightDir);
    m_shader.setVec3("uLightColor", ctx.lightColor);
    m_shader.setVec3("uAmbient", ctx.ambient);
    m_shader.setVec3("uFogColor", ctx.fogColor);
    m_shader.setVec3("uFogSunColor", ctx.fogSunColor);
    m_shader.setFloat("uFogDensity", ctx.fogDensity);
    m_shader.setFloat("uFogHeightFalloff", ctx.fogHeightFalloff);
    m_shader.setFloat("uFogHeight", ctx.fogHeight);
    m_shader.setFloat("uTime", static_cast<float>(ctx.time));
    m_shader.setFloat("uSnowLevel", snowLevel);
    m_shader.setFloat("uTreeLine", treeLine);
    m_shader.setFloat("uWaterLevel", waterLevel);
    m_shader.setVec3("uGrassTint", grassTint);
    m_shader.setVec3("uCanopy", canopy);
    applyCloudShadow(m_shader);
    ecology::forEachUniform(
        eco, [&](const char* n, int v) { m_shader.setInt(n, v); },
        [&](const char* n, float v) { m_shader.setFloat(n, v); });
    m_shader.setInt("uCutHole", mirror ? 1 : 0);
    m_shader.setFloat("uGrid", static_cast<float>(kGrid));
    m_shader.setInt("uHeight", 0);
    m_shader.setInt("uCoarse", 1);

    glBindVertexArray(m_vao);
    // Finest first: where two rings overlap at their seam the finer one is
    // already in the depth buffer and the coarser, pushed back a hair, loses.
    glEnable(GL_POLYGON_OFFSET_FILL);
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

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, l.tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, c.valid ? c.tex : l.tex);
        m_shader.setVec2("uOrigin", l.origin);
        m_shader.setFloat("uSize", l.size());
        m_shader.setVec2("uCoarseOrigin", c.valid ? c.origin : l.origin);
        m_shader.setFloat("uCoarseSize", c.valid ? c.size() : l.size());
        m_shader.setVec4("uHole", (hmax.x > hmin.x) ? hole : glm::vec4(1e9f, 1e9f, -1e9f, -1e9f));
        m_shader.setFloat("uSink", 40.0f + l.cell * 4.0f);
        m_shader.setFloat("uCell", l.cell);
        glPolygonOffset(1.0f, static_cast<float>(i) * 8.0f);
        glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (cull) glEnable(GL_CULL_FACE);
}
