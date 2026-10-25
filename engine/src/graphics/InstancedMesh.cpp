#include "fitzel/graphics/InstancedMesh.hpp"

#include <utility>

#include "fitzel/rhi/Device.hpp"

namespace fitzel {

namespace {
rhi::VertexFormat floats(int components) {
    switch (components) {
        case 1:  return rhi::VertexFormat::Float;
        case 2:  return rhi::VertexFormat::Float2;
        case 3:  return rhi::VertexFormat::Float3;
        default: return rhi::VertexFormat::Float4;
    }
}
} // namespace

InstancedMesh::~InstancedMesh() {
    rhi::Device& gpu = rhi::device();
    if (m_instVBO) gpu.destroy(m_instVBO);
    if (m_baseVBO) gpu.destroy(m_baseVBO);
    if (m_vao)     gpu.destroy(m_vao);
}

InstancedMesh::InstancedMesh(InstancedMesh&& other) noexcept
    : m_vao(std::exchange(other.m_vao, rhi::VertexArrayHandle{})),
      m_baseVBO(std::exchange(other.m_baseVBO, rhi::BufferHandle{})),
      m_instVBO(std::exchange(other.m_instVBO, rhi::BufferHandle{})),
      m_instStride(std::exchange(other.m_instStride, 0)),
      m_count(std::exchange(other.m_count, 0)) {}

InstancedMesh& InstancedMesh::operator=(InstancedMesh&& other) noexcept {
    if (this != &other) {
        rhi::Device& gpu = rhi::device();
        if (m_instVBO) gpu.destroy(m_instVBO);
        if (m_baseVBO) gpu.destroy(m_baseVBO);
        if (m_vao)     gpu.destroy(m_vao);
        m_vao        = std::exchange(other.m_vao, rhi::VertexArrayHandle{});
        m_baseVBO    = std::exchange(other.m_baseVBO, rhi::BufferHandle{});
        m_instVBO    = std::exchange(other.m_instVBO, rhi::BufferHandle{});
        m_instStride = std::exchange(other.m_instStride, 0);
        m_count      = std::exchange(other.m_count, 0);
    }
    return *this;
}

InstancedMesh InstancedMesh::create(const float* baseData, std::size_t baseFloats,
                                    int baseStrideBytes,
                                    const std::vector<VertexAttr>& baseAttrs,
                                    int instanceStrideBytes,
                                    const std::vector<VertexAttr>& instanceAttrs) {
    rhi::Device& gpu = rhi::device();
    InstancedMesh m;
    m.m_instStride = instanceStrideBytes;

    std::vector<rhi::VertexAttribute> attrs;
    if (baseData && baseFloats > 0) {
        m.m_baseVBO = gpu.createBuffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static,
                                       baseFloats * sizeof(float), baseData);
        for (const VertexAttr& a : baseAttrs)
            attrs.push_back({a.location, m.m_baseVBO, floats(a.components),
                             static_cast<std::uint32_t>(a.offsetBytes),
                             static_cast<std::uint32_t>(baseStrideBytes), 0});
    }
    m.m_instVBO = gpu.createBuffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Dynamic, 0);
    for (const VertexAttr& a : instanceAttrs)
        attrs.push_back({a.location, m.m_instVBO, floats(a.components),
                         static_cast<std::uint32_t>(a.offsetBytes),
                         static_cast<std::uint32_t>(instanceStrideBytes), 1});

    rhi::VertexArrayDesc d;
    d.attributes     = attrs.data();
    d.attributeCount = attrs.size();
    m.m_vao = gpu.createVertexArray(d);
    return m;
}

void InstancedMesh::upload(const std::vector<float>& instanceData) {
    rhi::device().bufferData(m_instVBO, instanceData.size() * sizeof(float), instanceData.data(),
                             rhi::BufferUsage::Dynamic);
    const int floatsPerInstance = m_instStride / static_cast<int>(sizeof(float));
    m_count = floatsPerInstance > 0
            ? static_cast<int>(instanceData.size()) / floatsPerInstance : 0;
}

void InstancedMesh::draw(rhi::Primitive primitive, int vertsPerInstance) const {
    if (m_count <= 0) return;
    rhi::Device& gpu = rhi::device();
    gpu.bindVertexArray(m_vao);
    gpu.draw(primitive, 0, vertsPerInstance, m_count);
    gpu.bindVertexArray(rhi::VertexArrayHandle{});
}

} // namespace fitzel
