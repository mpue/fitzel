#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

namespace fitzel {

// A compiled and linked OpenGL shader program. Move-only RAII wrapper around a
// GL program object.
class Shader {
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&)            = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    // Build from GLSL source strings. Returns an invalid Shader on failure
    // (check with isValid()); compile/link errors are logged to stderr.
    static Shader fromSource(std::string_view vertexSrc, std::string_view fragmentSrc);

    // Build from files on disk.
    static Shader fromFiles(const std::string& vertexPath, const std::string& fragmentPath);

    bool isValid() const { return m_program != 0; }

    void bind() const;
    static void unbind();

    // Uniform setters (program is bound automatically).
    void setBool(std::string_view name, bool value) const;
    void setInt(std::string_view name, int value) const;
    void setFloat(std::string_view name, float value) const;
    void setVec3(std::string_view name, const glm::vec3& value) const;
    void setVec4(std::string_view name, const glm::vec4& value) const;
    void setMat4(std::string_view name, const glm::mat4& value) const;

    std::uint32_t id() const { return m_program; }

private:
    int uniformLocation(std::string_view name) const;

    std::uint32_t m_program = 0;
};

} // namespace fitzel
