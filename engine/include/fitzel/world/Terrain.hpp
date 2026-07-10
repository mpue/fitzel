#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
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

    // Epic-scale shaping. All default to a no-op, so scenes saved before these
    // existed (and the plain generator) look exactly as they did.
    float valleyDepth   = 0.0f;   // carve meandering valleys/canyons (world units)
    float peakSharpness = 1.0f;   // >1 pinches sharp alpine crests, <1 rounds peaks
    float reliefGain    = 1.0f;   // master vertical exaggeration of the whole relief
};

// --- Editable deformation layer -------------------------------------------
// A sparse grid of manual height offsets, sampled bilinearly and layered on top
// of the procedural terrain by terrainHeight(). A sculpt brush edits it. Cells
// are keyed by integer grid coordinate at `cell` spacing (world units), so only
// touched ground costs memory -- the terrain stays effectively infinite.
struct TerrainEditField {
    float cell = 1.0f;                              // grid spacing (world units)
    std::unordered_map<std::int64_t, float> deltas; // cell -> height offset

    bool  empty() const { return deltas.empty(); }
    float sample(float worldX, float worldZ) const; // bilinear; 0 where unedited

    static std::int64_t cellKey(int ix, int iz);

    // Sculpt operations over a circular world-space brush. `amount` is the step
    // applied this call; raise() uses it as a height delta (negative = lower),
    // flatten()/smooth() use it as a 0..1 blend rate toward the target/average.
    void raise  (glm::vec2 center, float radius, float amount);
    void flatten(const TerrainSettings& s, glm::vec2 center, float radius,
                 float amount, float target);
    void smooth (const TerrainSettings& s, glm::vec2 center, float radius,
                 float amount);

    // Carve a valley (or, with negative depth, raise a ridge) by pulling the
    // surface toward `depth` below the brush-centre ground with a smooth
    // cross-section. Digging never fills and raising never digs, so holding and
    // dragging cuts a continuous channel (or piles a continuous crest) that follows
    // the land. `rate` (0..1) is the blend applied this call.
    void carve  (const TerrainSettings& s, glm::vec2 center, float radius,
                 float rate, float depth);

    // Thermal erosion: over the brush disc, material on slopes steeper than the
    // talus angle slides to lower neighbours, weathering ridges into scree and
    // filling hollows. `rate` (0..1) scales how much moves per call; `iterations`
    // is the relaxation sweeps per call (holding the brush accumulates more).
    void erode  (const TerrainSettings& s, glm::vec2 center, float radius,
                 float rate, int iterations = 8);

    // Stamp a procedural landform additively under the brush. `height` is the
    // peak offset in world units (negative digs the shape in instead); `shape`
    // selects the profile: 0 dome, 1 cone, 2 plateau/mesa, 3 crater, 4 ridge,
    // 5 mountain range (a rugged multi-peak crest). `rotation` (radians) orients
    // the ridge/range. Apply once per click, not held.
    void stamp  (glm::vec2 center, float radius, float height, int shape,
                 float rotation = 0.0f);

    // Roughen: add signed fBm noise under the brush to break up smooth ground.
    // `amount` scales the bump height this call; `frequency` sets the feature
    // size; `seed` decorrelates successive dabs so holding layers detail rather
    // than amplifying one fixed pattern.
    void roughen(glm::vec2 center, float radius, float amount, float frequency,
                 float seed);
};

// --- Editable texture-paint layer -----------------------------------------
// A sparse world grid of per-layer paint weights for the first four terrain
// texture layers, sampled bilinearly and baked into terrain vertices by
// buildMeshData. Zero everywhere unpainted, so the shader falls back to the
// automatic height/slope blend -- painting is a purely additive override.
struct TerrainPaintField {
    float cell = 1.0f;                                   // grid spacing (world units)
    std::unordered_map<std::int64_t, glm::vec4> weights; // cell -> paint (layers 0..3)

    bool      empty() const { return weights.empty(); }
    glm::vec4 sample(float worldX, float worldZ) const;  // bilinear; 0 where unpainted

    // Paint `layer` (0..3) up over a circular brush, pushing the other three down so
    // the painted layer comes to dominate. `amount` (0..1) is the step this call.
    void paint(glm::vec2 center, float radius, int layer, float amount);
    // Erase: fade all weights toward 0 over the brush (revert to the auto blend).
    void erase(glm::vec2 center, float radius, float amount);
};

// The global texture-paint snapshot buildMeshData bakes into terrain vertices.
// Same publish/read discipline as the edit snapshot. Treat null as "unpainted".
std::shared_ptr<const TerrainPaintField> terrainPaintSnapshot();
void setTerrainPaintSnapshot(std::shared_ptr<const TerrainPaintField> field);

// Pure procedural height, with no manual edits applied. Thread-safe.
float terrainBaseHeight(const TerrainSettings& settings, float worldX, float worldZ);

// The global deformation snapshot terrainHeight() layers on top. Mutex-guarded
// shared_ptr swap: the render thread publishes a new immutable field, worker
// threads (and everyone querying heights) read the current one. Never null after
// the first publish; treat null as "no edits".
std::shared_ptr<const TerrainEditField> terrainEditSnapshot();
void setTerrainEditSnapshot(std::shared_ptr<const TerrainEditField> field);

// World-space terrain height at (x, z) = procedural base + manual edits.
// Continuous and cheap enough to query (e.g. for placing objects on the ground,
// draping roads, scattering grass). Thread-safe.
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

    // Terrain edits changed: rebuild the already-loaded chunks overlapping the
    // world-space rectangle [worldMin, worldMax] against the latest edit
    // snapshot. The old mesh keeps rendering until its replacement is ready, so
    // sculpting never punches a hole in the ground. Call after publishing the
    // new snapshot with setTerrainEditSnapshot().
    void editsChanged(const glm::vec2& worldMin, const glm::vec2& worldMax);

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
    struct Job    { glm::ivec2 coord; TerrainSettings settings; std::uint64_t generation; std::uint64_t editVersion; };
    struct Result { glm::ivec2 coord; std::uint64_t generation; std::uint64_t editVersion; MeshData data; };

    static std::int64_t key(glm::ivec2 c);
    static glm::ivec2   coordOf(std::int64_t k);
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

    // Sculpt bookkeeping: which edit-field version a loaded chunk was built at,
    // and which loaded chunks still need a rebuild to catch up to it.
    std::uint64_t                                  m_editVersion = 0;
    std::unordered_set<std::int64_t>               m_editDirty;
    std::unordered_map<std::int64_t, std::uint64_t> m_chunkEdit;

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
