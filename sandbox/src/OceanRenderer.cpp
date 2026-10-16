#include "OceanRenderer.hpp"

#include <algorithm>
#include <cmath>

#include <glad/gl.h>

#include <fitzel/graphics/Shader.hpp>

#include "Ocean.hpp"
#include "ShoreMap.hpp"
#include "WaveSim.hpp"

namespace ocean {

namespace {

constexpr int   kSegments = 256;     // around
constexpr float kFirst    = 0.2f;    // radius of the first ring, m
constexpr float kGrowth   = 1.03f;   // each ring this much wider than the last
// To the horizon: from 600 m up the sea's edge is twenty kilometres off. With
// the horizon terrain on, the far pass stops at kOceanReach (main) and its own
// lakes take over; without it this is all the sea there is. The rings past a
// few kilometres are few and cheap -- each is 3% wider than the last.
constexpr float kReach    = 20000.0f;

fitzel::Mesh makeRings() {
    std::vector<fitzel::Vertex> v;
    std::vector<std::uint32_t> idx;
    v.push_back({{0.0f, kFirst * 0.03f, 0.0f}, {0, 1, 0}, {0, 0}});
    std::vector<float> radii;
    for (float r = kFirst; r < kReach; r *= kGrowth) radii.push_back(r);
    radii.push_back(kReach);
    for (float r : radii) {
        // The wider of the two spacings: along the ring and out to the next.
        const float spacing = std::max(r * (kGrowth - 1.0f), 2.0f * 3.14159265f * r / kSegments);
        for (int s = 0; s < kSegments; ++s) {
            const float a = 2.0f * 3.14159265f * static_cast<float>(s) / kSegments;
            v.push_back({{r * std::cos(a), spacing, r * std::sin(a)}, {0, 1, 0}, {0, 0}});
        }
    }
    // The centre fan, then a band of quads between each ring and the next,
    // wound to face up.
    auto ring = [](std::size_t i, int s) {
        return static_cast<std::uint32_t>(1 + i * kSegments + static_cast<std::size_t>((s + kSegments) % kSegments));
    };
    for (int s = 0; s < kSegments; ++s) idx.insert(idx.end(), {0u, ring(0, s + 1), ring(0, s)});
    for (std::size_t i = 0; i + 1 < radii.size(); ++i)
        for (int s = 0; s < kSegments; ++s) {
            const std::uint32_t a = ring(i, s), b = ring(i, s + 1), c = ring(i + 1, s), d = ring(i + 1, s + 1);
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }
    return fitzel::Mesh::create(v, idx);
}

unsigned makeTex(int n, bool repeat, bool mips) {
    unsigned t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, n, n, 0, GL_RGBA, GL_FLOAT, nullptr);
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (mips && (GLAD_GL_ARB_texture_filter_anisotropic || GLAD_GL_EXT_texture_filter_anisotropic)) {
        GLfloat cap = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &cap);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, std::min(cap, 8.0f));
    }
    return t;
}

// RGBA and tightly packed, always: see Texture.cpp on the driver that
// over-reads anything else.
void send(unsigned tex, int n, const glm::vec4* data, bool mips) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, n, n, GL_RGBA, GL_FLOAT, data);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
}

} // namespace

OceanRenderer::OceanRenderer() : m_mesh(makeRings()) {}

OceanRenderer::~OceanRenderer() {
    glDeleteTextures(3, m_disp);
    glDeleteTextures(3, m_slope);
    if (m_wave) glDeleteTextures(1, &m_wave);
    if (m_shore) glDeleteTextures(1, &m_shore);
}

void OceanRenderer::upload(const Ocean& o, const WaveSim& w, const ShoreMap* shore) {
    if (shore && shore->valid() && (shore->generation() != m_shoreGen || shore->cells() != m_shoreN)) {
        if (shore->cells() != m_shoreN) {
            if (m_shore) glDeleteTextures(1, &m_shore);
            m_shoreN = shore->cells();
            m_shore  = makeTex(m_shoreN, false, false);
        }
        send(m_shore, m_shoreN, shore->texels().data(), false);
        m_shoreGen = shore->generation();
    }
    if (o.size() > 0 && (o.generation() != m_oceanGen || o.size() != m_n)) {
        if (o.size() != m_n) {
            glDeleteTextures(3, m_disp);
            glDeleteTextures(3, m_slope);
            m_n = o.size();
            for (int c = 0; c < kCascades; ++c) {
                m_disp[c]  = makeTex(m_n, true, true);
                m_slope[c] = makeTex(m_n, true, true);
            }
        }
        for (int c = 0; c < kCascades; ++c) {
            const Field& f = o.field(c);
            send(m_disp[c], m_n, f.disp.data(), true);
            m_scratch.resize(f.slope.size());
            for (std::size_t i = 0; i < f.slope.size(); ++i)
                m_scratch[i] = glm::vec4(f.slope[i], 0.0f, 0.0f);
            send(m_slope[c], m_n, m_scratch.data(), true);
        }
        m_oceanGen = o.generation();
    }
    if (w.generation() != m_waveGen || w.cells() != m_waveN) {
        if (w.cells() != m_waveN) {
            if (m_wave) glDeleteTextures(1, &m_wave);
            m_waveN = w.cells();
            m_wave  = makeTex(m_waveN, false, false);
        }
        send(m_wave, m_waveN, w.texels().data(), false);
        m_waveGen = w.generation();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OceanRenderer::bind(unsigned prog, int unit, glm::vec3 camera, float level,
                         const Ocean& o, const WaveSim& w, const ShoreMap* shore,
                         const Breakers& br) const {
    fitzel::Shader::use(prog);
    auto loc = [&](const char* n) { return glGetUniformLocation(prog, n); };
    static const char* dispNames[]  = {"uDisp0", "uDisp1", "uDisp2"};
    static const char* slopeNames[] = {"uSlope0", "uSlope1", "uSlope2"};
    for (int c = 0; c < kCascades; ++c) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, m_disp[c]);
        glUniform1i(loc(dispNames[c]), unit++);
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, m_slope[c]);
        glUniform1i(loc(slopeNames[c]), unit++);
    }
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glBindTexture(GL_TEXTURE_2D, m_wave);
    glUniform1i(loc("uWaveTex"), unit++);
    const bool shoreOn = shore && shore->valid() && m_shore != 0 && br.height > 0.0f;
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glBindTexture(GL_TEXTURE_2D, shoreOn ? m_shore : m_wave);
    glUniform1i(loc("uShoreTex"), unit++);
    glUniform1i(loc("uShoreOn"), shoreOn ? 1 : 0);
    if (shoreOn) {
        glUniform2f(loc("uShoreOrigin"), shore->origin().x, shore->origin().y);
        glUniform1f(loc("uShoreExtent"), shore->extent());
    }
    glUniform3f(loc("uBreakers"), br.height, br.length, br.speed);
    glActiveTexture(GL_TEXTURE0);
    glUniform3f(loc("uPatch"), kPatch[0], kPatch[1], kPatch[2]);
    glUniform1f(loc("uOceanN"), static_cast<float>(std::max(m_n, 1)));
    glUniform1f(loc("uWaveHs"), o.significantHeight());
    glUniform3f(loc("uGridCentre"), camera.x, level, camera.z);
    const glm::vec2 wo = w.origin();
    glUniform2f(loc("uWaveOrigin"), wo.x, wo.y);
    glUniform1f(loc("uWaveExtent"), static_cast<float>(w.cells()) * w.cellSize());
}

} // namespace ocean
