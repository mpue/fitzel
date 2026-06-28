#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "fitzel/graphics/Mesh.hpp"

namespace fitzel {

// Global terrain definition. Heights come from world-space fBm Perlin noise,
// so the field is continuous everywhere -- adjacent chunks tile seamlessly
// because they sample the same function (including across shared edges).
struct TerrainSettings {
    float chunkSize   = 64.0f;  // world units per chunk side
    int   resolution  = 96;     // quads per side (vertices = resolution + 1)
    float heightScale = 14.0f;  // amplitude of the rolling base
    float frequency   = 0.012f;
    int   octaves     = 6;
    float lacunarity  = 2.0f;
    float gain        = 0.5f;
    float seed        = 0.0f;

    // Mountain ridges layered on top of the base (ridged multifractal).
    float ridgeScale     = 24.0f;
    // Domain warping bends the noise field for organic, non-grid-aligned shapes.
    float warpStrength   = 14.0f; // world units of displacement
    float warpFrequency  = 0.01f;
};

// World-space terrain height at (x, z) for the given settings. Continuous and
// cheap enough to call for queries (e.g. placing objects on the ground).
float terrainHeight(const TerrainSettings& settings, float worldX, float worldZ);

// A single square tile of terrain, generated in world space (model = identity).
class TerrainChunk {
public:
    TerrainChunk() = default;
    static TerrainChunk generate(const TerrainSettings& settings, glm::ivec2 coord);

    const Mesh& mesh()  const { return m_mesh; }
    glm::ivec2  coord() const { return m_coord; }

private:
    Mesh       m_mesh;
    glm::ivec2 m_coord{0};
};

// Streams an NxN grid of TerrainChunks centered on the viewer, regenerating the
// ring as the camera crosses chunk boundaries. Chunks outside the radius are
// dropped. Generation is seamless thanks to world-space noise.
class TerrainStreamer {
public:
    explicit TerrainStreamer(const TerrainSettings& settings, int radius = 4);

    // Ensure the chunks around `cameraPos` exist; drop the rest. Returns the
    // number of chunks (re)generated this call (0 when nothing changed).
    int update(const glm::vec3& cameraPos);

    // Force-regenerate everything (e.g. after settings changed).
    void rebuild();

    const std::vector<const TerrainChunk*>& visibleChunks() const { return m_visible; }

    const TerrainSettings& settings() const { return m_settings; }
    TerrainSettings&       settings()       { return m_settings; }

    float heightAt(float worldX, float worldZ) const {
        return terrainHeight(m_settings, worldX, worldZ);
    }

    int loadedChunkCount() const { return static_cast<int>(m_chunks.size()); }

private:
    static std::int64_t key(glm::ivec2 c);
    glm::ivec2 chunkCoordOf(const glm::vec3& pos) const;
    void refreshVisible();

    TerrainSettings m_settings;
    int             m_radius;
    glm::ivec2      m_center{INT32_MAX, INT32_MAX}; // forces first update
    bool            m_dirty = true;

    std::unordered_map<std::int64_t, TerrainChunk> m_chunks;
    std::vector<const TerrainChunk*>               m_visible;
};

} // namespace fitzel
