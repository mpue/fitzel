#include "fitzel/graphics/Shader.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

#include "fitzel/asset/Vfs.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

namespace fitzel {

namespace {

#ifdef __EMSCRIPTEN__
// Whether the browser lets a vertex shader write gl_ClipDistance. WebGL2 has no
// user clip planes of its own; WEBGL_clip_cull_distance adds them back (Chrome
// has it, not every browser does). Asked once, on the first shader compiled --
// by then the context exists and is current.
bool clipDistanceAvailable() {
    static const bool have = [] {
        const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
        return ctx && emscripten_webgl_enable_extension(ctx, "WEBGL_clip_cull_distance");
    }();
    return have;
}

// WebGL2 speaks GLSL ES 3.00, the shaders are written in desktop GLSL 3.30
// core. The two are close enough that a rewrite at compile time covers every
// shader shipped, so the files stay written for one target:
//  - the version line, and the default precisions ES requires (a fragment
//    shader has no default float precision there, and no sampler type but
//    sampler2D/samplerCube has one in either stage);
//  - FITZEL_WEB defined, for the few places a shader has to choose (the sampler
//    budget of lit.frag: WebGL2 allows 16 per stage);
//  - gl_ClipDistance through the extension when there is one, and dropped when
//    there is not -- the water's reflection then simply is not clipped.
// web/shadercheck.py does the same rewrite offline; keep the two in step.
std::string translateForES(GLenum type, std::string_view source) {
    std::string s(source);
    const std::size_t v = s.find("#version");
    if (v == std::string::npos) return s;
    std::size_t nl = s.find('\n', v);
    if (nl == std::string::npos) nl = s.size() - 1;
    std::string head = "#version 300 es\n";
    const bool clips = type == GL_VERTEX_SHADER &&
                       s.find("gl_ClipDistance") != std::string::npos;
    if (clips && clipDistanceAvailable()) {
        head += "#extension GL_ANGLE_clip_cull_distance : require\n";
    } else if (clips) {
        for (std::size_t p = s.find("gl_ClipDistance"); p != std::string::npos;
             p = s.find("gl_ClipDistance", p)) {
            const std::size_t ls = s.rfind('\n', p);
            const std::size_t le = s.find('\n', p);
            const std::size_t from = (ls == std::string::npos) ? 0 : ls + 1;
            s.erase(from, (le == std::string::npos ? s.size() : le) - from);
            p = from;
        }
    }
    head +=
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp sampler3D;\n"
        "precision highp samplerCube;\n"
        "precision highp sampler2DArray;\n"
        "precision highp sampler2DShadow;\n"
        "precision highp sampler2DArrayShadow;\n"
        "precision highp samplerCubeShadow;\n"
        "precision highp isampler2D;\n"
        "precision highp usampler2D;\n"
        "precision highp isampler3D;\n"
        "precision highp usampler3D;\n"
        "precision highp isampler2DArray;\n"
        "precision highp usampler2DArray;\n"
        "#define FITZEL_WEB 1\n"
        "#line 2\n";   // the driver's error lines stay those of the file
    s.replace(v, nl + 1 - v, head);
    return s;
}

// WebGL2 refuses a draw -- INVALID_OPERATION, nothing drawn -- when two active
// samplers of different types name the same texture unit, whether or not the
// shader reads them on that draw. Every sampler starts at unit 0, so a program
// with a sampler2D and a samplerCube that one path never sets breaks every draw
// of that path; desktop drivers shrug the same thing off. So each sampler TYPE
// starts on a unit of its own (samplers of one type may share), picked from the
// units the browser build leaves unused: the desktop's point-shadow cubes past
// the first (13-15; the browser's array sits on 12), the terrain normal maps
// past the first (19-23), the light grid's G and B (25, 26) and 31. A sampler
// the engine sets moves off its default as on the desktop.
void assignDefaultSamplerUnits(GLuint program) {
    GLint prev = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev);
    glUseProgram(program);
    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; ++i) {
        char name[256];
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, static_cast<GLuint>(i), sizeof(name), &len, &size,
                           &type, name);
        int unit = -1;
        switch (type) {
            case GL_SAMPLER_2D:              unit = 31; break;
            case GL_SAMPLER_2D_ARRAY:        unit = 25; break;
            case GL_SAMPLER_3D:              unit = 13; break;
            case GL_SAMPLER_CUBE:            unit = 14; break;
            case GL_SAMPLER_2D_SHADOW:       unit = 15; break;
            case GL_SAMPLER_2D_ARRAY_SHADOW: unit = 19; break;
            case GL_SAMPLER_CUBE_SHADOW:     unit = 20; break;
            case GL_INT_SAMPLER_2D:
            case GL_UNSIGNED_INT_SAMPLER_2D: unit = 21; break;
            case GL_INT_SAMPLER_3D:
            case GL_UNSIGNED_INT_SAMPLER_3D: unit = 22; break;
            case GL_INT_SAMPLER_2D_ARRAY:
            case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY: unit = 23; break;
            default: break;
        }
        if (unit < 0) continue;
        const GLint loc = glGetUniformLocation(program, name);
        if (loc < 0) continue;
        // An array ("uX[0]") takes every element to the same unit.
        std::vector<GLint> units(static_cast<std::size_t>(std::max(size, 1)), unit);
        glUniform1iv(loc, static_cast<GLsizei>(units.size()), units.data());
    }
    glUseProgram(static_cast<GLuint>(prev));
}
#endif

std::uint32_t compileStage(GLenum type, std::string_view source) {
    const std::uint32_t shader = glCreateShader(type);
#ifdef __EMSCRIPTEN__
    const std::string es = translateForES(type, source);
    source = es;
#endif
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
#ifdef __EMSCRIPTEN__
        assignDefaultSamplerUnits(program);
#endif
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
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
