#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace fitzel {

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

// CPU-side geometry, separated from the GPU upload so it can be built off the
// render thread (e.g. async terrain generation) and uploaded later.
struct MeshData {
    std::vector<Vertex>        vertices;
    std::vector<std::uint32_t> indices;
};

// A renderable mesh: owns a VAO/VBO (and optional EBO) on the GPU. Move-only.
class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&)            = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    // Upload vertex data (and optionally indices) to the GPU.
    static Mesh create(const std::vector<Vertex>& vertices,
                       const std::vector<std::uint32_t>& indices = {});
    static Mesh create(const MeshData& data);

    // Built-in primitive: a unit cube centered at the origin, with normals
    // and UVs. Handy for testing lighting and texturing.
    static Mesh cube();

    // Issue the draw call. Assumes a shader is already bound.
    void draw() const;

    // Local-space axis-aligned bounding box (for frustum culling).
    const glm::vec3& boundsMin() const { return m_boundsMin; }
    const glm::vec3& boundsMax() const { return m_boundsMax; }

private:
    std::uint32_t m_vao         = 0;
    std::uint32_t m_vbo         = 0;
    std::uint32_t m_ebo         = 0;
    std::uint32_t m_vertexCount = 0;
    std::uint32_t m_indexCount  = 0;
    glm::vec3     m_boundsMin{0.0f};
    glm::vec3     m_boundsMax{0.0f};
};

} // namespace fitzel
