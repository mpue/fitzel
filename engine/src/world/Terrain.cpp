#include "fitzel/world/Terrain.hpp"

#include <algorithm>
#include <cmath>

#include <stb_perlin.h>

namespace fitzel {

float terrainBaseHeight(const TerrainSettings& s, float worldX, float worldZ) {
    // Domain warp: displace the sample point by a low-frequency noise field so
    // features bend and meander instead of looking grid-aligned.
    const float wf = s.warpFrequency;
    const float wx = worldX + s.warpStrength *
        stb_perlin_fbm_noise3(worldX * wf, 0.0f, worldZ * wf, 2.0f, 0.5f, 4);
    const float wz = worldZ + s.warpStrength *
        stb_perlin_fbm_noise3((worldX + 101.7f) * wf, 0.0f,
                              (worldZ + 57.1f) * wf, 2.0f, 0.5f, 4);

    const float f  = s.frequency;
    const float bf = s.biomeFreq;

    // 1) Continents: big lowland basins (sea/lake floors) vs highlands.
    const float continent = stb_perlin_fbm_noise3(
        wx * bf + s.seed, 0.0f, wz * bf + s.seed, 2.0f, 0.5f, 4);
    const float baseElev = continent * s.heightScale * s.continentAmp;
    const float highland = glm::smoothstep(-0.15f, 0.45f, continent);

    // 2) Regional roughness: smooth plains in some regions, rugged in others.
    const float roughN = stb_perlin_fbm_noise3(
        (wx + 311.0f) * bf * 1.7f + s.seed, 0.0f,
        (wz + 117.0f) * bf * 1.7f + s.seed, 2.0f, 0.5f, 3);
    const float rough = glm::smoothstep(0.30f, 0.75f, roughN * 0.5f + 0.5f);

    // 3) Rolling hills (medium scale), stronger where it's rugged.
    const float hills = stb_perlin_fbm_noise3(
        wx * f + s.seed, 0.0f, wz * f + s.seed, s.lacunarity, s.gain, s.octaves)
        * s.heightScale * 0.5f * (0.3f + 0.7f * rough);

    // 4) Sharp ridged mountains, only on rugged highlands.
    const float ridge = stb_perlin_ridge_noise3(
        wx * f * 1.9f + s.seed, 0.0f, wz * f * 1.9f + s.seed,
        2.0f, 0.5f, 1.0f, s.octaves);
    const float mountains = ridge * s.ridgeScale * rough * highland;

    float h = baseElev + hills + mountains;

    // 5) Plateaus / terraces in some regions (soft-stepped).
    if (s.terrace > 0.001f) {
        const float tMask = glm::smoothstep(0.55f, 0.85f,
            stb_perlin_fbm_noise3((wx - 733.0f) * bf * 1.3f, 0.0f,
                                  (wz + 401.0f) * bf * 1.3f, 2.0f, 0.5f, 3) * 0.5f + 0.5f);
        const float step = 5.0f;
        const float q    = std::floor(h / step) * step;
        const float frac = (h - q) / step;
        const float soft = q + step * glm::smoothstep(0.35f, 0.65f, frac);
        h = glm::mix(h, soft, tMask * s.terrace);
    }

    return h;
}

float terrainMoisture(const TerrainSettings& s, float worldX, float worldZ) {
    const float mf = s.biomeFreq * 0.8f;
    const float m = stb_perlin_fbm_noise3(
        worldX * mf + 137.0f + s.seed, 0.0f, worldZ * mf + 91.0f + s.seed,
        2.0f, 0.5f, 3);
    return glm::clamp(m * 0.5f + 0.5f, 0.0f, 1.0f);
}

// --- Editable deformation layer -------------------------------------------

std::int64_t TerrainEditField::cellKey(int ix, int iz) {
    return (static_cast<std::int64_t>(ix) << 32) ^
           (static_cast<std::uint32_t>(iz));
}

float TerrainEditField::sample(float worldX, float worldZ) const {
    if (deltas.empty()) return 0.0f;
    const float gx = worldX / cell, gz = worldZ / cell;
    const int   ix = static_cast<int>(std::floor(gx));
    const int   iz = static_cast<int>(std::floor(gz));
    const float fx = gx - ix, fz = gz - iz;
    auto at = [&](int x, int z) -> float {
        const auto it = deltas.find(cellKey(x, z));
        return it == deltas.end() ? 0.0f : it->second;
    };
    const float h00 = at(ix,     iz),     h10 = at(ix + 1, iz);
    const float h01 = at(ix,     iz + 1), h11 = at(ix + 1, iz + 1);
    return glm::mix(glm::mix(h00, h10, fx), glm::mix(h01, h11, fx), fz);
}

// Iterate the grid cells whose centre lies inside the brush disc, invoking
// `fn(ix, iz, worldX, worldZ, weight)` with a smooth 1-at-centre..0-at-rim
// weight. Shared by all three sculpt ops.
template <class Fn>
static void forBrushCells(float cell, glm::vec2 c, float radius, Fn&& fn) {
    if (radius <= 0.0f) return;
    const int x0 = static_cast<int>(std::floor((c.x - radius) / cell));
    const int x1 = static_cast<int>(std::ceil ((c.x + radius) / cell));
    const int z0 = static_cast<int>(std::floor((c.y - radius) / cell));
    const int z1 = static_cast<int>(std::ceil ((c.y + radius) / cell));
    const float r2 = radius * radius;
    for (int iz = z0; iz <= z1; ++iz) {
        for (int ix = x0; ix <= x1; ++ix) {
            const float wx = ix * cell, wz = iz * cell;
            const float d2 = (wx - c.x) * (wx - c.x) + (wz - c.y) * (wz - c.y);
            if (d2 > r2) continue;
            const float t = 1.0f - std::sqrt(d2) / radius; // 1 centre -> 0 rim
            fn(ix, iz, wx, wz, t * t * (3.0f - 2.0f * t));  // smoothstep weight
        }
    }
}

void TerrainEditField::raise(glm::vec2 c, float radius, float amount) {
    forBrushCells(cell, c, radius, [&](int ix, int iz, float, float, float w) {
        deltas[cellKey(ix, iz)] += amount * w;
    });
}

void TerrainEditField::flatten(const TerrainSettings& s, glm::vec2 c,
                               float radius, float amount, float target) {
    forBrushCells(cell, c, radius, [&](int ix, int iz, float wx, float wz, float w) {
        const std::int64_t k = cellKey(ix, iz);
        const auto  it  = deltas.find(k);
        const float cur = (it == deltas.end()) ? 0.0f : it->second;
        const float combined = terrainBaseHeight(s, wx, wz) + cur;
        // Blend the surface toward the target height; store the resulting offset.
        deltas[k] = cur + (target - combined) * glm::clamp(amount * w, 0.0f, 1.0f);
    });
}

void TerrainEditField::smooth(const TerrainSettings& s, glm::vec2 c,
                              float radius, float amount) {
    // Buffer the new offsets so cells within one dab don't smear into each other
    // (every cell reads the pre-dab surface for its neighbourhood average).
    struct Upd { std::int64_t k; float v; };
    std::vector<Upd> ups;
    forBrushCells(cell, c, radius, [&](int ix, int iz, float wx, float wz, float w) {
        float sum = 0.0f; int n = 0;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const float nx = (ix + dx) * cell, nz = (iz + dz) * cell;
                sum += terrainBaseHeight(s, nx, nz) + sample(nx, nz);
                ++n;
            }
        const float base     = terrainBaseHeight(s, wx, wz);
        const float combined = base + sample(wx, wz);
        const float target   = glm::mix(combined, sum / n,
                                        glm::clamp(amount * w, 0.0f, 1.0f));
        ups.push_back({cellKey(ix, iz), target - base});
    });
    for (const Upd& u : ups) deltas[u.k] = u.v;
}

