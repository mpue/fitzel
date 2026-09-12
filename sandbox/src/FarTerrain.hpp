#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Shader.hpp>
#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Terrain.hpp>

#include "Ecology.hpp"
#include "FrameRender.hpp"

// The far terrain's projection: the camera's lens, a depth range of its own
// (50 m .. 60 km -- see FarTerrain) and the same sub-pixel jitter as the
// frame, so TAA resolves the horizon instead of smearing it.
inline glm::mat4 farProjection(const fitzel::Camera& cam, float aspect,
                               const glm::vec2& jitter) {
    glm::mat4 p = glm::perspective(glm::radians(cam.fov()), aspect, 50.0f, 60000.0f);
    p[2][0] -= jitter.x;
    p[2][1] -= jitter.y;
    return p;
}

// --- The ground beyond the streamed ring ----------------------------------------
//
// The terrain streamer holds a square of full-detail chunks around the eye --
// half a kilometre each way at the widest setting -- and past its edge the world
// simply stopped: fog, then sky. Nothing on the horizon, so nothing to measure
// the valley against, which is most of why a landscape can look small however
// good it is up close.
//
// This draws the same height function (terrainHeight, edits and all) out to the
// horizon as five nested heightfield rings, each twice as coarse and twice as
// large as the one inside it: 2 km at 8 m out to 32 km at 128 m. Each ring is
// one shared 257x257 grid displaced in the vertex shader from a float texture
// two worker threads fill, re-centred when the eye has moved an eighth of its
// size. The part of a ring that a finer one covers is sunk out
// of sight, so there is never a double surface to fight over.
//
// It is drawn BEFORE the scene, right after the sky, with its own depth range
// (50 m .. 60 km), and the depth buffer is cleared behind it. The near scene can
// therefore never be occluded by it -- which is correct, because by construction
// everything the near scene holds is nearer -- and 24 bits of depth never have
// to span a millimetre and sixty kilometres at once.
//
// Shading is its own, not lit.frag's: at a kilometre a terrain layer texture is a
// single averaged colour, and what reads is where the forest stops, where rock
// breaks through and where the snow lies. Those are decided here from height,
// slope and the moisture field, lit by the sun with the ranges shadowing each
// other, and fogged with the same height fog as everything else plus the blue of
// the air itself, so a range twenty kilometres off is the colour of distance.
class FarTerrain {
public:
    FarTerrain();
    ~FarTerrain();
    FarTerrain(const FarTerrain&) = delete;
    FarTerrain& operator=(const FarTerrain&) = delete;

    bool enabled = true;

    // The look. Heights are world metres.
    float snowLevel  = 1100.0f;  // snow lies above this (on slopes it can hold)
    float treeLine   = 750.0f;   // forest gives way to alpine meadow here
    float waterLevel = -1000.0f; // lakes and sea below this are water
    glm::vec3 grassTint{1.0f};   // the near grass's colour multiplier
    ecology::Params eco;         // where the forests are (the trees' own rule)
    glm::vec3 canopy{0.02f, 0.035f, 0.012f};  // their foliage colour, linear

    // Compile the shaders and build the grid. False if a shader failed.
    bool init();

    // Re-centre the rings on `eye` and pick up heightfields the worker finished.
    // `nearMin/nearMax` is the square the streamed chunks cover (the finest ring
    // sinks inside it). Settings that differ from the last call recompute
    // everything.
    void update(const glm::vec3& eye, const fitzel::TerrainSettings& settings,
                bool terrainPresent, const glm::vec2& nearMin,
                const glm::vec2& nearMax);

    // Throw the heightfields away (edits changed under them).
    void invalidate() { m_settingsValid = false; }

    // Draw into the bound target with `view` and the far projection `proj`
    // (whose near plane lies inside the streamed ring). Leaves depth test on and
    // culling as it found it; the caller clears depth afterwards.
    //
    // `mirror`: the water's reflection pass. There the sunk part of a ring is
    // cut out instead -- sunk ground flipped over the water plane comes up
    // ABOVE the mirrored near terrain, into the reflection of the sky.
    void draw(const FrameContext& ctx, const glm::mat4& view, const glm::mat4& proj,
              bool mirror = false);

    // Anything to draw yet?
    bool ready() const;

    // The streamed square this frame (xz min, xz max): where the near scene's
    // own surfaces -- the lake quad -- end and this one's begin.
    glm::vec4 nearRect() const { return glm::vec4(m_nearMin, m_nearMax); }

    // The finest ring's field (r = height, g = moisture) and where it lies:
    // (origin x, origin z, cell, samples per side). The near terrain reads the
    // moisture from it to colour its meadows the way the grass grows them.
    // 0 until the ring has been filled.
    unsigned  fineTexture() const { return m_levels[0].valid ? m_levels[0].tex : 0u; }
    glm::vec4 fineRect() const {
        return glm::vec4(m_levels[0].origin, m_levels[0].cell, static_cast<float>(kGrid));
    }

private:
    // Five rings, each twice the cell and twice the size of the one inside it:
    // 8 m over 2 km out to 128 m over 32 km. Doubling (rather than quadrupling)
    // keeps a cell at about the same size ON SCREEN wherever it is -- a few
    // pixels -- which is what a ridgeline silhouette needs to stay a ridge and
    // not a row of facets.
    static constexpr int kLevels = 5;
    static constexpr int kGrid   = 257;          // vertices per side

    struct Level {
        float      cell = 16.0f;                 // metres between samples
        glm::vec2  origin{0.0f};                 // world XZ of sample (0,0)
        bool       valid = false;                // texture holds a field
        unsigned   tex = 0;                      // RG32F: height, moisture
        glm::vec2  wanted{0.0f};                 // origin a job is running for
        bool       busy = false;
        std::uint64_t stamp = 0;                 // settings generation of the job
        float size() const { return cell * (kGrid - 1); }
    };

    struct Job {
        int level;
        glm::vec2 origin;
        float cell;
        fitzel::TerrainSettings settings;
        std::uint64_t stamp;
    };
    struct Result {
        int level;
        glm::vec2 origin;
        std::uint64_t stamp;
        std::vector<float> data;                 // kGrid*kGrid*2
    };

    void worker();
    void queue(int level, const glm::vec2& origin);

    std::array<Level, kLevels> m_levels;
    fitzel::Shader m_shader;
    unsigned m_vao = 0, m_vbo = 0, m_ibo = 0;
    int      m_indexCount = 0;
    bool     m_ready = false;

    fitzel::TerrainSettings m_settings;
    bool          m_settingsValid = false;
    bool          m_present = true;
    std::uint64_t m_stamp = 1;
    glm::vec2     m_nearMin{0.0f}, m_nearMax{0.0f};

    std::thread               m_thread, m_thread2;
    std::mutex                m_mutex;
    std::condition_variable   m_cv;
    std::deque<Job>           m_jobs;
    std::vector<Result>       m_results;
    std::atomic<bool>         m_stop{false};
};
