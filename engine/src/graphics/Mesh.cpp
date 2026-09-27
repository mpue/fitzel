#include "fitzel/graphics/Mesh.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <glad/gl.h>

namespace fitzel {

Mesh::~Mesh() {
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_vbo && m_ownsVbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_vao(std::exchange(other.m_vao, 0)),
      m_vbo(std::exchange(other.m_vbo, 0)),
      m_ebo(std::exchange(other.m_ebo, 0)),
      m_vertexCount(std::exchange(other.m_vertexCount, 0)),
      m_indexCount(std::exchange(other.m_indexCount, 0)),
      m_ownsVbo(std::exchange(other.m_ownsVbo, true)),
      m_vboBytes(std::exchange(other.m_vboBytes, 0)),
      m_eboBytes(std::exchange(other.m_eboBytes, 0)),
      m_boundsMin(other.m_boundsMin),
      m_boundsMax(other.m_boundsMax) {}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (this != &other) {
        if (m_ebo) glDeleteBuffers(1, &m_ebo);
        if (m_vbo && m_ownsVbo) glDeleteBuffers(1, &m_vbo);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);

        m_vao         = std::exchange(other.m_vao, 0);
        m_vbo         = std::exchange(other.m_vbo, 0);
        m_ebo         = std::exchange(other.m_ebo, 0);
        m_vertexCount = std::exchange(other.m_vertexCount, 0);
        m_indexCount  = std::exchange(other.m_indexCount, 0);
        m_ownsVbo     = std::exchange(other.m_ownsVbo, true);
        m_vboBytes    = std::exchange(other.m_vboBytes, 0);
        m_eboBytes    = std::exchange(other.m_eboBytes, 0);
        m_boundsMin   = other.m_boundsMin;
        m_boundsMax   = other.m_boundsMax;
    }
    return *this;
}

Mesh Mesh::create(const MeshData& data) {
    return create(data.vertices, data.indices);
}

Mesh Mesh::create(const std::vector<Vertex>& vertices,
                  const std::vector<std::uint32_t>& indices) {
    Mesh mesh;
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

    glGenVertexArrays(1, &mesh.m_vao);
    glBindVertexArray(mesh.m_vao);

    glGenBuffers(1, &mesh.m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                 vertices.data(), GL_STATIC_DRAW);

    if (!indices.empty()) {
        glGenBuffers(1, &mesh.m_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.m_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
                     indices.data(), GL_STATIC_DRAW);
    }

    setVertexLayout();
    glBindVertexArray(0);
    return mesh;
}

// The Vertex layout on the bound VAO/VBO: position, normal, uv, paint.
void Mesh::setVertexLayout() {
    // layout(location = 0) in vec3 aPos;
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, position)));

    // layout(location = 1) in vec3 aNormal;
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, normal)));

    // layout(location = 2) in vec2 aUV;
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, uv)));

    // layout(location = 3) in vec4 aPaint; (terrain texture-paint weights)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, paint)));
}

Mesh Mesh::createView(const Mesh& base, const std::vector<std::uint32_t>& indices) {
    Mesh view;
    if (!base.m_vbo || indices.empty()) return view;
    view.m_ownsVbo     = false;
    view.m_vbo         = base.m_vbo;
    view.m_vertexCount = base.m_vertexCount;
    view.m_indexCount  = static_cast<std::uint32_t>(indices.size());
    view.m_eboBytes    = indices.size() * sizeof(std::uint32_t);
    view.m_boundsMin   = base.m_boundsMin;
    view.m_boundsMax   = base.m_boundsMax;
    glGenVertexArrays(1, &view.m_vao);
    glBindVertexArray(view.m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, view.m_vbo);
    glGenBuffers(1, &view.m_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, view.m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(view.m_eboBytes),
                 indices.data(), GL_STATIC_DRAW);
    setVertexLayout();
    glBindVertexArray(0);
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

void Mesh::update(const std::vector<Vertex>& vertices) {
    if (m_vbo == 0) { *this = create(vertices); return; }
    m_vertexCount = static_cast<std::uint32_t>(vertices.size());
    m_vboBytes    = vertices.size() * sizeof(Vertex);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_vboBytes), vertices.data(),
                 GL_DYNAMIC_DRAW);
    constexpr float inf = std::numeric_limits<float>::max();
    glm::vec3 lo{inf}, hi{-inf};
    for (const Vertex& v : vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    if (!vertices.empty()) { m_boundsMin = lo; m_boundsMax = hi; }
}

namespace {
// Refill a streamed buffer: orphan it at its allocated size (the driver hands
// back fresh memory while the GPU may still read the old) and write the data
// in; grow it, with half again as headroom, only when the data outgrows it.
void stream(GLenum target, const void* data, std::size_t bytes, std::size_t& allocated) {
    if (bytes > allocated) allocated = std::max<std::size_t>(bytes + bytes / 2, 64 * 1024);
    glBufferData(target, static_cast<GLsizeiptr>(allocated), nullptr, GL_STREAM_DRAW);
    if (bytes > 0) glBufferSubData(target, 0, static_cast<GLsizeiptr>(bytes), data);
}
} // namespace

void Mesh::update(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices) {
    if (m_vao == 0) { *this = create(vertices, indices); return; }
    m_vertexCount = static_cast<std::uint32_t>(vertices.size());
    m_indexCount  = static_cast<std::uint32_t>(indices.size());
    // The element buffer binding lives in the VAO.
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    stream(GL_ARRAY_BUFFER, vertices.data(), vertices.size() * sizeof(Vertex), m_vboBytes);
    if (m_ebo == 0) glGenBuffers(1, &m_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    stream(GL_ELEMENT_ARRAY_BUFFER, indices.data(), indices.size() * sizeof(std::uint32_t),
           m_eboBytes);
    glBindVertexArray(0);
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

    // The VBO holds exactly the Vertex struct that create() wrote, so the read
    // is a straight memcpy out of the driver -- no attribute walking, no format
    // guessing. If the layout in Mesh.hpp ever changes, this follows it for free.
    data.vertices.resize(m_vertexCount);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glGetBufferSubData(GL_ARRAY_BUFFER, 0,
                       static_cast<GLsizeiptr>(m_vertexCount * sizeof(Vertex)),
                       data.vertices.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (m_ebo && m_indexCount > 0) {
        data.indices.resize(m_indexCount);
        // Bound to the plain target, not through the VAO: binding an element
        // buffer while a VAO is current would rewrite that VAO's index binding.
        glBindVertexArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0,
                           static_cast<GLsizeiptr>(m_indexCount * sizeof(std::uint32_t)),
                           data.indices.data());
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
    return data;
}

void Mesh::draw() const {
    glBindVertexArray(m_vao);
    if (m_indexCount > 0) {
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_indexCount),
                       GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertexCount));
    }
    glBindVertexArray(0);
}

} // namespace fitzel
