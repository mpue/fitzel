#include "TreeField.hpp"

#include <algorithm>
#include <cmath>

#include "SandboxMath.hpp"

namespace {

// Stable per-cell randomness: a small counter-based generator seeded from the
// cell's lattice coordinates, so a cell always rolls the same tree however and
// whenever its tile is built.
struct CellRng {
    std::uint32_t s;
    CellRng(int gx, int gz) {
        s = static_cast<std::uint32_t>(gx) * 73856093u ^
            static_cast<std::uint32_t>(gz) * 19349663u ^ 0x9e3779b9u;
        next(); next();
    }
    std::uint32_t next() {
        s += 0x6d2b79f5u;
        std::uint32_t t = s;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return t ^ (t >> 14);
    }
    float uni() { return static_cast<float>(next() & 0xffffffu) * (1.0f / 16777216.0f); }
};

float smooth01(float e0, float e1, float x) {
    const float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

bool TreeField::Inputs::operator==(const Inputs& o) const {
    const auto eqEco = [](const ecology::Params& a, const ecology::Params& b) {
        return a.enabled == b.enabled && a.cover == b.cover && a.standSize == b.standSize &&
               a.treeLine == b.treeLine && a.waterLevel == b.waterLevel &&
               a.solitary == b.solitary && a.slopeLove == b.slopeLove;
    };
    if (!(terrain == o.terrain) || !eqEco(eco, o.eco) || waterLevel != o.waterLevel ||
        snowLevel != o.snowLevel || roadClear != o.roadClear ||
        road.size() != o.road.size() || wet.size() != o.wet.size() ||
        species.size() != o.species.size())
        return false;
    for (std::size_t i = 0; i < road.size(); ++i) if (road[i] != o.road[i]) return false;
    for (std::size_t i = 0; i < wet.size(); ++i)  if (wet[i] != o.wet[i]) return false;
    for (std::size_t i = 0; i < species.size(); ++i)
        if (species[i].density != o.species[i].density ||
            species[i].size != o.species[i].size || species[i].shrub != o.species[i].shrub)
            return false;
    return true;
}

TreeField::TreeField() {
    for (int i = 0; i < 2; ++i) m_threads.emplace_back([this] { worker(); });
}

TreeField::~TreeField() {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop = true;
    }
    m_cv.notify_all();
    for (std::thread& t : m_threads) if (t.joinable()) t.join();
}

void TreeField::configure(const Inputs& in) {
    if (m_configured && in == m_in) return;
    m_in = in;
    m_configured = true;
    std::lock_guard<std::mutex> lk(m_mtx);
    m_shared = std::make_shared<const Inputs>(in);
    ++m_gen;
    // Queued for the old inputs: useless now, and no longer in flight -- the
    // next update() queues them again against the new ones.
    for (const Job& j : m_jobs) m_inFlight.erase(j.key);
    m_jobs.clear();
}

void TreeField::invalidate() {
    if (!m_configured) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    ++m_gen;
    for (const Job& j : m_jobs) m_inFlight.erase(j.key);
    m_jobs.clear();
}

void TreeField::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait(lk, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        Result r{job.key, job.gen, {}};
        generate(*job.in, job.tx, job.tz, r.trees);
        std::lock_guard<std::mutex> lk(m_mtx);
        m_results.push_back(std::move(r));
    }
}

void TreeField::generate(const Inputs& in, int tx, int tz, std::vector<float>& out) {
    constexpr int n = static_cast<int>(kTile / kCell);   // 16 cells a side
    const float ox = tx * kTile, oz = tz * kTile;
    // Heights at the cell centres plus a one-cell apron, for the normals.
    float hg[n + 2][n + 2];
    for (int j = 0; j < n + 2; ++j)
        for (int i = 0; i < n + 2; ++i)
            hg[j][i] = fitzel::terrainHeight(in.terrain, ox + (i - 0.5f) * kCell,
                                             oz + (j - 0.5f) * kCell);
    float wsum = 0.0f;
    for (const Species& s : in.species) wsum += std::max(s.density, 0.0f);
    if (wsum <= 0.0f) return;

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int   gx = tx * n + i, gz = tz * n + j;
            const float cx = ox + (i + 0.5f) * kCell, cz = oz + (j + 0.5f) * kCell;
            const float h  = hg[j + 1][i + 1];
            if (h < in.waterLevel + 0.8f || h > in.snowLevel - 2.0f) continue;
            const glm::vec3 nrm = glm::normalize(glm::vec3(
                hg[j + 1][i] - hg[j + 1][i + 2], 2.0f * kCell,
                hg[j][i + 1] - hg[j + 2][i + 1]));
            const ecology::Sample e = ecology::sample(in.eco, cx, cz, h, nrm.y);
            CellRng rng(gx, gz);
            if (rng.uni() >= e.density) continue;
            // Where in the cell -- decided before the exclusions, so a road
            // clears exactly the trees that would stand on it.
            const float px = cx + (rng.uni() - 0.5f) * kCell * 0.9f;
            const float pz = cz + (rng.uni() - 0.5f) * kCell * 0.9f;
            if (!in.road.empty() &&
                roadDistanceSq(in.road, px, pz) < in.roadClear * in.roadClear)
                continue;
            if (inDiscs(in.wet, px, pz)) continue;
            // Which species. Shrubs belong to the edges and the open ground:
            // a wood is fringed with bushes, not full of them.
            const float openness = e.edge + (1.0f - e.forest) * 0.6f;
            float wtot = 0.0f;
            float w[64];
            const int ns = std::min<int>(static_cast<int>(in.species.size()), 64);
            for (int s = 0; s < ns; ++s) {
                const Species& sp = in.species[s];
                float ws = std::max(sp.density, 0.0f);
                if (sp.shrub) ws *= glm::mix(0.25f, 2.4f, glm::clamp(openness, 0.0f, 1.0f));
                w[s] = ws;
                wtot += ws;
            }
            if (wtot <= 0.0f) continue;
            float r = rng.uni() * wtot;
            int pick = ns - 1;
            for (int s = 0; s < ns; ++s) { r -= w[s]; if (r <= 0.0f) { pick = s; break; } }
            const Species& sp = in.species[pick];
            // Size: the wood's interior grows tall, its edge and the tree line
            // keep them short.
            const float tl = in.eco.treeLine;
            float scale = sp.size * glm::mix(0.72f, 1.28f, rng.uni())
                        * glm::mix(0.82f, 1.08f, e.forest)
                        * glm::mix(1.0f, 0.55f, smooth01(tl - 320.0f, tl, h));
            const float yaw = rng.uni() * 6.2831853f;
            const float py  = fitzel::terrainHeight(in.terrain, px, pz) - 0.3f;
            out.insert(out.end(), {px, py, pz, yaw, scale, static_cast<float>(pick)});
        }
    }
}

