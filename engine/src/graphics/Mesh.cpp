#include "fitzel/graphics/Mesh.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <atomic>
#include <cstdio>
#include <limits>
#include <utility>

#include "fitzel/rhi/Device.hpp"

namespace fitzel {

namespace {
std::uint64_t nextRevision() {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}
} // namespace

IndexBuffer::IndexBuffer(const std::vector<std::uint32_t>& indices)
    : m_count(static_cast<std::uint32_t>(indices.size())) {
    m_ebo = rhi::device().createBuffer(rhi::BufferKind::Index, rhi::BufferUsage::Static,
                                       indices.size() * sizeof(std::uint32_t), indices.data());
}

IndexBuffer::~IndexBuffer() {
    if (m_ebo) rhi::device().destroy(m_ebo);
}

Mesh::~Mesh() {
    rhi::Device& gpu = rhi::device();
    if (m_ebo && !m_sharedIndices) gpu.destroy(m_ebo);
    if (m_vbo && m_ownsVbo) gpu.destroy(m_vbo);
    if (m_vao) gpu.destroy(m_vao);
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_vao(std::exchange(other.m_vao, rhi::VertexArrayHandle{})),
      m_vbo(std::exchange(other.m_vbo, rhi::BufferHandle{})),
      m_ebo(std::exchange(other.m_ebo, rhi::BufferHandle{})),
      m_vertexCount(std::exchange(other.m_vertexCount, 0)),
      m_indexCount(std::exchange(other.m_indexCount, 0)),
      m_ownsVbo(std::exchange(other.m_ownsVbo, true)),
      m_vboBytes(std::exchange(other.m_vboBytes, 0)),
      m_eboBytes(std::exchange(other.m_eboBytes, 0)),
      m_sharedIndices(std::move(other.m_sharedIndices)),
      m_boundsMin(other.m_boundsMin),
      m_boundsMax(other.m_boundsMax),
      m_revision(std::exchange(other.m_revision, 0)) {}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (this != &other) {
        rhi::Device& gpu = rhi::device();
        if (m_ebo && !m_sharedIndices) gpu.destroy(m_ebo);
        if (m_vbo && m_ownsVbo) gpu.destroy(m_vbo);
        if (m_vao) gpu.destroy(m_vao);

        m_vao         = std::exchange(other.m_vao, rhi::VertexArrayHandle{});
        m_vbo         = std::exchange(other.m_vbo, rhi::BufferHandle{});
        m_ebo         = std::exchange(other.m_ebo, rhi::BufferHandle{});
        m_vertexCount = std::exchange(other.m_vertexCount, 0);
        m_indexCount  = std::exchange(other.m_indexCount, 0);
        m_ownsVbo     = std::exchange(other.m_ownsVbo, true);
        m_vboBytes    = std::exchange(other.m_vboBytes, 0);
        m_eboBytes    = std::exchange(other.m_eboBytes, 0);
        m_sharedIndices = std::move(other.m_sharedIndices);
        other.m_sharedIndices.reset();
        m_boundsMin   = other.m_boundsMin;
        m_boundsMax   = other.m_boundsMax;
        m_revision    = std::exchange(other.m_revision, 0);
    }
    return *this;
}

Mesh Mesh::create(const MeshData& data) {
    return create(data.vertices, data.indices);
}

Mesh Mesh::create(const std::vector<Vertex>& vertices,
                  const std::vector<std::uint32_t>& indices) {
    Mesh mesh;
    mesh.m_revision    = nextRevision();
    mesh.m_vertexCount = static_cast<std::uint32_t>(vertices.size());
    mesh.m_indexCount  = static_cast<std::uint32_t>(indices.size());
    mesh.m_vboBytes    = vertices.size() * sizeof(Vertex);
    mesh.m_eboBytes    = indices.size() * sizeof(std::uint32_t);

    // Local-space AABB for frustum culling.
    constexpr float inf = std::numeric_limits<float>::max();
    glm::vec3 lo{inf}, hi{-inf};
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    if (!vertices.empty()) {
        mesh.m_boundsMin = lo;
        mesh.m_boundsMax = hi;
    }

    rhi::Device& gpu = rhi::device();
    mesh.m_vbo = gpu.createBuffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static,
                                  mesh.m_vboBytes, vertices.data());
    if (!indices.empty())
        mesh.m_ebo = gpu.createBuffer(rhi::BufferKind::Index, rhi::BufferUsage::Static,
                                      mesh.m_eboBytes, indices.data());
    mesh.m_vao = makeVertexArray(mesh.m_vbo, mesh.m_ebo);
    return mesh;
}