// --- Global deformation snapshot ------------------------------------------

namespace {
std::mutex                                g_editMutex;
std::shared_ptr<const TerrainEditField>   g_edits;
} // namespace

std::shared_ptr<const TerrainEditField> terrainEditSnapshot() {
    std::lock_guard<std::mutex> lock(g_editMutex);
    return g_edits;
}

void setTerrainEditSnapshot(std::shared_ptr<const TerrainEditField> field) {
    std::lock_guard<std::mutex> lock(g_editMutex);
    g_edits = std::move(field);
}

float terrainHeight(const TerrainSettings& s, float worldX, float worldZ) {
    float h = terrainBaseHeight(s, worldX, worldZ);
    if (const auto e = terrainEditSnapshot()) h += e->sample(worldX, worldZ);
    return h;
}

MeshData TerrainChunk::buildMeshData(const TerrainSettings& s, glm::ivec2 coord) {
    MeshData data;

    const int   verts   = std::max(2, s.resolution + 1);
    const float step    = s.chunkSize / static_cast<float>(s.resolution);
    const float originX = coord.x * s.chunkSize;
    const float originZ = coord.y * s.chunkSize;

    // Grab the edit snapshot once for the whole chunk (avoids a mutex-guarded
    // shared_ptr load per vertex); sample base noise + this chunk's edits.
    const std::shared_ptr<const TerrainEditField> edits = terrainEditSnapshot();
    auto height = [&](float wx, float wz) {
        float h = terrainBaseHeight(s, wx, wz);
        if (edits) h += edits->sample(wx, wz);
        return h;
    };

    data.vertices.reserve(static_cast<std::size_t>(verts) * verts);
    for (int z = 0; z < verts; ++z) {
        for (int x = 0; x < verts; ++x) {
            const float wx = originX + x * step;
            const float wz = originZ + z * step;

            // Central differences in world space (continuous across chunks).
            const float hl = height(wx - step, wz);
            const float hr = height(wx + step, wz);
            const float hd = height(wx, wz - step);
            const float hu = height(wx, wz + step);
            const glm::vec3 normal =
                glm::normalize(glm::vec3(hl - hr, 2.0f * step, hd - hu));

            Vertex v;
            v.position = {wx, height(wx, wz), wz};
            v.normal   = normal;
            v.uv       = {static_cast<float>(x) / s.resolution,
                          static_cast<float>(z) / s.resolution};
            data.vertices.push_back(v);
        }
    }

    data.indices.reserve(static_cast<std::size_t>(s.resolution) * s.resolution * 6);
    for (int z = 0; z < verts - 1; ++z) {
        for (int x = 0; x < verts - 1; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(z * verts + x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + verts;
            const std::uint32_t i3 = i2 + 1;
            data.indices.insert(data.indices.end(),
                                {i0, i2, i1, i1, i2, i3});
        }
    }

    return data;
}

TerrainChunk TerrainChunk::fromData(glm::ivec2 coord, const MeshData& data) {
    TerrainChunk chunk;
    chunk.m_coord = coord;
    chunk.m_mesh  = Mesh::create(data);
    return chunk;
}

TerrainChunk TerrainChunk::generate(const TerrainSettings& s, glm::ivec2 coord) {
    return fromData(coord, buildMeshData(s, coord));
}

// --- TerrainStreamer -------------------------------------------------------

TerrainStreamer::TerrainStreamer(const TerrainSettings& settings, int radius)
    : m_settings(settings), m_radius(std::max(0, radius)) {
    unsigned hw = std::thread::hardware_concurrency();
    unsigned count = (hw > 2) ? hw - 1 : 1;
    count = std::min(count, 4u);
    for (unsigned i = 0; i < count; ++i) {
        m_workers.emplace_back([this] { workerLoop(); });
    }
}

TerrainStreamer::~TerrainStreamer() {
    m_stop.store(true);
    m_jobCv.notify_all();
    for (std::thread& t : m_workers) {
        if (t.joinable()) t.join();
    }
}

std::int64_t TerrainStreamer::key(glm::ivec2 c) {
    return (static_cast<std::int64_t>(c.x) << 32) ^
           (static_cast<std::uint32_t>(c.y));
}

glm::ivec2 TerrainStreamer::coordOf(std::int64_t k) {
    return {static_cast<int>(k >> 32),
            static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(k)))};
}

