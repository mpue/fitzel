#include "VolumetricFog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

#include <glad/gl.h>

#include <fitzel/graphics/CascadedShadowMap.hpp>

namespace {

// The fullscreen vertex stage every post-style pass in this program shares.
constexpr const char* kVert = "assets/shaders/sky.vert";

// --- The baked noise -------------------------------------------------------
// A 64^3 RGBA volume, tileable on every axis, built once at startup.
//
// Why baked and not evaluated in the shader: the sky already marches hash-based
// fBm, and it is the most expensive thing in the frame -- five octaves is forty
// hashes per sample, and this march wants three lookups per step plus a light
// march on top. A texture fetch is one lookup whatever the octave count, so the
// noise gets as complicated as it needs to be for free, and 1 MB of video memory
// buys back the whole cost.
//
// Tileable is not optional: the field is addressed in world metres, so a volume
// eight hundred metres wide wraps the texture many times over and any seam would
// draw a grid across the fog. Every lookup below wraps its cell index by hand for
// exactly that reason.
constexpr int kNoiseRes = 64;

// A cheap deterministic hash of a wrapped cell index. FNV-1a, which is enough
// mixing for noise and does not need a table.
inline std::uint32_t hashCell(int x, int y, int z, std::uint32_t seed) {
    std::uint32_t h = 2166136261u ^ seed;
    const std::uint32_t v[3] = {static_cast<std::uint32_t>(x),
                                static_cast<std::uint32_t>(y),
                                static_cast<std::uint32_t>(z)};
    for (std::uint32_t k : v) {
        h ^= k;
        h *= 16777619u;
        h ^= h >> 13;
    }
    return h;
}

inline float hashUnit(int x, int y, int z, std::uint32_t seed) {
    return static_cast<float>(hashCell(x, y, z, seed) & 0xFFFFFFu) / 16777215.0f;
}

inline int wrap(int i, int n) { return ((i % n) + n) % n; }

// Value noise over a `cells`-per-axis lattice, wrapping at the lattice edge.
// `p` is in [0,1) volume space.
float valueNoise(const glm::vec3& p, int cells, std::uint32_t seed) {
    const glm::vec3 g = p * static_cast<float>(cells);
    const int ix = static_cast<int>(std::floor(g.x));
    const int iy = static_cast<int>(std::floor(g.y));
    const int iz = static_cast<int>(std::floor(g.z));
    const glm::vec3 f = g - glm::vec3(static_cast<float>(ix), static_cast<float>(iy),
                                      static_cast<float>(iz));
    const glm::vec3 u = f * f * (glm::vec3(3.0f) - 2.0f * f); // smoothstep

    auto corner = [&](int dx, int dy, int dz) {
        return hashUnit(wrap(ix + dx, cells), wrap(iy + dy, cells), wrap(iz + dz, cells),
                        seed);
    };
    const float c00 = glm::mix(corner(0, 0, 0), corner(1, 0, 0), u.x);
    const float c10 = glm::mix(corner(0, 1, 0), corner(1, 1, 0), u.x);
    const float c01 = glm::mix(corner(0, 0, 1), corner(1, 0, 1), u.x);
    const float c11 = glm::mix(corner(0, 1, 1), corner(1, 1, 1), u.x);
    return glm::mix(glm::mix(c00, c10, u.y), glm::mix(c01, c11, u.y), u.z);
}

float valueFbm(const glm::vec3& p, int cells, int octaves, std::uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum  += amp * valueNoise(p, cells, seed + static_cast<std::uint32_t>(o) * 977u);
        norm += amp;
        cells *= 2;
        amp  *= 0.5f;
    }
    return sum / std::max(norm, 1e-4f);
}

// Worley (cellular) noise, inverted so a feature point is a peak rather than a
// pit -- that is what gives the fog its billowed, cauliflower edge instead of a
// smooth blob. Feature points are precomputed per lattice so the inner loop is
// twenty-seven distances and no hashing.
struct WorleyLattice {
    int cells = 0;
    std::vector<glm::vec3> points; // per cell, offset within the cell in [0,1)

    WorleyLattice(int c, std::uint32_t seed) : cells(c) {
        points.resize(static_cast<std::size_t>(c) * c * c);
        for (int z = 0; z < c; ++z)
            for (int y = 0; y < c; ++y)
                for (int x = 0; x < c; ++x)
                    points[static_cast<std::size_t>((z * c + y) * c + x)] = {
                        hashUnit(x, y, z, seed),
                        hashUnit(x, y, z, seed + 7919u),
                        hashUnit(x, y, z, seed + 15013u)};
    }

