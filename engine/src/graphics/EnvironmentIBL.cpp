#include "fitzel/graphics/EnvironmentIBL.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <stb_image.h>
#include <tinyexr.h>

namespace fitzel {

namespace {

// A cube capture rig: 90-degree projection and the six face views from the origin.
const glm::mat4 kProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
const glm::mat4 kViews[6] = {
    glm::lookAt(glm::vec3(0), glm::vec3( 1, 0, 0), glm::vec3(0, -1, 0)),
    glm::lookAt(glm::vec3(0), glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)),
    glm::lookAt(glm::vec3(0), glm::vec3( 0, 1, 0), glm::vec3(0, 0,  1)),
    glm::lookAt(glm::vec3(0), glm::vec3( 0,-1, 0), glm::vec3(0, 0, -1)),
    glm::lookAt(glm::vec3(0), glm::vec3( 0, 0, 1), glm::vec3(0, -1, 0)),
    glm::lookAt(glm::vec3(0), glm::vec3( 0, 0,-1), glm::vec3(0, -1, 0)),
};

constexpr const char* kCubeVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
out vec3 vLocal;
uniform mat4 uProj;
uniform mat4 uView;
void main() { vLocal = aPos; gl_Position = uProj * uView * vec4(aPos, 1.0); }
)";

constexpr const char* kToCubeFrag = R"(#version 330 core
in vec3 vLocal;
out vec4 FragColor;
uniform sampler2D uEquirect;
const vec2 invAtan = vec2(0.1591, 0.3183);
vec2 sampleSpherical(vec3 v) {
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan; uv += 0.5; return uv;
}
void main() {
    vec3 c = texture(uEquirect, sampleSpherical(normalize(vLocal))).rgb;
    FragColor = vec4(c, 1.0);
}
)";

constexpr const char* kIrradianceFrag = R"(#version 330 core
in vec3 vLocal;
out vec4 FragColor;
uniform samplerCube uEnv;
const float PI = 3.14159265359;
void main() {
    vec3 N = normalize(vLocal);
    vec3 up    = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));
    vec3 irr = vec3(0.0);
    float n = 0.0;
    const float d = 0.025;
    for (float phi = 0.0; phi < 2.0 * PI; phi += d)
        for (float theta = 0.0; theta < 0.5 * PI; theta += d) {
            vec3 t = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 s = t.x * right + t.y * up + t.z * N;
            irr += texture(uEnv, s).rgb * cos(theta) * sin(theta);
            n += 1.0;
        }
    FragColor = vec4(PI * irr / n, 1.0);
}
)";

constexpr const char* kPrefilterFrag = R"(#version 330 core
in vec3 vLocal;
out vec4 FragColor;
uniform samplerCube uEnv;
uniform float uRoughness;
const float PI = 3.14159265359;
float radicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), radicalInverse(i)); }
vec3 importanceGGX(vec2 xi, vec3 N, float rough) {
    float a = rough * rough;
    float phi = 2.0 * PI * xi.x;
    float ct = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float st = sqrt(1.0 - ct * ct);
    vec3 H = vec3(cos(phi) * st, sin(phi) * st, ct);
    vec3 up = abs(N.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tx = normalize(cross(up, N));
    vec3 ty = cross(N, tx);
    return normalize(tx * H.x + ty * H.y + N * H.z);
}
void main() {
    vec3 N = normalize(vLocal);
    vec3 V = N;
    const uint SAMPLES = 1024u;
    vec3 col = vec3(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < SAMPLES; ++i) {
        vec2 xi = hammersley(i, SAMPLES);
        vec3 H  = importanceGGX(xi, N, uRoughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);
        float ndl = max(dot(N, L), 0.0);
        if (ndl > 0.0) { col += texture(uEnv, L).rgb * ndl; wsum += ndl; }
    }
    FragColor = vec4(col / max(wsum, 0.001), 1.0);
}
)";

// Load an equirectangular HDR panorama as tightly-packed RGB floats (caller
// frees with std::free). Supports Radiance .hdr (stb) and OpenEXR .exr (tinyexr).
float* loadEquirect(const std::string& path, int& w, int& h) {
    const auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    for (char& c : ext) c = static_cast<char>(::tolower(c));

    if (ext == ".exr") {
        float* rgba = nullptr;
        const char* err = nullptr;
        if (LoadEXR(&rgba, &w, &h, path.c_str(), &err) != TINYEXR_SUCCESS) {
            std::fprintf(stderr, "[Fitzel] EXR load failed '%s': %s\n",
                         path.c_str(), err ? err : "?");
            if (err) FreeEXRErrorMessage(err);
            return nullptr;
        }
        auto* rgb = static_cast<float*>(std::malloc(sizeof(float) * 3 * w * h));
        for (int y = 0; y < h; ++y)          // flip vertically (EXR is top-down)
            for (int x = 0; x < w; ++x) {
                const int s = ((h - 1 - y) * w + x) * 4;
                const int d = (y * w + x) * 3;
                rgb[d + 0] = rgba[s + 0];
                rgb[d + 1] = rgba[s + 1];
                rgb[d + 2] = rgba[s + 2];
            }
        std::free(rgba);
        return rgb;
    }

    stbi_set_flip_vertically_on_load(1);     // .hdr panoramas are bottom-up for us
    int n = 0;
    float* d = stbi_loadf(path.c_str(), &w, &h, &n, 3);
    stbi_set_flip_vertically_on_load(0);
    if (!d) std::fprintf(stderr, "[Fitzel] HDR load failed '%s'\n", path.c_str());
    return d;
}

