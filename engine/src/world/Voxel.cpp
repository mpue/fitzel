#include "fitzel/world/Voxel.hpp"

namespace fitzel {

glm::vec3 voxelColor(VoxelType t, bool faceUp) {
    switch (t) {
        case VoxelType::Grass:  return faceUp ? glm::vec3(0.36f, 0.62f, 0.24f)
                                              : glm::vec3(0.48f, 0.34f, 0.20f);
        case VoxelType::Dirt:   return glm::vec3(0.48f, 0.34f, 0.20f);
        case VoxelType::Stone:  return glm::vec3(0.50f, 0.50f, 0.52f);
        case VoxelType::Sand:   return glm::vec3(0.82f, 0.74f, 0.50f);
        case VoxelType::Snow:   return glm::vec3(0.92f, 0.94f, 0.98f);
        case VoxelType::Wood:   return glm::vec3(0.42f, 0.30f, 0.16f);
        case VoxelType::Leaves: return glm::vec3(0.20f, 0.45f, 0.16f);
        case VoxelType::Water:  return glm::vec3(0.16f, 0.34f, 0.52f);
        case VoxelType::Air:    break;
    }
    return glm::vec3(1.0f, 0.0f, 1.0f); // magenta = "shouldn't happen"
}

namespace {

// The six cube faces, in min-corner voxel coordinates. Each face is its outward
// normal (also the neighbour direction to test for air) plus four CCW corner
// offsets in {0,1}^3 -- lifted straight from Mesh::cube's winding so front-face
// culling matches the rest of the engine. UVs run 0..1 across each quad.
struct Face {
    glm::ivec3 normal;
    glm::ivec3 corner[4];
};

constexpr Face kFaces[6] = {
    // +X
    {{ 1, 0, 0}, {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}},
    // -X
    {{-1, 0, 0}, {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}},
    // +Y (top)
    {{ 0, 1, 0}, {{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}}},
    // -Y (bottom)
    {{ 0,-1, 0}, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}},
    // +Z
    {{ 0, 0, 1}, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}},
    // -Z
    {{ 0, 0,-1}, {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}},
};

constexpr glm::vec2 kFaceUV[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

} // namespace

MeshData VoxelChunk::buildMeshData() const {
    MeshData data;
    for (int z = 0; z < kSize; ++z) {
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const VoxelType t = m_cells[index(x, y, z)];
                if (!isSolid(t)) continue;

                for (const Face& f : kFaces) {
                    // Emit the face only where it borders air (or the chunk edge).
                    if (isSolid(at(x + f.normal.x, y + f.normal.y, z + f.normal.z)))
                        continue;

                    const glm::vec3 n(f.normal);
                    const glm::vec4 col(voxelColor(t, f.normal.y > 0), 0.0f);
                    const auto base = static_cast<std::uint32_t>(data.vertices.size());

                    for (int i = 0; i < 4; ++i) {
                        Vertex v;
                        v.position = glm::vec3(x + f.corner[i].x,
                                               y + f.corner[i].y,
                                               z + f.corner[i].z);
                        v.normal = n;
                        v.uv     = kFaceUV[i];
                        v.paint  = col; // voxel colour rides in rgb (see lit shader)
                        data.vertices.push_back(v);
                    }
                    data.indices.insert(data.indices.end(),
                        {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});
                }
            }
        }
    }
    return data;
}

} // namespace fitzel
