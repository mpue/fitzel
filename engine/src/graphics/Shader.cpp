#include "fitzel/graphics/Shader.hpp"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/type_ptr.hpp>

#include "fitzel/asset/Vfs.hpp"
#include "fitzel/rhi/Device.hpp"

namespace fitzel {

namespace {

std::string readFile(const std::string& path, int depth = 0) {
    // Through the VFS: in an exported game the shaders live inside the archive
    // like everything else, and a missing shader is a black screen either way.
    std::string src = vfs::readText(path);
    if (src.empty()) {
        std::fprintf(stderr, "[Fitzel] failed to open shader file: %s\n", path.c_str());
        return src;
    }
    // #include "file", relative to this file. GLSL has none of its own, and
    // without it every shader that wants the sun's shadow carries its own copy
    // of the cascade lookup -- five copies of one function, which is four more
    // than stay in step. A #line after each insert keeps the driver's error
    // line numbers pointing into the including file.
    if (depth > 8 || src.find("#include") == std::string::npos) return src;
    const std::size_t slash = path.find_last_of("/\\");
    const std::string dir = (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
    std::string out;
    out.reserve(src.size());
    std::size_t pos = 0;
    int line = 1;
    while (pos < src.size()) {
        std::size_t end = src.find('\n', pos);
        if (end == std::string::npos) end = src.size();
        const std::string_view ln(src.data() + pos, end - pos);
        const std::size_t first = ln.find_first_not_of(" \t");
        if (first != std::string_view::npos && ln.substr(first, 8) == "#include") {
            const std::size_t q0 = ln.find('"'), q1 = ln.rfind('"');
            if (q0 != std::string_view::npos && q1 > q0) {
                out += readFile(dir + std::string(ln.substr(q0 + 1, q1 - q0 - 1)), depth + 1);
                out += "\n#line " + std::to_string(line + 1) + "\n";
            }
        } else {
            out.append(ln);
            out += '\n';
        }
        pos = end + 1;
        ++line;
    }
    return out;
}

} // namespace

Shader::~Shader() {
    if (m_program) rhi::device().destroy(m_program);
}

Shader::Shader(Shader&& other) noexcept
    : m_program(std::exchange(other.m_program, rhi::ProgramHandle{})),
      m_uniforms(std::move(other.m_uniforms)) {
    other.m_uniforms.clear(); // locations belong to the program, which moved
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        if (m_program) rhi::device().destroy(m_program);
        m_program  = std::exchange(other.m_program, rhi::ProgramHandle{});
        m_uniforms = std::move(other.m_uniforms);
        other.m_uniforms.clear();
    }
    return *this;
}

Shader Shader::fromSource(std::string_view vertexSrc, std::string_view fragmentSrc) {
    // The device takes C strings; views into a file need not end in a null.
    const std::string vs(vertexSrc), fs(fragmentSrc);
    rhi::ProgramSource src;
    src.vertex   = vs.c_str();
    src.fragment = fs.c_str();
    Shader result;
    result.m_program = rhi::device().createProgram(src);
    return result;
}

Shader Shader::computeFromSource(std::string_view computeSrc) {
    const std::string cs(computeSrc);
    rhi::ProgramSource src;
    src.compute = cs.c_str();
    Shader result;
    result.m_program = rhi::device().createProgram(src);
    return result;
}

std::string Shader::readSource(const std::string& path) {
    return readFile(path);
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
    rhi::device().bindProgram(m_program);
}

void Shader::unbind() {
    rhi::device().bindProgram(rhi::ProgramHandle{});
}

Shader::Slot& Shader::slot(std::string_view name) const {
    if (const auto it = m_uniforms.find(name); it != m_uniforms.end()) return it->second;
    // The lookup needs a null-terminated string.
    const std::string n(name);
    Slot s;
    s.loc = rhi::device().uniformLocation(m_program, n.c_str());
    // Misses (-1) are cached too. Half the uniforms the renderer sets are
    // deliberately absent from any given shader -- the point-light block on a
    // grass shader, say -- and those are exactly the ones that would otherwise
    // pay the driver lookup on every draw, forever. Now they cost no GL call
    // at all (see the setters).
    return m_uniforms.emplace(n, s).first->second;
}

int Shader::uniformLocation(std::string_view name) const { return slot(name).loc; }

bool Shader::changed(Slot& s, const void* v, std::size_t bytes) {
    if (s.known && std::memcmp(s.last, v, bytes) == 0) return false;
    std::memcpy(s.last, v, bytes);
    s.known = true;
    return true;
}

namespace {
// FITZEL_VERIFY_UNIFORMS=1: every upload a setter SKIPS is checked against
// what the program really holds, and a disagreement is reported once per
// uniform. The skip is only right while nothing writes uniforms behind the
// setters' back; this is how that is checked rather than assumed.
bool verifyUniforms() {
    static const bool on = [] {
        const char* e = std::getenv("FITZEL_VERIFY_UNIFORMS");
        return e && e[0] == '1';
    }();
    return on;
}
void verifySkipped(rhi::ProgramHandle program, int loc, std::string_view name, const void* want,
                   std::size_t bytes, bool isInt) {
    float got[16] = {};
    rhi::device().readUniform(program, loc, isInt, got);
    if (std::memcmp(got, want, bytes) == 0) return;
    static std::vector<std::string> told;
    const std::string n(name);
    if (std::find(told.begin(), told.end(), n) != told.end()) return;
    told.push_back(n);
    std::fprintf(stderr, "[Fitzel] uniform cache out of step: '%s' in program %u\n", n.c_str(),
                 program.id);
}
} // namespace

// Every setter: nothing at all for a uniform the program does not have, and
// nothing but a compare for one whose value is already there. Only a real
// change reaches the device (which makes the program current to upload).
void Shader::setBool(std::string_view name, bool value) const {
    setInt(name, value ? 1 : 0);
}

void Shader::setInt(std::string_view name, int value) const {
    Slot& s = slot(name);
    if (s.loc < 0) return;
    if (!changed(s, &value, sizeof(value))) {
        if (verifyUniforms()) verifySkipped(m_program, s.loc, name, &value, sizeof(value), true);
        return;
    }
    rhi::device().setUniform(m_program, s.loc, rhi::UniformType::Int, &value);
}

void Shader::setFloat(std::string_view name, float value) const {
    Slot& s = slot(name);
    if (s.loc < 0) return;
    if (!changed(s, &value, sizeof(value))) {
        if (verifyUniforms()) verifySkipped(m_program, s.loc, name, &value, sizeof(value), false);
        return;
    }
    rhi::device().setUniform(m_program, s.loc, rhi::UniformType::Float, &value);
}

void Shader::setVec2(std::string_view name, const glm::vec2& value) const {
    Slot& s = slot(name);
    if (s.loc < 0) return;
    if (!changed(s, glm::value_ptr(value), sizeof(float) * 2)) {
        if (verifyUniforms())
            verifySkipped(m_program, s.loc, name, glm::value_ptr(value), sizeof(float) * 2, false);
        return;
    }
    rhi::device().setUniform(m_program, s.loc, rhi::UniformType::Vec2, glm::value_ptr(value));
}

void Shader::setVec3(std::string_view name, const glm::vec3& value) const {
    Slot& s = slot(name);
    if (s.loc < 0) return;
    if (!changed(s, glm::value_ptr(value), sizeof(float) * 3)) {
        if (verifyUniforms())
            verifySkipped(m_program, s.loc, name, glm::value_ptr(value), sizeof(float) * 3, false);
        return;
    }
    rhi::device().setUniform(m_program, s.loc, rhi::UniformType::Vec3, glm::value_ptr(value));
}

void Shader::setVec4(std::string_view name, const glm::vec4& value) const {
    Slot& s = slot(name);
    if (s.loc < 0) return;
    if (!changed(s, glm::value_ptr(value), sizeof(float) * 4)) {
        if (verifyUniforms())
            verifySkipped(m_program, s.loc, name, glm::value_ptr(value), sizeof(float) * 4, false);
        return;
    }
    rhi::device().setUniform(m_program, s.loc, rhi::UniformType::Vec4, glm::value_ptr(value));
}

void Shader::setMat4(std::string_view name, const glm::mat4& value) const {
    Slot& s = slot(name);
    if (s.loc < 0) return;
    if (!changed(s, glm::value_ptr(value), sizeof(float) * 16)) {
        if (verifyUniforms())
            verifySkipped(m_program, s.loc, name, glm::value_ptr(value), sizeof(float) * 16, false);
        return;
    }
    rhi::device().setUniform(m_program, s.loc, rhi::UniformType::Mat4, glm::value_ptr(value));
}

} // namespace fitzel