    float at(const glm::vec3& p) const {
        const glm::vec3 g = p * static_cast<float>(cells);
        const int ix = static_cast<int>(std::floor(g.x));
        const int iy = static_cast<int>(std::floor(g.y));
        const int iz = static_cast<int>(std::floor(g.z));
        float best = 4.0f;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const glm::vec3& off =
                        points[static_cast<std::size_t>(
                            (wrap(iz + dz, cells) * cells + wrap(iy + dy, cells)) * cells +
                            wrap(ix + dx, cells))];
                    // The neighbour is placed UNWRAPPED so the distance is the
                    // real one; only the lookup of its offset wrapped.
                    const glm::vec3 q(static_cast<float>(ix + dx) + off.x,
                                      static_cast<float>(iy + dy) + off.y,
                                      static_cast<float>(iz + dz) + off.z);
                    best = std::min(best, glm::dot(g - q, g - q));
                }
        return 1.0f - std::min(std::sqrt(best), 1.0f);
    }
};

inline unsigned char toByte(float v) {
    return static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Bake the volume. R is the shape band the march reads; G and B are two more
// value-fBm bands at other frequencies and seeds, read together as the vector
// field that warps the lookup (three bands of the same KIND, so the warp has no
// direction it prefers -- mixing a worley band in here biases the swirl); A is
// the worley band that erodes the shape into wisps.
std::uint32_t bakeNoise() {
    const int N = kNoiseRes;
    const std::size_t texels = static_cast<std::size_t>(N) * N * N;
    std::vector<float> band[4];
    for (auto& v : band) v.resize(texels);

    const WorleyLattice w8(8, 31u), w16(16, 131u);

    for (int z = 0; z < N; ++z) {
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const glm::vec3 p(static_cast<float>(x) / N, static_cast<float>(y) / N,
                                  static_cast<float>(z) / N);
                const std::size_t i = static_cast<std::size_t>((z * N + y)) * N + x;
                band[0][i] = valueFbm(p, 4, 4, 1u);
                band[1][i] = valueFbm(p, 6, 3, 5501u);
                band[2][i] = valueFbm(p, 8, 3, 9203u);
                band[3][i] = 0.65f * w8.at(p) + 0.35f * w16.at(p);
            }
        }
    }

    // Stretch every band across the full range before it is quantised.
    //
    // This is not cosmetic. Summed octaves of uniform noise pile up around the
    // middle -- four of them span maybe 0.25..0.75 in practice, never 0..1 --
    // and every knob in the shader is a THRESHOLD against that value. Left
    // unstretched, "coverage" spends its whole travel inside the half of the
    // slider where the noise actually lives and does almost nothing on either
    // side of it; the shape band also loses half of its already scarce eight
    // bits to values that never occur. Stretching here is what makes the
    // authored numbers mean the same thing they read as.
    std::vector<unsigned char> data(texels * 4);
    for (int c = 0; c < 4; ++c) {
        float lo = 1.0f, hi = 0.0f;
        for (float v : band[c]) { lo = std::min(lo, v); hi = std::max(hi, v); }
        const float inv = 1.0f / std::max(hi - lo, 1e-4f);
        for (std::size_t i = 0; i < texels; ++i)
            data[i * 4 + static_cast<std::size_t>(c)] = toByte((band[c][i] - lo) * inv);
    }

    std::uint32_t tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, tex);
    // Force the unpack state rather than trusting it: a dependency that leaves
    // GL_UNPACK_ROW_LENGTH set reads this tightly-packed buffer with the wrong
    // stride and over-reads past its end, which is a crash inside the driver and
    // not a wrong picture. (The engine's Texture does the same, for the same
    // reason -- see resetPixelStore there.)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, N, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 data.data());
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // REPEAT on all three axes is what makes the tiling above worth anything.
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    // No mips: the march samples at wildly varying rates and a mip chain would
    // dissolve the far half of the volume into flat grey.
    glBindTexture(GL_TEXTURE_3D, 0);
    return tex;
}

// Uniform name with an index, for the cascade arrays.
struct Indexed {
    char buf[32];
    Indexed(const char* base, int i) { std::snprintf(buf, sizeof(buf), "%s[%d]", base, i); }
    operator std::string_view() const { return buf; } // NOLINT
};

} // namespace

VolumetricFog::~VolumetricFog() {
    if (m_noiseTex) glDeleteTextures(1, &m_noiseTex);
}

bool VolumetricFog::init() {
    m_march    = fitzel::Shader::fromFiles(kVert, "assets/shaders/volfog.frag");
    m_upsample = fitzel::Shader::fromFiles(kVert, "assets/shaders/volfogup.frag");
    if (!m_march.isValid() || !m_upsample.isValid()) {
        std::fprintf(stderr, "Failed to load volumetric fog shaders\n");
        return false;
    }
    m_noiseTex = bakeNoise();
    m_ok = m_noiseTex != 0;
    return m_ok;
}

void VolumetricFog::resize(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == m_w && h == m_h) return;
    m_w = w;
    m_h = h;
    // 16F because the march accumulates HDR radiance: the in-scatter around the
    // sun goes well past 1 and is meant to bloom.
    m_fogRT = fitzel::RenderTarget(w, h, fitzel::RenderTarget::Format::RGBA16F);
}

