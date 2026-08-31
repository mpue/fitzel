#include "GpuTrace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <glad/gl.h>
#include <glm/gtc/constants.hpp>

#include <fitzel/core/GlCaps.hpp>

namespace gputrace {
namespace {

// The GPU's view of the scene. Every member padded to a vec4 rather than packed
// tight: std430 would allow tighter, and the tightest layout is where a GPU
// port spends its debugging time. A vec3 inside an array is 16 bytes on the
// device and 12 on the host, and the picture that comes out of getting that
// wrong looks like a broken accelerator rather than like a padding mistake.
struct GpuTri {
    float p0[4], p1[4], p2[4];
    float n0[4], n1[4], n2[4];
    float uv01[4];
    float uv2mat[4];
};
static_assert(sizeof(GpuTri) == 128, "GpuTri must match its std430 counterpart");

struct GpuNode {
    float lo[4];   // .w = leftFirst, bit-cast
    float hi[4];   // .w = count, bit-cast
};
static_assert(sizeof(GpuNode) == 32, "GpuNode must match its std430 counterpart");

struct GpuMat {
    float albedoRough[4];
    float emissionRefl[4];
    float misc[4];
};
static_assert(sizeof(GpuMat) == 48, "GpuMat must match its std430 counterpart");

struct GpuLamp {
    float posRange[4];
    float dirRadius[4];
    float color[4];
    float cone[4];
};
static_assert(sizeof(GpuLamp) == 64, "GpuLamp must match its std430 counterpart");

float asFloat(int i) {
    float f = 0.0f;
    static_assert(sizeof(f) == sizeof(i), "int and float must be the same width");
    std::memcpy(&f, &i, sizeof f);
    return f;
}

void put3(float* dst, const glm::vec3& v, float w = 0.0f) {
    dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; dst[3] = w;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void setVec3(unsigned program, const char* name, const glm::vec3& v) {
    glUniform3f(glGetUniformLocation(program, name), v.x, v.y, v.z);
}
void setFloat(unsigned program, const char* name, float v) {
    glUniform1f(glGetUniformLocation(program, name), v);
}
void setInt(unsigned program, const char* name, int v) {
    glUniform1i(glGetUniformLocation(program, name), v);
}

} // namespace

bool available() { return fitzel::glcaps::compute(); }

Tracer::~Tracer() { release(); }

void Tracer::release() {
    if (m_program) glDeleteProgram(m_program);
    if (m_accum)   glDeleteTextures(1, &m_accum);
    for (std::uint32_t& b : m_ssbo)
        if (b) { glDeleteBuffers(1, &b); b = 0; }
    m_program = 0;
    m_accum   = 0;
}

bool Tracer::init(const std::string& shaderPath) {
    if (!available()) {
        m_error = "this context is OpenGL " +
                  std::to_string(fitzel::glcaps::majorVersion()) + "." +
                  std::to_string(fitzel::glcaps::minorVersion()) +
                  "; the GPU tracer needs 4.3 (compute shaders)";
        return false;
    }
    const std::string src = readFile(shaderPath);
    if (src.empty()) {
        m_error = "could not read the kernel: " + shaderPath;
        return false;
    }

    const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char*  cstr   = src.c_str();
    glShaderSource(shader, 1, &cstr, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        // The log, verbatim. A shader that will not build is the one case where
        // a summary is worth nothing: the line number is the whole message.
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetShaderInfoLog(shader, len, nullptr, log.data());
        glDeleteShader(shader);
        m_error = "gputrace.comp did not compile:\n" + log;
        return false;
    }

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, shader);
    glLinkProgram(prog);
    glDeleteShader(shader);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        glDeleteProgram(prog);
        m_error = "gputrace.comp did not link:\n" + log;
        return false;
    }

    if (m_program) glDeleteProgram(m_program);
    m_program = prog;
    return true;
}

bool Tracer::uploadBuffer(std::uint32_t& id, int binding, const void* data,
                          std::size_t bytes) {
    if (!id) glGenBuffers(1, &id);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, id);
    // Never zero bytes: a binding of size 0 is not a buffer the driver has to
    // accept, and an empty lamp list is the normal case rather than an error.
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(std::max<std::size_t>(bytes, 16)),
                 bytes ? data : nullptr, GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(binding), id);
    return glGetError() == GL_NO_ERROR;
}

