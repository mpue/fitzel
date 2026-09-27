#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace fitzel {

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    // Terrain texture-paint weights for the first four layers (see TerrainPaintField).
    // Zero on every non-terrain mesh -> the shader ignores it. Baked per terrain
    // vertex so it interpolates across the surface like any other attribute.
    glm::vec4 paint{0.0f};
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

    // Another index list over `base`'s vertices: its own VAO and element
    // buffer, the vertex buffer borrowed (never freed by the view, so `base`
    // must outlive it). For levels of detail that pick fewer of the same
    // vertices -- a terrain chunk's coarser triangulations cost no second copy
    // of its grid.
    static Mesh createView(const Mesh& base, const std::vector<std::uint32_t>& indices);

    // Built-in primitive: a unit cube centered at the origin, with normals
    // and UVs. Handy for testing lighting and texturing.
    static Mesh cube();

    // Re-upload vertex data into the existing (non-indexed) VBO, keeping the same
    // attribute layout/VAO. For CPU animation: skin on the CPU each frame and
    // update the mesh in place. Refreshes the local AABB.
    void update(const std::vector<Vertex>& vertices);
    // Same, with a new index list too: for CPU-built geometry whose triangle
    // count changes from frame to frame (only what is in view, say). The
    // buffers only ever grow (with headroom) and are otherwise refilled at
    // their size -- a buffer re-allocated at a new size every frame makes the
    // driver stall now and then for a whole frame or more.
    void update(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices);

    // Issue the draw call. Assumes a shader is already bound.
    void draw() const;

    // Pull the geometry back off the GPU.
    //
    // create() uploads and forgets: after it returns, the only copy of the mesh
    // is the one in the driver's buffer, and the CPU knows nothing but the
    // bounds. That is right for drawing and wrong for anything that has to
    // REASON about the surface -- the path tracer, which needs triangles, not
    // draw calls. Rather than keep a second copy of every mesh alive for the one
    // time in a session somebody renders a still, this asks the driver for it
    // back at that moment.
    //
    // Costs a full pipeline stall, so it belongs in a one-off (an offline
    // render), never in a frame. Needs a current GL context. An empty MeshData
    // means there was nothing uploaded.
    MeshData readback() const;

    std::uint32_t vertexCount() const { return m_vertexCount; }
    std::uint32_t indexCount()  const { return m_indexCount; }

    // Local-space axis-aligned bounding box (for frustum culling).
    const glm::vec3& boundsMin() const { return m_boundsMin; }
    const glm::vec3& boundsMax() const { return m_boundsMax; }

private:
    static void setVertexLayout();   // on the bound VAO/VBO (create, createView)

    std::uint32_t m_vao         = 0;
    std::uint32_t m_vbo         = 0;
    std::uint32_t m_ebo         = 0;
    std::uint32_t m_vertexCount = 0;
    std::uint32_t m_indexCount  = 0;
    bool          m_ownsVbo     = true;   // false for a view (createView)
    std::size_t   m_vboBytes    = 0;   // allocated sizes, for the streaming update
    std::size_t   m_eboBytes    = 0;
    glm::vec3     m_boundsMin{0.0f};
    glm::vec3     m_boundsMax{0.0f};
};

} // namespace fitzel