void VolumetricFog::worldBox(const Settings& s, const glm::vec3& camPos, glm::vec3& lo,
                             glm::vec3& hi) {
    glm::vec3 c = s.center;
    if (s.followCamera) {
        c.x = camPos.x;
        c.z = camPos.z;
    }
    const glm::vec3 half = glm::max(s.size, glm::vec3(0.1f)) * 0.5f;
    lo = c - half;
    hi = c + half;
}

void VolumetricFog::render(const fitzel::RenderTarget& hdr, const Settings& s,
                           const Params& p, fitzel::Mesh& fsQuad,
                           const fitzel::CascadedShadowMap* shadows) {
    if (!m_ok || !s.enabled || s.density <= 1e-5f) return;

    const int scale = std::clamp(s.resScale, 1, 4);
    resize(hdr.width() / scale, hdr.height() / scale);

    glm::vec3 lo, hi;
    worldBox(s, p.camPos, lo, hi);

    // --- The march, at a fraction of the pane -------------------------------
    m_fogRT.bind();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);

    m_march.bind();
    hdr.bindDepthTexture(0);
    m_march.setInt("uDepth", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, m_noiseTex);
    m_march.setInt("uNoise", 1);

    const bool shafts = s.shafts && shadows != nullptr;
    if (shafts) {
        shadows->bindTextureArray(2);
        const int n = std::min(shadows->cascadeCount(), 4);
        for (int i = 0; i < n; ++i) {
            m_march.setMat4(Indexed("uLightSpace", i), shadows->lightMatrices()[i]);
            m_march.setFloat(Indexed("uCascadeSplits", i), shadows->splitDistances()[i]);
        }
        m_march.setInt("uCascadeCount", n);
    } else {
        // The sampler still needs a bound texture of the right type; the cascade
        // count is what actually switches the lookup off.
        if (shadows) shadows->bindTextureArray(2);
        m_march.setInt("uCascadeCount", 0);
    }
    m_march.setInt("uShadowMap", 2);
    m_march.setInt("uShafts", shafts ? 1 : 0);

    m_march.setMat4("uInvViewProj", glm::inverse(p.viewProj));
    m_march.setVec3("uCamPos", p.camPos);
    m_march.setVec3("uCamFwd", p.camFwd);
    m_march.setFloat("uTime", p.time);

    m_march.setVec3("uBoxMin", lo);
    m_march.setVec3("uBoxMax", hi);
    m_march.setFloat("uEdge", std::clamp(s.edge, 0.01f, 1.0f));
    m_march.setFloat("uHeightFalloff", std::max(s.heightFalloff, 0.0f));

    m_march.setFloat("uDensity", s.density);
    m_march.setVec3("uColor", s.color);
    m_march.setFloat("uCoverage", std::clamp(s.coverage, 0.0f, 0.95f));

    m_march.setFloat("uNoiseScale", std::max(s.noiseScale, 1e-5f));
    m_march.setFloat("uDetail", std::clamp(s.detail, 0.0f, 0.95f));
    m_march.setFloat("uWarp", std::max(s.warp, 0.0f));
    m_march.setVec3("uWind", s.wind);

    m_march.setVec3("uSunDir", p.sunDir);
    m_march.setVec3("uSunColor", p.sunColor);
    m_march.setVec3("uAmbient", p.ambient);
    m_march.setFloat("uG", std::clamp(s.anisotropy, -0.9f, 0.9f));
    m_march.setFloat("uSunIntensity", s.sunIntensity);
    m_march.setFloat("uAmbientIntensity", s.ambientIntensity);
    m_march.setInt("uSelfShadow", s.selfShadow ? 1 : 0);
    // The light march has to cross a meaningful part of a bank to mean anything,
    // and what "meaningful" is scales with the volume: an eighth of its height,
    // so a shallow ground mist and a tall wall of fog both get three useful taps.
    m_march.setFloat("uLightStep", std::max((hi.y - lo.y) * 0.125f, 0.5f));
    m_march.setInt("uSteps", s.steps);
    fsQuad.draw();

    // --- Back onto the scene ------------------------------------------------
    // src + dst*srcAlpha: the in-scatter is added, what is behind is attenuated
    // by the transmittance the march carried out. Alpha of the target is left
    // alone -- nothing downstream reads it, and writing it here would only make
    // that a thing to remember.
    hdr.bind();
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
    m_upsample.bind();
    m_fogRT.bindColorTexture(0);
    m_upsample.setInt("uFog", 0);
    m_upsample.setVec2("uTexel", {1.0f / static_cast<float>(m_w),
                                  1.0f / static_cast<float>(m_h)});
    fsQuad.draw();

    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // the state everything else assumes
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}