namespace {
// The Vertex struct as attributes: position, normal, uv, paint.
std::array<rhi::VertexAttribute, 4> vertexLayout(rhi::BufferHandle vbo) {
    constexpr auto stride = static_cast<std::uint32_t>(sizeof(Vertex));
    return {{
        // layout(location = 0) in vec3 aPos;
        {0, vbo, rhi::VertexFormat::Float3, offsetof(Vertex, position), stride, 0},
        // layout(location = 1) in vec3 aNormal;
        {1, vbo, rhi::VertexFormat::Float3, offsetof(Vertex, normal), stride, 0},
        // layout(location = 2) in vec2 aUV;
        {2, vbo, rhi::VertexFormat::Float2, offsetof(Vertex, uv), stride, 0},
        // layout(location = 3) in vec4 aPaint; (terrain texture-paint weights)
        {3, vbo, rhi::VertexFormat::Float4, offsetof(Vertex, paint), stride, 0},
    }};
}
} // namespace

rhi::VertexArrayHandle Mesh::makeVertexArray(rhi::BufferHandle vbo, rhi::BufferHandle ebo) {
    const auto attrs = vertexLayout(vbo);
    rhi::VertexArrayDesc d;
    d.attributes     = attrs.data();
    d.attributeCount = attrs.size();
    d.indexBuffer    = ebo;
    return rhi::device().createVertexArray(d);
}

void Mesh::relinkIndices() {
    const auto attrs = vertexLayout(m_vbo);
    rhi::VertexArrayDesc d;
    d.attributes     = attrs.data();
    d.attributeCount = attrs.size();
    d.indexBuffer    = m_ebo;
    rhi::device().setVertexArray(m_vao, d);
}

Mesh Mesh::createView(const Mesh& base, const std::vector<std::uint32_t>& indices) {
    Mesh view;
    if (!base.m_vbo || indices.empty()) return view;
    view.m_revision    = nextRevision();
    view.m_ownsVbo     = false;
    view.m_vbo         = base.m_vbo;
    view.m_vertexCount = base.m_vertexCount;
    view.m_indexCount  = static_cast<std::uint32_t>(indices.size());
    view.m_eboBytes    = indices.size() * sizeof(std::uint32_t);
    view.m_boundsMin   = base.m_boundsMin;
    view.m_boundsMax   = base.m_boundsMax;
    view.m_ebo = rhi::device().createBuffer(rhi::BufferKind::Index, rhi::BufferUsage::Static,
                                            view.m_eboBytes, indices.data());
    view.m_vao = makeVertexArray(view.m_vbo, view.m_ebo);
    return view;
}

Mesh Mesh::create(const std::vector<Vertex>& vertices,
                  std::shared_ptr<const IndexBuffer> indices) {
    Mesh mesh = create(vertices);
    if (!indices || !mesh.m_vao) return mesh;
    mesh.m_indexCount    = indices->count();
    mesh.m_ebo           = indices->id();
    mesh.m_sharedIndices = std::move(indices);
    mesh.relinkIndices();
    return mesh;
}