bool Tracer::upload(const pathtrace::Scene& scene) {
    if (!m_program) { m_error = "upload() before a kernel was built"; return false; }
    if (scene.triangles.empty()) {
        m_error = "the scene has no triangles";
        return false;
    }

    // The tracer's own accelerator, not one of ours. See BvhNode in
    // PathTrace.hpp for why that matters more than it looks.
    const pathtrace::BvhData bvh = pathtrace::buildBvh(scene.triangles);

    std::vector<GpuTri> tris(scene.triangles.size());
    for (std::size_t i = 0; i < scene.triangles.size(); ++i) {
        const pathtrace::Triangle& t = scene.triangles[i];
        GpuTri& g = tris[i];
        put3(g.p0, t.p0); put3(g.p1, t.p1); put3(g.p2, t.p2);
        put3(g.n0, t.n0); put3(g.n1, t.n1); put3(g.n2, t.n2);
        g.uv01[0] = t.uv0.x; g.uv01[1] = t.uv0.y;
        g.uv01[2] = t.uv1.x; g.uv01[3] = t.uv1.y;
        g.uv2mat[0] = t.uv2.x;
        g.uv2mat[1] = t.uv2.y;
        g.uv2mat[2] = static_cast<float>(t.material);
        g.uv2mat[3] = 0.0f;
    }

    std::vector<GpuNode> nodes(bvh.nodes.size());
    for (std::size_t i = 0; i < bvh.nodes.size(); ++i) {
        const pathtrace::BvhNode& n = bvh.nodes[i];
        put3(nodes[i].lo, n.lo, asFloat(n.leftFirst));
        put3(nodes[i].hi, n.hi, asFloat(n.count));
    }

    std::vector<GpuMat> mats(std::max<std::size_t>(scene.materials.size(), 1));
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
        const pathtrace::Material& m = scene.materials[i];
        GpuMat& g = mats[i];
        put3(g.albedoRough, m.albedo, m.roughness);
        put3(g.emissionRefl, m.emission, m.reflectivity);
        g.misc[0] = m.opacity;
        g.misc[1] = m.emissionStrength;
        g.misc[2] = m.alphaCutoff;
        g.misc[3] = m.glass ? 1.0f : 0.0f;
    }

    std::vector<GpuLamp> lamps(scene.lamps.size());
    for (std::size_t i = 0; i < scene.lamps.size(); ++i) {
        const pathtrace::Lamp& l = scene.lamps[i];
        GpuLamp& g = lamps[i];
        put3(g.posRange, l.position, l.range);
        put3(g.dirRadius, l.direction, l.radius);
        put3(g.color, l.color);
        g.cone[0] = l.cosInner;
        g.cone[1] = l.cosOuter;
        g.cone[2] = 0.0f;
        g.cone[3] = 0.0f;
    }

    bool ok = true;
    ok = ok && uploadBuffer(m_ssbo[0], 1, tris.data(),  tris.size()  * sizeof(GpuTri));
    ok = ok && uploadBuffer(m_ssbo[1], 2, nodes.data(), nodes.size() * sizeof(GpuNode));
    ok = ok && uploadBuffer(m_ssbo[2], 3, bvh.index.data(),
                            bvh.index.size() * sizeof(int));
    ok = ok && uploadBuffer(m_ssbo[3], 4, mats.data(),  mats.size()  * sizeof(GpuMat));
    ok = ok && uploadBuffer(m_ssbo[4], 5, lamps.data(), lamps.size() * sizeof(GpuLamp));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (!ok) { m_error = "uploading the scene failed"; return false; }

    m_camera       = scene.camera;
    m_sun          = scene.sun;
    m_fog          = scene.fog;
    m_envZenith    = scene.env.zenith;
    m_envHorizon   = scene.env.horizon;
    m_envGround    = scene.env.ground;
    m_envIntensity = scene.env.intensity;
    m_lampCount    = static_cast<int>(scene.lamps.size());
    m_triangles    = static_cast<long long>(scene.triangles.size());
    m_haveScene    = true;
    m_samples      = 0;
    return true;
}

bool Tracer::resize(int width, int height) {
    const int w = std::max(1, width), h = std::max(1, height);
    if (m_accum && w == m_width && h == m_height) {
        // Same picture, new view: keep the texture, drop what is in it.
        const std::vector<float> zero(static_cast<std::size_t>(w) * h * 4, 0.0f);
        glBindTexture(GL_TEXTURE_2D, m_accum);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, zero.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        m_samples = 0;
        return true;
    }
    if (m_accum) glDeleteTextures(1, &m_accum);
    glGenTextures(1, &m_accum);
    glBindTexture(GL_TEXTURE_2D, m_accum);
    // Immutable storage: an image unit binds a level of a texture, and a
    // mutable one can have that level redefined under it.
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, w, h);
    const std::vector<float> zero(static_cast<std::size_t>(w) * h * 4, 0.0f);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, zero.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    m_width   = w;
    m_height  = h;
    m_samples = 0;
    return glGetError() == GL_NO_ERROR;
}