glm::ivec2 TerrainStreamer::chunkCoordOf(const glm::vec3& pos) const {
    return {static_cast<int>(std::floor(pos.x / m_settings.chunkSize)),
            static_cast<int>(std::floor(pos.z / m_settings.chunkSize))};
}

bool TerrainStreamer::inRange(glm::ivec2 c, glm::ivec2 center) const {
    return std::abs(c.x - center.x) <= m_radius &&
           std::abs(c.y - center.y) <= m_radius;
}

void TerrainStreamer::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [this] { return m_stop.load() || !m_jobs.empty(); });
            if (m_stop.load()) return;
            job = std::move(m_jobs.front());
            m_jobs.pop();
        }
        MeshData data = TerrainChunk::buildMeshData(job.settings, job.coord);
        {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_results.push({job.coord, job.generation, job.editVersion, std::move(data)});
        }
    }
}

int TerrainStreamer::update(const glm::vec3& cameraPos, int maxUploads) {
    const glm::ivec2 center = chunkCoordOf(cameraPos);

    // 1) Upload finished chunks (render thread). Bounded per frame.
    int uploaded = 0;
    std::vector<Result> ready;
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        while (!m_results.empty() && static_cast<int>(ready.size()) < maxUploads) {
            ready.push_back(std::move(m_results.front()));
            m_results.pop();
        }
    }
    for (Result& r : ready) {
        const std::int64_t k = key(r.coord);
        m_pending.erase(k);
        const bool stale = (r.generation != m_generation) || !inRange(r.coord, center);
        if (!stale) {
            // insert_or_assign so a sculpt rebuild swaps the mesh in place (the
            // old one kept rendering until now -> no hole). The replaced Mesh's
            // GL resources free here on the render thread, which is correct.
            m_chunks.insert_or_assign(k, TerrainChunk::fromData(r.coord, r.data));
            m_chunkEdit[k] = r.editVersion;
            ++uploaded;
        }
        // stale / out-of-range results are simply dropped.
    }

    bool changed = uploaded > 0;

    // 2) When the camera changes chunk (or after a rebuild), refresh the desired
    //    set: drop far chunks and queue any missing ones.
    if (center != m_center || m_dirty) {
        m_center = center;
        m_dirty  = false;
        changed  = true;

        for (auto it = m_chunks.begin(); it != m_chunks.end();) {
            if (!inRange(it->second.coord(), center)) {
                m_chunkEdit.erase(key(it->second.coord()));
                it = m_chunks.erase(it);
            } else ++it;
        }

        std::vector<Job> newJobs;
        for (int dz = -m_radius; dz <= m_radius; ++dz) {
            for (int dx = -m_radius; dx <= m_radius; ++dx) {
                const glm::ivec2 c{center.x + dx, center.y + dz};
                const std::int64_t k = key(c);
                if (m_chunks.find(k) == m_chunks.end() &&
                    m_pending.find(k) == m_pending.end()) {
                    m_pending.insert(k);
                    newJobs.push_back({c, m_settings, m_generation, m_editVersion});
                }
            }
        }
        if (!newJobs.empty()) {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            for (Job& j : newJobs) m_jobs.push(std::move(j));
            m_jobCv.notify_all();
        }
    }

    // 3) Sculpt rebuilds: re-queue loaded chunks whose mesh predates the latest
    //    edit. A chunk stays dirty until a build that ran against the current
    //    edit version lands, so a stroke always converges to the final shape.
    if (!m_editDirty.empty()) {
        std::vector<Job> editJobs;
        for (auto it = m_editDirty.begin(); it != m_editDirty.end();) {
            const std::int64_t k = *it;
            const glm::ivec2   c = coordOf(k);
            const auto ce = m_chunkEdit.find(k);
            const bool loaded  = m_chunks.find(k) != m_chunks.end();
            const bool current = ce != m_chunkEdit.end() && ce->second >= m_editVersion;
            if (!inRange(c, center) || (current && loaded)) {
                it = m_editDirty.erase(it);          // done or gone out of range
                continue;
            }
            if (m_pending.find(k) == m_pending.end()) {
                m_pending.insert(k);
                editJobs.push_back({c, m_settings, m_generation, m_editVersion});
            }
            ++it; // keep until a current-version result replaces it
        }
        if (!editJobs.empty()) {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            for (Job& j : editJobs) m_jobs.push(std::move(j));
            m_jobCv.notify_all();
        }
    }

    if (changed) {
        refreshVisible();
    }
    return uploaded;
}

