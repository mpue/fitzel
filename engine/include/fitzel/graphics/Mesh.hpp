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

    // Built-in primitive: a unit cube centered at the origin, with normals
    // and UVs. Handy for testing lighting and texturing.
    static Mesh cube();

    // Issue the draw call. Assumes a shader is already bound.
    void draw() const;

private:
    std::uint32_t m_vao         = 0;
    std::uint32_t m_vbo         = 0;
    std::uint32_t m_ebo         = 0;
    std::uint32_t m_vertexCount = 0;
    std::uint32_t m_indexCount  = 0;
};

} // namespace fitzel
