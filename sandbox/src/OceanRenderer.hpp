#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>

#include "Breakers.hpp"

namespace ocean {

class Ocean;
class WaveSim;
class ShoreMap;

// The open water's GPU half: the ocean's cascades and the wave patch as
// textures, and the mesh they displace.
//
// THE MESH IS ROUND. Rings around the camera, each a few percent wider than the
// last, out to the horizon -- so a vertex is about as far from its neighbours
// as a pixel is wide at that distance, near and far. A square grid dragged
// along with the camera had the same spacing at the foot of the jetty as two
// kilometres out: too coarse for the ripples at your feet, wasted on the
// horizon, and its vertices slid through the waves, which is what made the
// whole surface swim. Rings have no seams to stitch either, so no cracks.
//
// The vertex keeps its spacing (in aPos.y) so the shader can read each cascade
// at a mip as coarse as the mesh there: waves a vertex cannot carry are
// filtered out rather than aliased, and the pixel normal carries them instead.
class OceanRenderer {
public:
    OceanRenderer();
    ~OceanRenderer();
    OceanRenderer(const OceanRenderer&) = delete;
    OceanRenderer& operator=(const OceanRenderer&) = delete;

    // Send whatever changed since the last call.
    void upload(const Ocean& o, const WaveSim& w, const ShoreMap* shore = nullptr);
    // Bind the textures from `firstUnit` on and set the uniforms the water
    // shaders read (see water.vert): grid centre at the camera, on the still
    // water level.
    // `program` is the GL program (Shader::id()), so a harness that compiles
    // the shaders itself can bind them too.
    void bind(unsigned program, int firstUnit, glm::vec3 camera, float waterLevel,
              const Ocean& o, const WaveSim& w, const ShoreMap* shore = nullptr,
              const Breakers& br = {}) const;
    const fitzel::Mesh& mesh() const { return m_mesh; }

private:
    unsigned m_disp[3]  = {0, 0, 0};
    unsigned m_slope[3] = {0, 0, 0};
    unsigned m_wave     = 0;
    unsigned m_shore    = 0;
    int      m_n = 0, m_waveN = 0, m_shoreN = 0;
    std::uint64_t m_oceanGen = ~0ull, m_waveGen = ~0ull, m_shoreGen = ~0ull;
    std::vector<glm::vec4> m_scratch;
    fitzel::Mesh m_mesh;
};

} // namespace ocean
