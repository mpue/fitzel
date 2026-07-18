#include "TiledScatter.hpp"

#include <algorithm>
#include <cmath>

#include <glad/gl.h>

TiledScatter::~TiledScatter() {
    stopWorkers();
    for (auto& [k, t] : m_tiles) deleteTile(t);
}

void TiledScatter::deleteTile(Tile& t) {
    if (t.vbo) glDeleteBuffers(1, &t.vbo);
    t.vbo   = 0;
    t.count = 0;
}

void TiledScatter::startWorkers() {
    if (!m_workers.empty()) return;
    int n = m_cfg.workerThreads;
    if (n <= 0) {
        // A few threads: enough to keep tiles ahead of the camera without
        // fighting the terrain streamer's pool for cores.
        const unsigned hw = std::thread::hardware_concurrency();
        n = std::clamp(static_cast<int>(hw) / 4, 1, 3);
    }
    m_stop = false;
    for (int i = 0; i < n; ++i)
        m_workers.emplace_back([this] { workerLoop(); });
}

void TiledScatter::stopWorkers() {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop = true;
    }
    m_cv.notify_all();
    for (std::thread& t : m_workers)
        if (t.joinable()) t.join();
    m_workers.clear();
}

void TiledScatter::workerLoop() {
    for (;;) {
        Job job;
        std::shared_ptr<Generator> gen;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait(lk, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop) return;
            job = m_jobs.front();
            m_jobs.pop();
            gen = m_gen;
        }
        std::vector<float> out;
        if (gen && *gen) (*gen)(job.tx, job.tz, job.origin, job.size, out);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_results.push_back({job.key, job.gen, std::move(out)});
        }
    }
}

void TiledScatter::configure(const Config& cfg, Generator gen) {
    // Keep the running pool's size; only the first configure sizes it.
    const int keepThreads = m_cfg.workerThreads;
    m_cfg = cfg;
    if (!m_workers.empty()) m_cfg.workerThreads = keepThreads;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_gen = std::make_shared<Generator>(std::move(gen));
        ++m_generation; // any in-flight job tagged with the old value is dropped
    }
    startWorkers();
}

void TiledScatter::invalidate() {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_generation;
        std::queue<Job>().swap(m_jobs); // drop not-yet-started jobs
        m_results.clear();              // drop stale finished results
    }
    for (auto& [k, t] : m_tiles) deleteTile(t);
    m_tiles.clear();
    m_inFlight.clear();
}

int TiledScatter::instanceCount() const {
    int n = 0;
    for (const auto& [k, t] : m_tiles) n += t.count;
    return n;
}

void TiledScatter::update(glm::vec2 camXZ) {
    if (m_workers.empty()) return; // not configured yet
    const float ts = m_cfg.tileSize;
    const int   R  = m_cfg.radius;
    const std::int32_t cx = static_cast<std::int32_t>(std::floor(camXZ.x / ts));
    const std::int32_t cz = static_cast<std::int32_t>(std::floor(camXZ.y / ts));
    const int r2 = R * R;

    // 1) Free tiles that have left the ring. In-flight generations for them are
    //    left to finish and discarded on arrival (out-of-range check below).
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
        const int dx = keyX(it->first) - cx, dz = keyZ(it->first) - cz;
        if (dx * dx + dz * dz > r2) {
            deleteTile(it->second);
            it = m_tiles.erase(it);
        } else {
            ++it;
        }
    }

    // 2) Queue the nearest missing in-ring tiles for the worker pool.
    {
        struct Cand { int d2; std::int32_t x, z; Key k; };
        std::vector<Cand> want;
        for (int dz = -R; dz <= R; ++dz)
            for (int dx = -R; dx <= R; ++dx) {
                const int d2 = dx * dx + dz * dz;
                if (d2 > r2) continue;
                const std::int32_t tx = cx + dx, tz = cz + dz;
                const Key k = key(tx, tz);
                if (m_tiles.count(k) || m_inFlight.count(k)) continue;
                want.push_back({d2, tx, tz, k});
            }
        if (!want.empty()) {
            std::sort(want.begin(), want.end(),
                      [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
            std::lock_guard<std::mutex> lk(m_mtx);
            for (const Cand& c : want) {
                m_jobs.push({c.k, m_generation, c.x, c.z, glm::vec2{c.x * ts, c.z * ts}, ts});
                m_inFlight.insert(c.k);
            }
        }
        m_cv.notify_all();
    }

    // 3) Drain finished results and upload them, bounded per frame so a burst
    //    (e.g. right after invalidate) doesn't spike the frame time.
    std::vector<Result> ready;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ready.swap(m_results);
    }
    std::vector<Result> deferred;
    int uploads = 0;
    for (Result& res : ready) {
        m_inFlight.erase(res.key);
        if (res.gen != m_generation) continue; // stale: invalidate()/reconfigure
        const int dx = keyX(res.key) - cx, dz = keyZ(res.key) - cz;
        if (dx * dx + dz * dz > r2) continue;  // camera moved on
        if (uploads >= m_cfg.maxUploadsPerFrame) { deferred.push_back(std::move(res)); continue; }
        Tile t;
        t.count = static_cast<int>(res.data.size()) /
                  std::max(1, m_cfg.floatsPerInstance);
        if (t.count > 0) {
            glGenBuffers(1, &t.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, t.vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(res.data.size() * sizeof(float)),
                         res.data.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            m_tiles.emplace(res.key, std::move(t));
        }
        ++uploads;
    }
    if (!deferred.empty()) {
        // Hold the over-budget results (still valid) for next frame; keep them
        // marked in-flight so we don't re-queue their tiles.
        for (Result& res : deferred) m_inFlight.insert(res.key);
        std::lock_guard<std::mutex> lk(m_mtx);
        for (Result& res : deferred) m_results.push_back(std::move(res));
    }
}

void TiledScatter::draw(const DrawTile& cb) const {
    if (!cb) return;
    for (const auto& [k, t] : m_tiles) {
        if (t.count <= 0) continue;
        cb(t.vbo, t.count, glm::vec2{keyX(k) * m_cfg.tileSize, keyZ(k) * m_cfg.tileSize},
           m_cfg.tileSize);
    }
}