bool TreeField::update(glm::vec2 eye) {
    if (!m_configured) return false;
    bool changed = false;

    // 1) Finished tiles.
    std::vector<Result> done;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        done.swap(m_results);
    }
    const float keepR = radius + kTile * 1.5f;
    for (Result& r : done) {
        m_inFlight.erase(r.key);
        if (r.gen != m_gen) continue;            // built for inputs that changed since
        const int tx = static_cast<int>(r.key >> 32);
        const int tz = static_cast<int>(static_cast<std::int32_t>(r.key & 0xffffffffLL));
        const glm::vec2 c((tx + 0.5f) * kTile, (tz + 0.5f) * kTile);
        if (glm::length(c - eye) > keepR) continue;
        auto it = m_tiles.find(r.key);
        if (it != m_tiles.end()) m_treeCount -= static_cast<int>(it->second.size() / kStride);
        m_treeCount += static_cast<int>(r.trees.size() / kStride);
        m_tiles[r.key]   = std::move(r.trees);
        m_tileGen[r.key] = r.gen;
        changed = true;
    }

    // 2) Tiles that fell out of reach.
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
        const int tx = static_cast<int>(it->first >> 32);
        const int tz = static_cast<int>(static_cast<std::int32_t>(it->first & 0xffffffffLL));
        const glm::vec2 c((tx + 0.5f) * kTile, (tz + 0.5f) * kTile);
        if (glm::length(c - eye) > keepR) {
            m_treeCount -= static_cast<int>(it->second.size() / kStride);
            m_tileGen.erase(it->first);
            it = m_tiles.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    // 3) Tiles wanted and missing (or built for old inputs), nearest first.
    const int R  = static_cast<int>(std::ceil(radius / kTile)) + 1;
    const int ex = static_cast<int>(std::floor(eye.x / kTile));
    const int ez = static_cast<int>(std::floor(eye.y / kTile));
    std::vector<std::pair<float, Job>> want;
    for (int dz = -R; dz <= R; ++dz)
        for (int dx = -R; dx <= R; ++dx) {
            const int tx = ex + dx, tz = ez + dz;
            const glm::vec2 c((tx + 0.5f) * kTile, (tz + 0.5f) * kTile);
            const float d = glm::length(c - eye);
            if (d > radius) continue;
            const Key k = key(tx, tz);
            if (m_inFlight.count(k)) continue;
            const auto g = m_tileGen.find(k);
            if (g != m_tileGen.end() && g->second == m_gen) continue;
            want.push_back({d, Job{k, tx, tz, m_gen, nullptr}});
        }
    if (!want.empty()) {
        std::sort(want.begin(), want.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto& w : want) {
            w.second.in = m_shared;
            m_inFlight.insert(w.second.key);
            m_jobs.push_back(std::move(w.second));
        }
        m_cv.notify_all();
    }
    return changed;
}

void TreeField::gather(glm::vec2 c, float r, std::vector<std::vector<float>>& per) const {
    const float r2 = r * r;
    for (const auto& [k, trees] : m_tiles) {
        if (r >= 0.0f) {
            const int tx = static_cast<int>(k >> 32);
            const int tz = static_cast<int>(static_cast<std::int32_t>(k & 0xffffffffLL));
            // Tile rectangle vs the circle: skip tiles entirely outside.
            const float qx = glm::clamp(c.x, tx * kTile, (tx + 1) * kTile);
            const float qz = glm::clamp(c.y, tz * kTile, (tz + 1) * kTile);
            if ((qx - c.x) * (qx - c.x) + (qz - c.y) * (qz - c.y) > r2) continue;
        }
        for (std::size_t i = 0; i + kStride <= trees.size(); i += kStride) {
            if (r >= 0.0f) {
                const float dx = trees[i] - c.x, dz = trees[i + 2] - c.y;
                if (dx * dx + dz * dz > r2) continue;
            }
            const int s = static_cast<int>(trees[i + 5]);
            if (s < 0 || s >= static_cast<int>(per.size())) continue;
            per[s].insert(per[s].end(), trees.begin() + i, trees.begin() + i + 5);
        }
    }
}
