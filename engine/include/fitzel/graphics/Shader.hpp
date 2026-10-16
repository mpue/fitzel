#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

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

    // A shader file's source as the driver will see it: read through the VFS,
    // with every `#include "file"` (relative to the including file) expanded.
    // Public so a harness that compiles shaders itself compiles the same text.
    static std::string readSource(const std::string& path);

    bool isValid() const { return m_program != 0; }

    void bind() const;
    static void unbind();

    // Make `program` current -- unless it already is. EVERY glUseProgram in
    // the engine and the editor goes through here (or bind/unbind), so the
    // remembered program is the real one; a raw call elsewhere would leave it
    // wrong, and a setter would then write into the wrong program. 0 unbinds.
    static void use(std::uint32_t program);
    // Forget which program is current: the next use() binds whatever it is
    // given. For code that cannot avoid a raw glUseProgram (a third-party
    // backend) and once a frame as a safety net.
    static void forgetBound();

    // Uniform setters (program is bound automatically).
    void setBool(std::string_view name, bool value) const;
    void setInt(std::string_view name, int value) const;
    void setFloat(std::string_view name, float value) const;
    void setVec2(std::string_view name, const glm::vec2& value) const;
    void setVec3(std::string_view name, const glm::vec3& value) const;
    void setVec4(std::string_view name, const glm::vec4& value) const;
    void setMat4(std::string_view name, const glm::mat4& value) const;

    std::uint32_t id() const { return m_program; }

private:
    // A uniform's location, and the last value this object uploaded there, so
    // a setter can skip the upload when nothing changed. The renderer sets
    // ~200 uniforms per draw (audit REN-01), most of them the frame's -- the
    // sun, the fog, the camera -- identical from one draw to the next.
    struct Slot {
        int   loc   = -1;
        bool  known = false;   // `last` holds what is in the program
        float last[16] = {};
    };
    Slot& slot(std::string_view name) const;
    // True when `n` floats at `v` differ from what the slot last uploaded
    // (and records them). Integers are compared by their bits.
    static bool changed(Slot& s, const void* v, std::size_t bytes);
    int uniformLocation(std::string_view name) const;

    // Transparent hash/compare, so looking a name up from a string_view does
    // not have to build a std::string first. Without these the cache would
    // trade the driver call for a heap allocation, which is most of what made
    // the uncached version expensive in the first place.
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    struct NameEq {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept {
            return a == b;
        }
    };

    std::uint32_t m_program = 0;
    // Uniform name -> location. glGetUniformLocation is a string hash inside
    // the driver, and the renderer asks for upwards of a hundred uniforms per
    // draw call, so this is the difference between a CPU-bound and a GPU-bound
    // frame. Mutable because the setters are const and this is pure memoization.
    mutable std::unordered_map<std::string, Slot, NameHash, NameEq> m_uniforms;
};

} // namespace fitzel
