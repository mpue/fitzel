#include "fitzel/graphics/InstancedMesh.hpp"

#include <utility>

#include <glad/gl.h>

namespace fitzel {

InstancedMesh::~InstancedMesh() {
    if (m_instVBO) glDeleteBuffers(1, &m_instVBO);
    if (m_baseVBO) glDeleteBuffers(1, &m_baseVBO);
    if (m_vao)     glDeleteVertexArrays(1, &m_vao);
}

InstancedMesh::InstancedMesh(InstancedMesh&& other) noexcept
    : m_vao(std::exchange(other.m_vao, 0)),
      m_baseVBO(std::exchange(other.m_baseVBO, 0)),
      m_instVBO(std::exchange(other.m_instVBO, 0)),
      m_instStride(std::exchange(other.m_instStride, 0)),
      m_count(std::exchange(other.m_count, 0)) {}

InstancedMesh& InstancedMesh::operator=(InstancedMesh&& other) noexcept {
    if (this != &other) {
        if (m_instVBO) glDeleteBuffers(1, &m_instVBO);
        if (m_baseVBO) glDeleteBuffers(1, &m_baseVBO);
        if (m_vao)     glDeleteVertexArrays(1, &m_vao);
        m_vao        = std::exchange(other.m_vao, 0);
        m_baseVBO    = std::exchange(other.m_baseVBO, 0);
        m_instVBO    = std::exchange(other.m_instVBO, 0);
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
    InstancedMesh m;
    m.m_instStride = instanceStrideBytes;

    glGenVertexArrays(1, &m.m_vao);
    glBindVertexArray(m.m_vao);

    if (baseData && baseFloats > 0) {
        glGenBuffers(1, &m.m_baseVBO);
        glBindBuffer(GL_ARRAY_BUFFER, m.m_baseVBO);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(baseFloats * sizeof(float)),
                     baseData, GL_STATIC_DRAW);
        for (const VertexAttr& a : baseAttrs) {
            glEnableVertexAttribArray(a.location);
            glVertexAttribPointer(a.location, a.components, GL_FLOAT, GL_FALSE,
                                  baseStrideBytes,
                                  reinterpret_cast<void*>(a.offsetBytes));
        }
    }

    glGenBuffers(1, &m.m_instVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m.m_instVBO);
    for (const VertexAttr& a : instanceAttrs) {
        glEnableVertexAttribArray(a.location);
        glVertexAttribPointer(a.location, a.components, GL_FLOAT, GL_FALSE,
                              instanceStrideBytes,
                              reinterpret_cast<void*>(a.offsetBytes));
        glVertexAttribDivisor(a.location, 1);
    }

    glBindVertexArray(0);
    return m;
}

void InstancedMesh::upload(const std::vector<float>& instanceData) {
    glBindBuffer(GL_ARRAY_BUFFER, m_instVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(instanceData.size() * sizeof(float)),
                 instanceData.data(), GL_DYNAMIC_DRAW);
    const int floatsPerInstance = m_instStride / static_cast<int>(sizeof(float));
    m_count = floatsPerInstance > 0
            ? static_cast<int>(instanceData.size()) / floatsPerInstance : 0;
}

void InstancedMesh::draw(unsigned primitive, int vertsPerInstance) const {
    if (m_count <= 0) return;
    glBindVertexArray(m_vao);
    glDrawArraysInstanced(primitive, 0, vertsPerInstance,
                          static_cast<GLsizei>(m_count));
    glBindVertexArray(0);
}

} // namespace fitzel
