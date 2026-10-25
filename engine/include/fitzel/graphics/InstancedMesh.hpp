#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fitzel/rhi/Types.hpp"

namespace fitzel {

// One float vertex attribute: its shader `location`, component count and byte
// offset within its (base or instance) vertex.
struct VertexAttr {
    std::uint32_t location;
    int           components;
    std::size_t   offsetBytes;
};

// GPU-instanced geometry: a static base mesh drawn many times from a dynamic
// per-instance buffer. Wraps vertex array + base buffer + instance buffer with
// RAII so the sandbox's instanced subsystems (grass, flowers, birds,
// fireflies...) stop juggling raw device handles. Move-only.
class InstancedMesh {
public:
    InstancedMesh() = default;
    ~InstancedMesh();

    InstancedMesh(const InstancedMesh&)            = delete;
    InstancedMesh& operator=(const InstancedMesh&) = delete;
    InstancedMesh(InstancedMesh&& other) noexcept;
    InstancedMesh& operator=(InstancedMesh&& other) noexcept;

    // Build the vertex array. `baseData`/`baseFloats` may be null/0 for
    // gl_VertexID-only meshes (then baseStrideBytes/baseAttrs are ignored).
    // instanceAttrs advance per instance. All attributes are floats.
    static InstancedMesh create(const float* baseData, std::size_t baseFloats,
                                int baseStrideBytes,
                                const std::vector<VertexAttr>& baseAttrs,
                                int instanceStrideBytes,
                                const std::vector<VertexAttr>& instanceAttrs);

    // Upload per-instance data (dynamic). The instance count is derived from the
    // data size and the instance stride.
    void upload(const std::vector<float>& instanceData);

    // Draw `vertsPerInstance` vertices as `primitive` for every instance.
    void draw(rhi::Primitive primitive, int vertsPerInstance) const;

    int count() const { return m_count; }

private:
    rhi::VertexArrayHandle m_vao;
    rhi::BufferHandle      m_baseVBO;
    rhi::BufferHandle      m_instVBO;
    int           m_instStride = 0; // bytes per instance
    int           m_count      = 0;
};

} // namespace fitzel
