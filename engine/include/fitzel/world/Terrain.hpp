#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "fitzel/graphics/Mesh.hpp"

namespace fitzel {

// Global terrain definition. Heights come from world-space noise (domain-warped
// fBm + ridged multifractal), so the field is continuous everywhere -- adjacent
// chunks tile seamlessly because they sample the same function.
struct TerrainSettings {
    float chunkSize   = 64.0f;  // world units per chunk side
    int   resolution  = 96;     // quads per side (vertices = resolution + 1)
    float heightScale = 14.0f;
    float frequency   = 0.012f;
    int   octaves     = 6;
    float lacunarity  = 2.0f;
    float gain        = 0.5f;
    float seed        = 0.0f;

    float ridgeScale    = 24.0f;
    float warpStrength  = 14.0f;
    float warpFrequency = 0.01f;

    // Large-scale variety: continents (lowland basins vs highlands), regional
    // roughness (plains vs rugged mountains) and plateaus.
    float continentAmp = 1.5f;    // strength of the big elevation swings
    float biomeFreq    = 0.0017f; // size of regions (lower = larger)
    float terrace      = 0.35f;   // 0..1 plateau/terracing amount
};

// World-space terrain height at (x, z). Continuous and cheap enough to query
// (e.g. for placing objects on the ground). Thread-safe (pure function).
float terrainHeight(const TerrainSettings& settings, float worldX, float worldZ);

// A 0..1 "moisture / lushness" field (independent low-frequency noise) for
// biome-dependent placement: lush valleys vs dry highlands. Thread-safe.
float terrainMoisture(const TerrainSettings& settings, float worldX, float worldZ);

// A single square tile of terrain, generated in world space (model = identity).
class TerrainChunk {
public:
    TerrainChunk() = default;

    // Build the CPU mesh data for a chunk. Pure/thread-safe -- no GL calls, so
    // it can run on a worker thread.
    static MeshData buildMeshData(const TerrainSettings& settings, glm::ivec2 coord);

    // Upload prebuilt data to the GPU (must run on the render thread).
    static TerrainChunk fromData(glm::ivec2 coord, const MeshData& data);

    // Convenience: build + upload in one go (render thread).
    static TerrainChunk generate(const TerrainSettings& settings, glm::ivec2 coord);

    const Mesh& mesh()  const { return m_mesh; }
    glm::ivec2  coord() const { return m_coord; }

private:
    Mesh       m_mesh;
    glm::ivec2 m_coord{0};
};

// Streams an NxN grid of TerrainChunks around the viewer. Chunk geometry is
// built on a pool of worker threads and uploaded to the GPU on the render
// thread, so crossing chunk borders doesn't stall the frame.
class TerrainStreamer {
public:
    explicit TerrainStreamer(const TerrainSettings& settings, int radius = 4);
    ~TerrainStreamer();

    TerrainStreamer(const TerrainStreamer&)            = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;

    // Queue generation for the ring around `cameraPos`, drop chunks that fell
    // out of range, and upload up to `maxUploads` finished chunks this frame.
    // Returns the number uploaded.
    int update(const glm::vec3& cameraPos, int maxUploads = 4);

    // Discard everything and regenerate (e.g. after settings changed).
    void rebuild();

    // View distance in chunks. Changing it streams the new ring in/out.
    int  radius() const { return m_radius; }
    void setRadius(int r) {
        r = (r < 1) ? 1 : r;
        if (r != m_radius) { m_radius = r; m_dirty = true; }
    }

    const std::vector<const TerrainChunk*>& visibleChunks() const { return m_visible; }

    const TerrainSettings& settings() const { return m_settings; }
    TerrainSettings&       settings()       { return m_settings; }

    float heightAt(float worldX, float worldZ) const {
        return terrainHeight(m_settings, worldX, worldZ);
    }

    int loadedChunkCount()  const { return static_cast<int>(m_chunks.size()); }
    int pendingChunkCount() const { return static_cast<int>(m_pending.size()); }

private:
    struct Job    { glm::ivec2 coord; TerrainSettings settings; std::uint64_t generation; };
    struct Result { glm::ivec2 coord; std::uint64_t generation; MeshData data; };

    static std::int64_t key(glm::ivec2 c);
    glm::ivec2 chunkCoordOf(const glm::vec3& pos) const;
    bool       inRange(glm::ivec2 c, glm::ivec2 center) const;
    void       refreshVisible();
    void       workerLoop();

    TerrainSettings m_settings;
    int             m_radius;
    glm::ivec2      m_center{INT32_MAX, INT32_MAX};
    bool            m_dirty = true;
    std::uint64_t   m_generation = 0;

    std::unordered_map<std::int64_t, TerrainChunk> m_chunks;
    std::unordered_set<std::int64_t>               m_pending; // queued/in-flight
    std::vector<const TerrainChunk*>               m_visible;

    // Worker pool + job/result queues.
    std::vector<std::thread> m_workers;
    std::queue<Job>          m_jobs;
    std::queue<Result>       m_results;
    std::mutex               m_jobMutex;
    std::mutex               m_resultMutex;
    std::condition_variable  m_jobCv;
    std::atomic<bool>        m_stop{false};
};

} // namespace fitzel