std::uint32_t makeCube(int size, bool mips) {
    std::uint32_t tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    for (int f = 0; f < 6; ++f)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGB16F,
                     size, size, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                    mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (mips) glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    return tex;
}

} // namespace

EnvironmentIBL::EnvironmentIBL()
    : m_toCube(Shader::fromSource(kCubeVert, kToCubeFrag)),
      m_irradiance(Shader::fromSource(kCubeVert, kIrradianceFrag)),
      m_prefilterSh(Shader::fromSource(kCubeVert, kPrefilterFrag)) {
    buildCubeVao();
    glGenFramebuffers(1, &m_captureFbo);
    glGenRenderbuffers(1, &m_captureRbo);
}

EnvironmentIBL::~EnvironmentIBL() {
    deleteMaps();
    if (m_captureRbo) glDeleteRenderbuffers(1, &m_captureRbo);
    if (m_captureFbo) glDeleteFramebuffers(1, &m_captureFbo);
    if (m_cubeVbo)    glDeleteBuffers(1, &m_cubeVbo);
    if (m_cubeVao)    glDeleteVertexArrays(1, &m_cubeVao);
}

void EnvironmentIBL::deleteMaps() {
    if (m_prefilter) { glDeleteTextures(1, &m_prefilter); m_prefilter = 0; }
    if (m_irradMap)  { glDeleteTextures(1, &m_irradMap);  m_irradMap = 0; }
    if (m_envCube)   { glDeleteTextures(1, &m_envCube);   m_envCube = 0; }
    m_valid = false;
}

void EnvironmentIBL::buildCubeVao() {
    const float v[] = {
        -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1, 1, -1, 1,-1, -1,-1,-1, // -X
         1,-1,-1,  1, 1,-1,  1, 1, 1,  1, 1, 1,  1,-1, 1,  1,-1,-1, // +X
        -1,-1,-1,  1,-1,-1,  1,-1, 1,  1,-1, 1, -1,-1, 1, -1,-1,-1, // -Y
        -1, 1,-1, -1, 1, 1,  1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1, // +Y
        -1,-1,-1, -1, 1,-1,  1, 1,-1,  1, 1,-1,  1,-1,-1, -1,-1,-1, // -Z
        -1,-1, 1,  1,-1, 1,  1, 1, 1,  1, 1, 1, -1, 1, 1, -1,-1, 1, // +Z
    };
    glGenVertexArrays(1, &m_cubeVao);
    glGenBuffers(1, &m_cubeVbo);
    glBindVertexArray(m_cubeVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_cubeVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
}

void EnvironmentIBL::renderCube() const {
    glBindVertexArray(m_cubeVao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
}

bool EnvironmentIBL::load(const std::string& path) {
    int w = 0, h = 0;
    float* pixels = loadEquirect(path, w, h);
    if (!pixels) return false;

    // Upload the panorama to a float 2D texture.
    std::uint32_t hdr;
    glGenTextures(1, &hdr);
    glBindTexture(GL_TEXTURE_2D, hdr);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, w, h, 0, GL_RGB, GL_FLOAT, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    std::free(pixels);

    deleteMaps();
    const int   kEnv = 512, kIrr = 32, kPre = 128;
    m_envCube   = makeCube(kEnv, true);
    m_irradMap  = makeCube(kIrr, false);
    m_prefilter = makeCube(kPre, true);

    // Save GL state we touch, so we don't disturb the caller's frame.
    GLint prevVp[4];      glGetIntegerv(GL_VIEWPORT, prevVp);
    const GLboolean depthWas = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cullWas  = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_captureFbo);

    auto renderFaces = [&](const Shader& sh, std::uint32_t cube, int size, int mip) {
        glBindRenderbuffer(GL_RENDERBUFFER, m_captureRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, m_captureRbo);
        glViewport(0, 0, size, size);
        for (int f = 0; f < 6; ++f) {
            sh.setMat4("uView", kViews[f]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, cube, mip);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderCube();
        }
    };

    // 1) Equirect panorama -> radiance cubemap, then build its mip chain.
    m_toCube.bind();
    m_toCube.setInt("uEquirect", 0);
    m_toCube.setMat4("uProj", kProj);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdr);
    renderFaces(m_toCube, m_envCube, kEnv, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCube);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

    // 2) Diffuse irradiance convolution.
    m_irradiance.bind();
    m_irradiance.setInt("uEnv", 0);
    m_irradiance.setMat4("uProj", kProj);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCube);
    renderFaces(m_irradiance, m_irradMap, kIrr, 0);

    // 3) Specular prefilter: one roughness per mip level.
    m_prefilterSh.bind();
    m_prefilterSh.setInt("uEnv", 0);
    m_prefilterSh.setMat4("uProj", kProj);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCube);
    for (int mip = 0; mip < m_prefilterMips; ++mip) {
        const int mipSize = kPre >> mip;
        const float rough = static_cast<float>(mip) /
                            static_cast<float>(m_prefilterMips - 1);
        m_prefilterSh.setFloat("uRoughness", rough);
        renderFaces(m_prefilterSh, m_prefilter, mipSize, mip);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    if (depthWas) glEnable(GL_DEPTH_TEST);
    if (cullWas)  glEnable(GL_CULL_FACE);
    glDeleteTextures(1, &hdr);

    m_valid = true;
    std::printf("[Fitzel] IBL environment loaded: %s (%dx%d)\n", path.c_str(), w, h);
    return true;
}

void EnvironmentIBL::bindIrradiance(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_irradMap);
}
void EnvironmentIBL::bindPrefilter(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_prefilter);
}
void EnvironmentIBL::bindEnvCube(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCube);
}

} // namespace fitzel
