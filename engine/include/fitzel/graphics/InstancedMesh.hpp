#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fitzel {

// One float vertex attribute: its shader `location`, component count and byte
// offset within its (base or instance) vertex.
struct VertexAttr {
    std::uint32_t location;
    int           components;
    std::size_t   offsetBytes;
};

// GPU-instanced geometry: a static base mesh drawn many times from a dynamic
// per-instance buffer. Wraps VAO + base VBO + instance VBO with RAII so the
// sandbox's instanced subsystems (grass, flowers, birds, fireflies...) stop
// juggling raw glGen/glDelete handles. Move-only.
class InstancedMesh {
public:
    InstancedMesh() = default;
    ~InstancedMesh();

    InstancedMesh(const InstancedMesh&)            = delete;
    InstancedMesh& operator=(const InstancedMesh&) = delete;
    InstancedMesh(InstancedMesh&& other) noexcept;
    InstancedMesh& operator=(InstancedMesh&& other) noexcept;

    // Build the VAO. `baseData`/`baseFloats` may be null/0 for gl_VertexID-only
    // meshes (then baseStrideBytes/baseAttrs are ignored). instanceAttrs are set
    // up with vertex divisor 1. All attributes are GL_FLOAT.
    static InstancedMesh create(const float* baseData, std::size_t baseFloats,
                                int baseStrideBytes,
                                const std::vector<VertexAttr>& baseAttrs,
                                int instanceStrideBytes,
                                const std::vector<VertexAttr>& instanceAttrs);

    // Upload per-instance data (dynamic). The instance count is derived from the
    // data size and the instance stride.
    void upload(const std::vector<float>& instanceData);

    // Draw `vertsPerInstance` vertices as `primitive` (a GL enum, passed as an
    // unsigned to keep GL headers out of this interface) for every instance.
    void draw(unsigned primitive, int vertsPerInstance) const;

    int count() const { return m_count; }

private:
    std::uint32_t m_vao       = 0;
    std::uint32_t m_baseVBO   = 0;
    std::uint32_t m_instVBO   = 0;
    int           m_instStride = 0; // bytes per instance
    int           m_count      = 0;
};

} // namespace fitzel
