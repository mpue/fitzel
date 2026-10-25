#include "OceanRenderer.hpp"

#include <algorithm>
#include <cmath>

#include <fitzel/graphics/Shader.hpp>
#include <fitzel/rhi/Device.hpp>

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

fitzel::rhi::TextureHandle makeTex(int n, bool repeat, bool mips) {
    using namespace fitzel::rhi;
    TextureDesc d;
    d.format    = Format::RGBA16F;
    d.width     = n;
    d.height    = n;
    d.mipLevels = mips ? 0 : 1;
    const Wrap wrap = repeat ? Wrap::Repeat : Wrap::ClampToEdge;
    d.sampler.wrapU = d.sampler.wrapV = d.sampler.wrapW = wrap;
    d.sampler.minFilter = d.sampler.magFilter = Filter::Linear;
    d.sampler.mipFilter = mips ? MipFilter::Linear : MipFilter::None;
    // Anisotropic where the device has it (clamped to its maximum there).
    if (mips) d.sampler.anisotropy = 8.0f;
    return device().createTexture(d);
}

// RGBA and tightly packed, always: see Texture.cpp on the driver that
// over-reads anything else.
void send(fitzel::rhi::TextureHandle tex, const glm::vec4* data, bool mips) {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    gpu.updateTexture(tex, fitzel::rhi::TextureRegion{}, fitzel::rhi::Format::RGBA32F, data);
    if (mips) gpu.generateMipmaps(tex);
}

} // namespace

OceanRenderer::OceanRenderer() : m_mesh(makeRings()) {}

OceanRenderer::~OceanRenderer() {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    for (int c = 0; c < 3; ++c) {
        gpu.destroy(m_disp[c]);
        gpu.destroy(m_slope[c]);
    }
    gpu.destroy(m_wave);
    gpu.destroy(m_shore);
}

void OceanRenderer::upload(const Ocean& o, const WaveSim& w, const ShoreMap* shore) {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    if (shore && shore->valid() && (shore->generation() != m_shoreGen || shore->cells() != m_shoreN)) {
        if (shore->cells() != m_shoreN) {
            gpu.destroy(m_shore);
            m_shoreN = shore->cells();
            m_shore  = makeTex(m_shoreN, false, false);
        }
        send(m_shore, shore->texels().data(), false);
        m_shoreGen = shore->generation();
    }
    if (o.size() > 0 && (o.generation() != m_oceanGen || o.size() != m_n)) {
        if (o.size() != m_n) {
            for (int c = 0; c < 3; ++c) {
                gpu.destroy(m_disp[c]);
                gpu.destroy(m_slope[c]);
            }
            m_n = o.size();
            for (int c = 0; c < kCascades; ++c) {
                m_disp[c]  = makeTex(m_n, true, true);
                m_slope[c] = makeTex(m_n, true, true);
            }
        }
        for (int c = 0; c < kCascades; ++c) {
            const Field& f = o.field(c);
            send(m_disp[c], f.disp.data(), true);
            m_scratch.resize(f.slope.size());
            for (std::size_t i = 0; i < f.slope.size(); ++i)
                m_scratch[i] = glm::vec4(f.slope[i], 0.0f, 0.0f);
            send(m_slope[c], m_scratch.data(), true);
        }
        m_oceanGen = o.generation();
    }
    if (w.generation() != m_waveGen || w.cells() != m_waveN) {
        if (w.cells() != m_waveN) {
            gpu.destroy(m_wave);
            m_waveN = w.cells();
            m_wave  = makeTex(m_waveN, false, false);
        }
        send(m_wave, w.texels().data(), false);
        m_waveGen = w.generation();
    }
}

void OceanRenderer::bind(const fitzel::Shader& sh, int unit, glm::vec3 camera, float level,
                         const Ocean& o, const WaveSim& w, const ShoreMap* shore,
                         const Breakers& br) const {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    sh.bind();
    static const char* dispNames[]  = {"uDisp0", "uDisp1", "uDisp2"};
    static const char* slopeNames[] = {"uSlope0", "uSlope1", "uSlope2"};
    for (int c = 0; c < kCascades; ++c) {
        gpu.bindTexture(unit, m_disp[c]);
        sh.setInt(dispNames[c], unit++);
        gpu.bindTexture(unit, m_slope[c]);
        sh.setInt(slopeNames[c], unit++);
    }
    gpu.bindTexture(unit, m_wave);
    sh.setInt("uWaveTex", unit++);
    const bool shoreOn = shore && shore->valid() && m_shore && br.height > 0.0f;
    gpu.bindTexture(unit, shoreOn ? m_shore : m_wave);
    sh.setInt("uShoreTex", unit++);
    sh.setInt("uShoreOn", shoreOn ? 1 : 0);
    if (shoreOn) {
        sh.setVec2("uShoreOrigin", shore->origin());
        sh.setFloat("uShoreExtent", shore->extent());
    }
    sh.setVec3("uBreakers", glm::vec3(br.height, br.length, br.speed));
    sh.setVec3("uPatch", glm::vec3(kPatch[0], kPatch[1], kPatch[2]));
    sh.setFloat("uOceanN", static_cast<float>(std::max(m_n, 1)));
    sh.setFloat("uWaveHs", o.significantHeight());
    sh.setVec3("uGridCentre", glm::vec3(camera.x, level, camera.z));
    sh.setVec2("uWaveOrigin", w.origin());
    sh.setFloat("uWaveExtent", static_cast<float>(w.cells()) * w.cellSize());
}

} // namespace ocean