Mesh Mesh::createView(const Mesh& base, std::shared_ptr<const IndexBuffer> indices) {
    Mesh view;
    if (!base.m_vbo || !indices || indices->count() == 0) return view;
    view.m_revision      = nextRevision();
    view.m_ownsVbo       = false;
    view.m_vbo           = base.m_vbo;
    view.m_vertexCount   = base.m_vertexCount;
    view.m_indexCount    = indices->count();
    view.m_ebo           = indices->id();
    view.m_sharedIndices = std::move(indices);
    view.m_boundsMin     = base.m_boundsMin;
    view.m_boundsMax     = base.m_boundsMax;
    view.m_vao = makeVertexArray(view.m_vbo, view.m_ebo);
    return view;
}

Mesh Mesh::cube() {
    // 6 faces * 4 vertices, indexed. Each face has a flat normal and a full
    // 0..1 UV quad.
    const std::vector<Vertex> vertices = {
        // +Z (front)
        {{-0.5f, -0.5f,  0.5f}, {0, 0, 1}, {0, 0}},
        {{ 0.5f, -0.5f,  0.5f}, {0, 0, 1}, {1, 0}},
        {{ 0.5f,  0.5f,  0.5f}, {0, 0, 1}, {1, 1}},
        {{-0.5f,  0.5f,  0.5f}, {0, 0, 1}, {0, 1}},
        // -Z (back)
        {{ 0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0, 0}},
        {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 0}},
        {{-0.5f,  0.5f, -0.5f}, {0, 0, -1}, {1, 1}},
        {{ 0.5f,  0.5f, -0.5f}, {0, 0, -1}, {0, 1}},
        // +X (right)
        {{ 0.5f, -0.5f,  0.5f}, {1, 0, 0}, {0, 0}},
        {{ 0.5f, -0.5f, -0.5f}, {1, 0, 0}, {1, 0}},
        {{ 0.5f,  0.5f, -0.5f}, {1, 0, 0}, {1, 1}},
        {{ 0.5f,  0.5f,  0.5f}, {1, 0, 0}, {0, 1}},
        // -X (left)
        {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {0, 0}},
        {{-0.5f, -0.5f,  0.5f}, {-1, 0, 0}, {1, 0}},
        {{-0.5f,  0.5f,  0.5f}, {-1, 0, 0}, {1, 1}},
        {{-0.5f,  0.5f, -0.5f}, {-1, 0, 0}, {0, 1}},
        // +Y (top)
        {{-0.5f,  0.5f,  0.5f}, {0, 1, 0}, {0, 0}},
        {{ 0.5f,  0.5f,  0.5f}, {0, 1, 0}, {1, 0}},
        {{ 0.5f,  0.5f, -0.5f}, {0, 1, 0}, {1, 1}},
        {{-0.5f,  0.5f, -0.5f}, {0, 1, 0}, {0, 1}},
        // -Y (bottom)
        {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0, 0}},
        {{ 0.5f, -0.5f, -0.5f}, {0, -1, 0}, {1, 0}},
        {{ 0.5f, -0.5f,  0.5f}, {0, -1, 0}, {1, 1}},
        {{-0.5f, -0.5f,  0.5f}, {0, -1, 0}, {0, 1}},
    };

    std::vector<std::uint32_t> indices;
    indices.reserve(36);
    for (std::uint32_t face = 0; face < 6; ++face) {
        const std::uint32_t base = face * 4;
        indices.insert(indices.end(),
                       {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});
    }

    return create(vertices, indices);
}

namespace {
// Refill a streamed buffer: orphan it at its allocated size (the driver hands
// back fresh memory while the GPU may still read the old) and write the data
// in; grow it, with half again as headroom, only when the data outgrows it.
void stream(rhi::BufferHandle buf, const void* data, std::size_t bytes, std::size_t& allocated) {
    if (bytes > allocated) allocated = std::max<std::size_t>(bytes + bytes / 2, 64 * 1024);
    rhi::device().bufferData(buf, allocated, nullptr, rhi::BufferUsage::Stream);
    if (bytes > 0) rhi::device().bufferSubData(buf, 0, bytes, data);
}
} // namespace

