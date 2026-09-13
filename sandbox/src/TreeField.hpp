#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Terrain.hpp>

#include "Ecology.hpp"

// --- The procedural forest, out to the horizon ----------------------------------
//
// Where every procedurally planted tree stands, streamed in 96 m tiles out to a
// kilometre and a half and more around the eye. The near ones are drawn as meshes,
// the rest as impostors; both come from here, so the switch between the two is
// the same tree changing its clothes, not one forest swapped for another.
//
// A tile is generated on a worker from a copy of everything it depends on and is
// deterministic from its coordinates: 16 x 16 cells of 6 m, each asking the
// ecology (Ecology.hpp) how likely a tree is there and rolling its own hash. A
// tile that leaves the ring and comes back is the same tile. Unlike TiledScatter
// (grass), the result stays on the CPU: the mesh pass has to cull and LOD
// individual trees, which a tile's GPU buffer cannot do.
class TreeField {
public:
    struct Species {
        float density = 1.0f;   // share of the draws among the species
        float size    = 9.0f;   // mean height (m)
        bool  shrub   = false;  // prefers forest edges and open ground
    };
    struct Inputs {
        fitzel::TerrainSettings terrain;
        ecology::Params         eco;
        float                   waterLevel = -1000.0f;
        float                   snowLevel  = 1e9f;
        std::vector<glm::vec2>  road;          // centre lines (kLineBreak between runs)
        float                   roadClear = 0.0f;
        std::vector<glm::vec3>  wet;           // discs nothing grows in
        std::vector<Species>    species;
        bool operator==(const Inputs& o) const;
    };

    // 6 floats a tree: x, y, z, yaw, scale, species.
    static constexpr int kStride = 6;
    static constexpr float kTile = 96.0f;
    static constexpr float kCell = 6.0f;

    TreeField();
    ~TreeField();
    TreeField(const TreeField&) = delete;
    TreeField& operator=(const TreeField&) = delete;

    // New inputs: every tile is regenerated (resident ones keep drawing until
    // their replacements land).
    void configure(const Inputs& in);
    const Inputs& inputs() const { return m_in; }
    // The same inputs, different ground: the terrain was sculpted, which no
    // setting records. Every tile is regenerated.
    void invalidate();

    float radius = 1600.0f;   // metres of forest around the eye

    // Stream around the eye. True when the resident set changed.
    bool update(glm::vec2 eye);

    // Every tree within `r` of `c` (r < 0: all), appended per species.
    void gather(glm::vec2 c, float r, std::vector<std::vector<float>>& perSpecies) const;

    int  treeCount() const { return m_treeCount; }
    int  pendingTiles() const { return static_cast<int>(m_inFlight.size()); }

private:
    using Key = std::int64_t;
    static Key key(int x, int z) {
        return (static_cast<Key>(x) << 32) ^ (static_cast<Key>(z) & 0xffffffffLL);
    }
    struct Job    { Key key; int tx, tz; std::uint32_t gen;
                    std::shared_ptr<const Inputs> in; };
    struct Result { Key key; std::uint32_t gen; std::vector<float> trees; };

    void worker();
    static void generate(const Inputs& in, int tx, int tz, std::vector<float>& out);

    Inputs m_in;
    std::shared_ptr<const Inputs> m_shared;   // what jobs queued now will read
    bool   m_configured = false;
    std::unordered_map<Key, std::vector<float>> m_tiles;
    std::unordered_set<Key> m_inFlight;
    std::unordered_map<Key, std::uint32_t> m_tileGen;   // generation a tile was built at
    int m_treeCount = 0;

    std::vector<std::thread> m_threads;
    std::mutex               m_mtx;
    std::condition_variable  m_cv;
    std::deque<Job>          m_jobs;
    std::vector<Result>      m_results;
    std::uint32_t            m_gen = 1;
    bool                     m_stop = false;
};
