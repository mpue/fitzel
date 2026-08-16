#include "fitzel/graphics/Shader.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

#include "fitzel/asset/Vfs.hpp"

namespace fitzel {

namespace {

std::uint32_t compileStage(GLenum type, std::string_view source) {
    const std::uint32_t shader = glCreateShader(type);
    const char* src = source.data();
    const auto  len = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &src, &len);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        const char* stage = (type == GL_VERTEX_SHADER) ? "vertex" : "fragment";
        std::fprintf(stderr, "[Fitzel] %s shader compile error:\n%s\n", stage, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

std::string readFile(const std::string& path) {
    // Through the VFS: in an exported game the shaders live inside the archive
    // like everything else, and a missing shader is a black screen either way.
    std::string src = vfs::readText(path);
    if (src.empty())
        std::fprintf(stderr, "[Fitzel] failed to open shader file: %s\n", path.c_str());
    return src;
}

} // namespace

Shader::~Shader() {
    if (m_program) {
        glDeleteProgram(m_program);
    }
}

Shader::Shader(Shader&& other) noexcept
    : m_program(std::exchange(other.m_program, 0)),
      m_uniforms(std::move(other.m_uniforms)) {
    other.m_uniforms.clear(); // locations belong to the program, which moved
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        if (m_program) {
            glDeleteProgram(m_program);
        }
        m_program  = std::exchange(other.m_program, 0);
        m_uniforms = std::move(other.m_uniforms);
        other.m_uniforms.clear();
    }
    return *this;
}

Shader Shader::fromSource(std::string_view vertexSrc, std::string_view fragmentSrc) {
    Shader result;

    const std::uint32_t vs = compileStage(GL_VERTEX_SHADER, vertexSrc);
    const std::uint32_t fs = compileStage(GL_FRAGMENT_SHADER, fragmentSrc);
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return result; // invalid
    }

    const std::uint32_t program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[Fitzel] shader link error:\n%s\n", log);
        glDeleteProgram(program);
    } else {
        result.m_program = program;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return result;
}

Shader Shader::fromFiles(const std::string& vertexPath, const std::string& fragmentPath) {
    const std::string vsrc = readFile(vertexPath);
    const std::string fsrc = readFile(fragmentPath);
    if (vsrc.empty() || fsrc.empty()) {
        return Shader{};
    }
    return fromSource(vsrc, fsrc);
}

void Shader::bind() const {
    glUseProgram(m_program);
}

void Shader::unbind() {
    glUseProgram(0);
}

int Shader::uniformLocation(std::string_view name) const {
    if (const auto it = m_uniforms.find(name); it != m_uniforms.end()) {
        return it->second;
    }
    // glGetUniformLocation needs a null-terminated string.
    const std::string n(name);
    const int loc = glGetUniformLocation(m_program, n.c_str());
    // Misses (-1) are cached too. Half the uniforms the renderer sets are
    // deliberately absent from any given shader -- the point-light block on a
    // grass shader, say -- and those are exactly the ones that would otherwise
    // pay the driver lookup on every draw, forever.
    m_uniforms.emplace(n, loc);
    return loc;
}

void Shader::setBool(std::string_view name, bool value) const {
    glUseProgram(m_program);
    glUniform1i(uniformLocation(name), static_cast<int>(value));
}

void Shader::setInt(std::string_view name, int value) const {
    glUseProgram(m_program);
    glUniform1i(uniformLocation(name), value);
}

void Shader::setFloat(std::string_view name, float value) const {
    glUseProgram(m_program);
    glUniform1f(uniformLocation(name), value);
}

void Shader::setVec2(std::string_view name, const glm::vec2& value) const {
    glUseProgram(m_program);
    glUniform2fv(uniformLocation(name), 1, glm::value_ptr(value));
}

void Shader::setVec3(std::string_view name, const glm::vec3& value) const {
    glUseProgram(m_program);
    glUniform3fv(uniformLocation(name), 1, glm::value_ptr(value));
}

void Shader::setVec4(std::string_view name, const glm::vec4& value) const {
    glUseProgram(m_program);
    glUniform4fv(uniformLocation(name), 1, glm::value_ptr(value));
}

void Shader::setMat4(std::string_view name, const glm::mat4& value) const {
    glUseProgram(m_program);
    glUniformMatrix4fv(uniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
}

} // namespace fitzel
