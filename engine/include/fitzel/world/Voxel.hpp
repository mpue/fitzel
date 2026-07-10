#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "fitzel/graphics/Mesh.hpp"

namespace fitzel {

// The kinds of block a voxel cell can hold. Air is empty (never meshed); every
// other value is solid and gets a colour from voxelColor(). Extend freely -- the
// mesher only cares about "is this cell air or not".
enum class VoxelType : std::uint8_t {
    Air = 0,
    Grass,
    Dirt,
    Stone,
    Sand,
    Snow,
    Wood,
    Leaves,
    Water,
};

inline bool isSolid(VoxelType t) { return t != VoxelType::Air; }

// Base sRGB albedo for a block type. `faceUp` gives grass a green top over its
// dirt sides; every other type is a single flat colour. Colours are sRGB (the
// lit shader linearises them), baked per face into the vertex "paint" attribute
// and read back in the shader's voxel colour mode.
glm::vec3 voxelColor(VoxelType t, bool faceUp);

// A fixed-size cubic grid of voxels. Pure CPU data + a mesher; no GL calls, so
// buildMeshData() can run off the render thread just like TerrainChunk. Geometry
// is emitted in local space (0..kSize per axis) -- place a chunk in the world
// with a translation model matrix at chunkSize * coord.
class VoxelChunk {
public:
    static constexpr int kSize = 32; // cells per axis

    VoxelChunk() : m_cells(kSize * kSize * kSize, VoxelType::Air) {}

    static bool inBounds(int x, int y, int z) {
        return x >= 0 && y >= 0 && z >= 0 && x < kSize && y < kSize && z < kSize;
    }

    // Read/write a cell. Out-of-range reads return Air (so boundary faces mesh);
    // out-of-range writes are ignored.
    VoxelType at(int x, int y, int z) const {
        return inBounds(x, y, z) ? m_cells[index(x, y, z)] : VoxelType::Air;
    }
    void set(int x, int y, int z, VoxelType t) {
        if (inBounds(x, y, z)) m_cells[index(x, y, z)] = t;
    }

    // Build the renderable geometry: one quad per solid-cell face that borders
    // air. Hidden interior faces are culled, so a filled chunk costs only its
    // surface. Per-voxel colour is baked into Vertex::paint.rgb.
    // (A greedy pass merging coplanar same-colour faces is the natural next step;
    // this culled mesher already turns a full 32^3 chunk into just its shell.)
    MeshData buildMeshData() const;

private:
    static int index(int x, int y, int z) { return x + kSize * (y + kSize * z); }

    std::vector<VoxelType> m_cells;
};

} // namespace fitzel