void Mesh::update(const std::vector<Vertex>& vertices) {
    if (!m_vbo) { *this = create(vertices); return; }
    // A view borrows its base's vertex buffer: writing it would change the
    // base under its owner. And an indexed mesh keeps its element buffer, so
    // fewer vertices than before would leave indices pointing past the end.
    if (!m_ownsVbo || (m_ebo && vertices.size() < m_vertexCount)) {
        std::fprintf(stderr, "[Fitzel] Mesh::update refused: %s\n",
                     m_ownsVbo ? "fewer vertices than the index list addresses"
                               : "the mesh is a view of another");
        return;
    }
    m_revision    = nextRevision();
    m_vertexCount = static_cast<std::uint32_t>(vertices.size());
    stream(m_vbo, vertices.data(), vertices.size() * sizeof(Vertex), m_vboBytes);
    constexpr float inf = std::numeric_limits<float>::max();
    glm::vec3 lo{inf}, hi{-inf};
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    if (!vertices.empty()) { m_boundsMin = lo; m_boundsMax = hi; }
}

void Mesh::update(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
    if (!m_vao) { *this = create(vertices, indices); return; }
    if (!m_ownsVbo) {
        std::fprintf(stderr, "[Fitzel] Mesh::update refused: the mesh is a view of another\n");
        return;
    }
    m_revision    = nextRevision();
    m_vertexCount = static_cast<std::uint32_t>(vertices.size());
    m_indexCount  = static_cast<std::uint32_t>(indices.size());
    stream(m_vbo, vertices.data(), vertices.size() * sizeof(Vertex), m_vboBytes);
    bool relink = false;
    if (m_sharedIndices) {   // not ours to stream into: get our own
        m_sharedIndices.reset();
        m_ebo      = rhi::BufferHandle{};
        m_eboBytes = 0;
    }
    if (!m_ebo) {
        m_ebo = rhi::device().createBuffer(rhi::BufferKind::Index, rhi::BufferUsage::Stream, 0);
        relink = true;
    }
    stream(m_ebo, indices.data(), indices.size() * sizeof(std::uint32_t), m_eboBytes);
    if (relink) relinkIndices();   // the index buffer belongs to the vertex array
    constexpr float inf = std::numeric_limits<float>::max();
    glm::vec3 lo{inf}, hi{-inf};
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    if (!vertices.empty()) { m_boundsMin = lo; m_boundsMax = hi; }
}

MeshData Mesh::readback() const {
    MeshData data;
    if (!m_vbo || m_vertexCount == 0) return data;

    // The vertex buffer holds exactly the Vertex struct that create() wrote, so
    // the read is a straight memcpy out of the driver -- no attribute walking, no
    // format guessing. If the layout in Mesh.hpp ever changes, this follows it
    // for free.
    data.vertices.resize(m_vertexCount);
    rhi::device().readBuffer(m_vbo, 0, m_vertexCount * sizeof(Vertex), data.vertices.data());
    if (m_ebo && m_indexCount > 0) {
        data.indices.resize(m_indexCount);
        rhi::device().readBuffer(m_ebo, 0, m_indexCount * sizeof(std::uint32_t),
                                 data.indices.data());
    }
    return data;
}

void Mesh::draw() const {
    rhi::Device& gpu = rhi::device();
    gpu.bindVertexArray(m_vao);
    if (m_indexCount > 0)
        gpu.drawIndexed(rhi::Primitive::Triangles, static_cast<int>(m_indexCount),
                        rhi::IndexType::U32);
    else
        gpu.draw(rhi::Primitive::Triangles, 0, static_cast<int>(m_vertexCount));
    gpu.bindVertexArray(rhi::VertexArrayHandle{});
}

} // namespace fitzel