bool Tracer::accumulate(int samples) {
    if (!m_program || !m_haveScene || !m_accum) {
        m_error = "accumulate() before init/upload/resize";
        return false;
    }
    if (samples <= 0) return true;

    glUseProgram(m_program);
    glBindImageTexture(0, m_accum, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);

    glUniform2i(glGetUniformLocation(m_program, "uSize"), m_width, m_height);
    setInt(m_program, "uSampleBase", m_samples);
    setInt(m_program, "uSamples", samples);
    setInt(m_program, "uSeed", 1);
    setInt(m_program, "uLampCount", m_lampCount);

    const float halfV = std::tan(glm::radians(m_camera.fovDegrees) * 0.5f);
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    setVec3(m_program, "uCamPos", m_camera.position);
    setVec3(m_program, "uCamFwd", glm::normalize(m_camera.forward));
    setVec3(m_program, "uCamUp", glm::normalize(m_camera.up));
    setVec3(m_program, "uCamRight", glm::normalize(m_camera.right));
    setFloat(m_program, "uHalfV", halfV);
    setFloat(m_program, "uHalfU", halfV * aspect);

    // The sun, resolved exactly as pathtrace::Tracer's constructor resolves it:
    // `color` is the irradiance the directional light was authored as, and
    // spreading it over the disc is what keeps a render at the same brightness
    // as the viewport whatever angle the disc is given.
    const glm::vec3 sunAxis = glm::normalize(m_sun.direction);
    const float cosMax = std::cos(glm::radians(std::max(0.0f, m_sun.angularRadiusDeg)));
    const float solid  = 2.0f * glm::pi<float>() * (1.0f - cosMax);
    const float sunPdf = solid > 1e-7f ? 1.0f / solid : 0.0f;
    const glm::vec3 sunRadiance = solid > 1e-7f ? m_sun.color / solid : glm::vec3(0.0f);
    setInt(m_program, "uSunOn", m_sun.enabled ? 1 : 0);
    setVec3(m_program, "uSunAxis", sunAxis);
    setVec3(m_program, "uSunDirRaw", m_sun.direction);
    setVec3(m_program, "uSunColor", m_sun.color);
    setVec3(m_program, "uSunRadiance", sunRadiance);
    setFloat(m_program, "uSunCosMax", cosMax);
    setFloat(m_program, "uSunPdf", sunPdf);

    setVec3(m_program, "uEnvZenith", m_envZenith);
    setVec3(m_program, "uEnvHorizon", m_envHorizon);
    setVec3(m_program, "uEnvGround", m_envGround);
    setFloat(m_program, "uEnvIntensity", m_envIntensity);

    setVec3(m_program, "uFogColor", m_fog.color);
    setVec3(m_program, "uFogSunColor", m_fog.sunColor);
    setFloat(m_program, "uFogDensity", m_fog.density);
    setFloat(m_program, "uFogFalloff", m_fog.heightFalloff);
    setFloat(m_program, "uFogHeight", m_fog.height);

    const GLuint gx = static_cast<GLuint>((m_width  + 7) / 8);
    const GLuint gy = static_cast<GLuint>((m_height + 7) / 8);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    glUseProgram(0);

    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        m_error = "the dispatch reported GL error " + std::to_string(err);
        return false;
    }
    m_samples += samples;
    return true;
}

bool Tracer::snapshotHdr(std::vector<float>& out) const {
    if (!m_accum || m_samples <= 0) return false;
    std::vector<float> rgba(static_cast<std::size_t>(m_width) * m_height * 4);
    glBindTexture(GL_TEXTURE_2D, m_accum);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Sums in, radiance out: the kernel only ever adds, so this is the one
    // place the sample count is divided out.
    const float inv = 1.0f / static_cast<float>(m_samples);
    out.resize(static_cast<std::size_t>(m_width) * m_height * 3);
    for (std::size_t i = 0, n = static_cast<std::size_t>(m_width) * m_height;
         i < n; ++i) {
        out[i * 3 + 0] = rgba[i * 4 + 0] * inv;
        out[i * 3 + 1] = rgba[i * 4 + 1] * inv;
        out[i * 3 + 2] = rgba[i * 4 + 2] * inv;
    }
    return true;
}

} // namespace gputrace
