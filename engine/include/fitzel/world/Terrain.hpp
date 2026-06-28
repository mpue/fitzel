#pragma once

#include <vector>

#include "fitzel/graphics/Mesh.hpp"

namespace fitzel {

struct TerrainParams {
    int   resolution  = 256;    // vertices per side (grid is resolution x resolution)
    float worldSize   = 60.0f;  // world units across, centered on the origin
    float heightScale = 8.0f;   // peak amplitude
    float frequency   = 0.04f;  // base noise frequency (per world unit)
    int   octaves     = 6;      // fBm octaves
    float lacunarity  = 2.0f;
    float gain        = 0.5f;
    float seed        = 0.0f;   // offset into the noise field
};

// A procedurally generated heightmap terrain. Heights come from fractional
// Brownian motion (fBm) over Perlin noise; per-vertex normals are derived from
// the heightfield. The result is a ready-to-draw Mesh.
class Terrain {
public:
    static Terrain generate(const TerrainParams& params = {});

    const Mesh&          mesh()   const { return m_mesh; }
    const TerrainParams& params() const { return m_params; }

    // World-space terrain height at (x, z), bilinearly interpolated from the
    // generated grid. Returns 0 outside the terrain bounds.
    float heightAt(float worldX, float worldZ) const;

private:
    TerrainParams      m_params;
    Mesh               m_mesh;
    std::vector<float> m_heights; // resolution*resolution, row-major (z, x)
};

} // namespace fitzel
