#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

// Streams GPU instance buffers over a moving grid of world-space tiles. Owns the
// per-tile instance VBOs (raw GL float buffers), a small persistent worker pool
// that generates tiles off the render thread, and the ring bookkeeping; the
// owner supplies a deterministic per-tile generator and draws each resident tile
// through the draw() callback (binding its own base VAO + instance attributes).
//
// This replaces the "regenerate the whole disc when the camera drifts" model:
// moving the camera only builds the handful of tiles entering the ring and frees
// the ones leaving it, so placement never reshuffles and there is no field-wide
// hitch. A fixed pool (not one std::async per tile) does the generation, so
// flying doesn't churn threads or oversubscribe the cores the terrain pool uses.
class TiledScatter {
public:
    // Fill one tile's instance floats. Called on a worker thread, so it must
    // only read thread-safe / copied inputs (e.g. a TerrainSettings copy). The
    // placement MUST be deterministic from (tx,tz) -- streaming re-enters the
    // same tile after the camera leaves and returns, and it must look identical.
    // `origin` is the tile's min corner in world XZ; `size` is the tile edge.
    using Generator = std::function<void(std::int32_t tx, std::int32_t tz,
                                         glm::vec2 origin, float size,
                                         std::vector<float>& out)>;

    // Visit one resident tile for drawing: (instance VBO, instance count, tile
    // origin (min corner XZ), tile size). The owner binds its base VAO once
    // before draw() and points the instance attributes at `vbo` per tile.
    using DrawTile = std::function<void(std::uint32_t vbo, int count,
                                        glm::vec2 origin, float size)>;

    struct Config {
        float tileSize           = 16.0f; // world units per tile edge
        int   radius             = 6;     // tiles each way from the camera tile
        int   floatsPerInstance  = 7;     // stride of the generator's output
        int   maxUploadsPerFrame = 2;     // GL uploads per update() (avoid stalls)
        int   workerThreads      = 0;     // pool size; <=0 = auto (a few threads)
    };

    ~TiledScatter();

    // (Re)set the config + generator. The worker pool starts on the first call
    // and keeps its size; later calls only swap the generator + non-thread
    // config. Any in-flight generation is tagged stale so its result is dropped.
    // Call invalidate() as well to rebuild the resident tiles with the new look.
    void configure(const Config& cfg, Generator gen);
    void setRadius(int r) { m_cfg.radius = r; }
    int  radius() const { return m_cfg.radius; }
    float tileSize() const { return m_cfg.tileSize; }

    // Stream tiles around camXZ: free out-of-range tiles, queue generation for
    // freshly-entering ones (nearest first), and upload finished results
    // (bounded). Call once per frame on the render thread (issues GL calls).
    void update(glm::vec2 camXZ);

    // Issue the owner's draw for every ready tile. Bind shader/VAO first.
    void draw(const DrawTile& cb) const;

    // Drop every tile and force a rebuild (call when the generator's inputs
    // change). In-flight generations are tagged stale and discarded on arrival,
    // so this never blocks on a worker.
    void invalidate();

    int residentTiles() const { return static_cast<int>(m_tiles.size()); }
    int instanceCount() const;

private:
    using Key = std::int64_t;
    static Key key(std::int32_t x, std::int32_t z) {
        return (static_cast<Key>(x) << 32) ^ (static_cast<Key>(z) & 0xffffffffLL);
    }
    static std::int32_t keyX(Key k) { return static_cast<std::int32_t>(k >> 32); }
    static std::int32_t keyZ(Key k) { return static_cast<std::int32_t>(k & 0xffffffffLL); }

    struct Tile   { std::uint32_t vbo = 0; int count = 0; };
    struct Job    { Key key; std::uint32_t gen; std::int32_t tx, tz;
                    glm::vec2 origin; float size; };
    struct Result { Key key; std::uint32_t gen; std::vector<float> data; };

    void startWorkers();
    void stopWorkers();
    void workerLoop();
    void deleteTile(Tile& t);

    Config m_cfg{};

    // Render-thread state.
    std::unordered_map<Key, Tile>  m_tiles;
    std::unordered_set<Key>        m_inFlight; // queued or running (not yet drained)

    // Shared with workers (guarded by m_mtx).
    std::shared_ptr<Generator> m_gen;
    std::uint32_t              m_generation = 0; // bumped on configure/invalidate
    std::vector<std::thread>   m_workers;
    std::mutex                 m_mtx;
    std::condition_variable    m_cv;
    std::queue<Job>            m_jobs;
    std::vector<Result>        m_results;
    bool                       m_stop = false;
};