void TerrainStreamer::rebuild() {
    // Bump generation so in-flight results are discarded; clear loaded + pending.
    ++m_generation;
    m_chunks.clear();
    m_pending.clear();
    m_visible.clear();
    m_editDirty.clear();
    m_chunkEdit.clear();
    m_dirty = true;
}

void TerrainStreamer::editsChanged(const glm::vec2& worldMin,
                                   const glm::vec2& worldMax) {
    ++m_editVersion; // any chunk built before this is now out of date
    const float cs = m_settings.chunkSize;
    const int x0 = static_cast<int>(std::floor(worldMin.x / cs));
    const int x1 = static_cast<int>(std::floor(worldMax.x / cs));
    const int z0 = static_cast<int>(std::floor(worldMin.y / cs));
    const int z1 = static_cast<int>(std::floor(worldMax.y / cs));
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const glm::ivec2 c{x, z};
            const std::int64_t k = key(c);
            // Only chunks we actually hold (or are already building) matter; ones
            // still off-screen will pick up the edits when they first stream in.
            if (m_chunks.find(k) != m_chunks.end() ||
                m_pending.find(k) != m_pending.end())
                m_editDirty.insert(k);
        }
}

void TerrainStreamer::refreshVisible() {
    m_visible.clear();
    m_visible.reserve(m_chunks.size());
    for (const auto& [k, chunk] : m_chunks) {
        m_visible.push_back(&chunk);
    }
}

} // namespace fitzel
