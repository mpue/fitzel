// VegetationSystem's impostors: baking a species into a card atlas and drawing
// the forest field with it. Split out of VegetationSystem.cpp only for size.

#include "VegetationSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <fitzel/rhi/Device.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rhi = fitzel::rhi;

namespace {

constexpr int kViews   = 4;     // baked directions, one atlas row
constexpr int kViewH   = 256;   // pixels per view, vertically

// Fill the colour of transparent texels from their opaque neighbours, a ring at
// a time. The atlas is sampled bilinearly and mipped: without this every leaf
// edge averages in the black of the empty background and the forest wears a
// dark outline that thickens with distance.
void dilate(std::vector<unsigned char>& px, int w, int h, int passes) {
    std::vector<unsigned char> src;
    for (int p = 0; p < passes; ++p) {
        src = px;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                unsigned char* o = &px[(static_cast<std::size_t>(y) * w + x) * 4];
                if (src[(static_cast<std::size_t>(y) * w + x) * 4 + 3] != 0) continue;
                int r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        const unsigned char* s =
                            &src[(static_cast<std::size_t>(yy) * w + xx) * 4];
                        if (s[3] == 0) continue;
                        r += s[0]; g += s[1]; b += s[2]; ++n;
                    }
                if (n == 0) continue;
                o[0] = static_cast<unsigned char>(r / n);
                o[1] = static_cast<unsigned char>(g / n);
                o[2] = static_cast<unsigned char>(b / n);
                o[3] = 1;   // filled: counts as a source for the next ring
            }
    }
    // The fill marker was only for the passes above.
    for (std::size_t i = 3; i < px.size(); i += 4) if (px[i] == 1) px[i] = 0;
}

// Upload with a hand-built mip chain whose alpha keeps the silhouette's
// coverage. Plain averaging thins an alpha-tested canopy at every level --
// half-covered texels fall under the cutoff -- until a distant forest is a
// scatter of twigs. Each level's alpha is scaled until as many texels pass the
// test as passed at the top (Castaño, "Computing alpha mipmaps").
// `tex` is made anew (the atlas changes size with the crown's aspect).
void uploadMipped(rhi::TextureHandle& tex, std::vector<unsigned char> level, int w, int h,
                  bool keepCoverage) {
    rhi::Device& gpu = rhi::device();
    if (tex) gpu.destroy(tex);
    rhi::TextureDesc d;
    d.format    = rhi::Format::RGBA8;
    d.width     = w;
    d.height    = h;
    d.mipLevels = 0;   // the full chain, down to 1x1
    tex = gpu.createTexture(d);   // every level exists, to be written below
    const auto coverage = [](const std::vector<unsigned char>& p, float s) {
        std::size_t in = 0, n = p.size() / 4;
        for (std::size_t i = 3; i < p.size(); i += 4)
            if (p[i] * s >= 127.5f) ++in;
        return n ? static_cast<float>(in) / static_cast<float>(n) : 0.0f;
    };
    const float target = coverage(level, 1.0f);
    int mip = 0;
    for (;;) {
        gpu.updateTexture(tex, rhi::TextureRegion{mip}, rhi::Format::RGBA8, level.data());
        if (w == 1 && h == 1) break;
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<unsigned char> next(static_cast<std::size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    int sum = 0, n = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int xx = std::min(w - 1, x * 2 + dx);
                            const int yy = std::min(h - 1, y * 2 + dy);
                            sum += level[(static_cast<std::size_t>(yy) * w + xx) * 4 + c];
                            ++n;
                        }
                    next[(static_cast<std::size_t>(y) * nw + x) * 4 + c] =
                        static_cast<unsigned char>(sum / n);
                }
        if (keepCoverage && target > 0.0f) {
            float lo = 0.5f, hi = 8.0f;
            for (int it = 0; it < 14; ++it) {
                const float mid = 0.5f * (lo + hi);
                (coverage(next, mid) < target ? lo : hi) = mid;
            }
            for (std::size_t i = 3; i < next.size(); i += 4)
                next[i] = static_cast<unsigned char>(std::min(255.0f, next[i] * hi));
        }
        level.swap(next);
        w = nw; h = nh; ++mip;
    }
    rhi::SamplerDesc smp;   // trilinear, clamped
    smp.mipFilter = rhi::MipFilter::Linear;
    smp.wrapW     = rhi::Wrap::Repeat;
    smp.maxLevel  = mip;
    gpu.setSampler(tex, smp);
}

} // namespace

void VegetationSystem::prepareTrees() {
    for (TreeSpecies& sp : m_species) {
        if (sp.lods.empty()) continue;
        if (sp.autoDirty) buildAutoLods(sp);
        if (sp.impDirty && eco.enabled) bakeImpostor(sp);
    }
}

glm::vec3 VegetationSystem::canopyColour() const {
    glm::vec3 sum(0.0f);
    float w = 0.0f;
    for (const TreeSpecies& sp : m_species) {
        if (!sp.enabled || sp.lods.empty() || !sp.impAlbedo) continue;
        const float ws = std::max(sp.density, 0.0f) * sp.size;   // big trees dominate a canopy
        sum += sp.canopy * ws;
        w   += ws;
    }
    return w > 0.0f ? sum / w : glm::vec3(0.02f, 0.035f, 0.012f);
}

void VegetationSystem::bakeImpostor(TreeSpecies& sp) {
    sp.impDirty = false;
    if (!m_impostorBake.isValid() || sp.lods.empty()) return;
    const TreeLOD& lod = sp.lods.front();          // the finest the author gave
    if (lod.cpuVerts.empty() || !lod.vbo) return;

    // The crown's reach around the trunk (the mesh is unit height, base at 0).
    float maxR = 0.05f;
    for (std::size_t i = 0; i + 7 < lod.cpuVerts.size(); i += 8)
        maxR = std::max(maxR, std::sqrt(lod.cpuVerts[i] * lod.cpuVerts[i] +
                                        lod.cpuVerts[i + 2] * lod.cpuVerts[i + 2]));
    maxR *= 1.03f;
    const float aspect = glm::clamp(2.0f * maxR / 1.04f, 0.2f, 2.5f);
    const int   vw = std::max(8, static_cast<int>(std::round(kViewH * aspect)));
    // The side views in a row, then one square cell for the view from above.
    const int   W  = vw * kViews + kViewH, H = kViewH;

    // --- Device state we are about to disturb.
    rhi::Device& gpu = rhi::device();
    const rhi::FramebufferHandle prevFbo   = gpu.boundFramebuffer();
    const rhi::Rect              prevVp    = gpu.viewport();
    const rhi::RenderState       prevState = gpu.renderState();
    const glm::vec4              prevClear = gpu.clearColor();

    // Colour into attachment 0, the normal and leaf flag into 1.
    rhi::TextureDesc cd;
    cd.format  = rhi::Format::RGBA8;
    cd.width   = W;
    cd.height  = H;
    cd.sampler = rhi::SamplerDesc::nearestClamp();
    cd.sampler.wrapU = cd.sampler.wrapV = cd.sampler.wrapW = rhi::Wrap::Repeat;
    const rhi::TextureHandle tex[2] = {gpu.createTexture(cd), gpu.createTexture(cd)};
    rhi::TextureDesc dd;
    dd.format = rhi::Format::Depth24;
    dd.width  = W;
    dd.height = H;
    dd.usage  = rhi::UsageRenderTarget;   // drawn into, never sampled
    const rhi::TextureHandle depth = gpu.createTexture(dd);
    rhi::FramebufferDesc fd;
    fd.color[0] = rhi::Attachment{tex[0]};
    fd.color[1] = rhi::Attachment{tex[1]};
    fd.depth    = rhi::Attachment{depth};
    const rhi::FramebufferHandle fbo = gpu.createFramebuffer(fd);
    gpu.bindFramebuffer(fbo);
    rhi::VertexArrayHandle vao;
    const bool complete = gpu.framebufferComplete(fbo);

    if (complete) {
        gpu.setViewport(0, 0, W, H);
        gpu.setClearColor(glm::vec4(0.0f));
        gpu.clear(rhi::ClearColor | rhi::ClearDepth);
        gpu.setDepthTest(true);
        gpu.setDepthWrite(true);
        gpu.setDepthFunc(rhi::CompareOp::Less);
        gpu.setCullEnabled(false);   // leaf cards are seen from both sides
        gpu.setBlendEnabled(false);

        // The mesh without its instance attributes: a vertex array of its own
        // over the same buffers.
        const std::uint32_t ms = 8 * sizeof(float);
        const rhi::VertexAttribute attrs[] = {
            {0, lod.vbo, rhi::VertexFormat::Float3, 0, ms},
            {1, lod.vbo, rhi::VertexFormat::Float3, 3 * sizeof(float), ms},
            {2, lod.vbo, rhi::VertexFormat::Float2, 6 * sizeof(float), ms},
        };
        rhi::VertexArrayDesc vd;
        vd.attributes     = attrs;
        vd.attributeCount = 3;
        vd.indexBuffer    = lod.ibo;
        vao = gpu.createVertexArray(vd);
        gpu.bindVertexArray(vao);

        const glm::mat4 proj = glm::ortho(-maxR, maxR, -0.02f, 1.02f, -(maxR + 1.0f),
                                          maxR + 1.0f);
        m_impostorBake.bind();
        m_impostorBake.setInt("uTex", 0);
        const auto drawMesh = [&] {
            for (const TreeLOD::Prim& p : lod.prims) {
                p.bindTex();
                m_impostorBake.setInt("uHasTex", p.textured() ? 1 : 0);
                m_impostorBake.setInt("uAlphaCutout", p.cutout ? 1 : 0);
                m_impostorBake.setFloat("uAlphaCutoff", p.cutoff);
                m_impostorBake.setVec3("uBaseColor", p.baseColor);
                m_impostorBake.setVec3("uTint", p.tint);
                gpu.drawIndexed(rhi::Primitive::Triangles, p.count, rhi::IndexType::U32,
                                static_cast<std::size_t>(p.first) * sizeof(std::uint32_t));
            }
        };
        m_impostorBake.setMat4("uProj", proj);
        m_impostorBake.setInt("uTop", 0);
        for (int v = 0; v < kViews; ++v) {
            gpu.setViewport(v * vw, 0, vw, H);
            m_impostorBake.setFloat("uYaw", static_cast<float>(v) * 1.5707963f);
            drawMesh();
        }
        // From above: the crown's reach both ways, the height as depth (the
        // shader lays the tree down so its top faces this camera).
        gpu.setViewport(kViews * vw, 0, H, H);
        m_impostorBake.setMat4("uProj", glm::ortho(-maxR, maxR, -maxR, maxR, -2.0f, 2.0f));
        m_impostorBake.setInt("uTop", 1);
        m_impostorBake.setFloat("uYaw", 0.0f);
        drawMesh();
        gpu.bindVertexArray({});

        // Back to the CPU for the dilation and the coverage-keeping mips.
        std::vector<unsigned char> albedo(static_cast<std::size_t>(W) * H * 4);
        std::vector<unsigned char> normal(albedo.size());
        gpu.readPixels(fbo, 0, rhi::Rect{0, 0, W, H}, rhi::Format::RGBA8, albedo.data());
        gpu.readPixels(fbo, 1, rhi::Rect{0, 0, W, H}, rhi::Format::RGBA8, normal.data());

        // The foliage's mean colour, for the far terrain's canopy (the texture
        // is sRGB; the brightness correction the tree shaders apply rides along).
        glm::dvec3 acc(0.0);
        std::size_t leafTexels = 0;
        for (std::size_t i = 0; i < albedo.size(); i += 4) {
            if (albedo[i + 3] < 128 || normal[i + 3] < 200) continue;
            acc += glm::dvec3(std::pow(albedo[i] / 255.0, 2.2), std::pow(albedo[i + 1] / 255.0, 2.2),
                              std::pow(albedo[i + 2] / 255.0, 2.2));
            ++leafTexels;
        }
        if (leafTexels > 0) sp.canopy = glm::vec3(acc / static_cast<double>(leafTexels));

        // The normal map's alpha is the leaf flag (255 foliage, 128 bark), not
        // coverage. The dilation needs coverage to know what to fill, so the
        // flags step aside for it and come back after; the filled rim counts
        // as foliage, which is what a crown's edge is.
        std::vector<unsigned char> leafFlag(static_cast<std::size_t>(W) * H);
        for (std::size_t i = 0; i < leafFlag.size(); ++i) {
            leafFlag[i] = normal[i * 4 + 3];
            normal[i * 4 + 3] = albedo[i * 4 + 3];
        }
        dilate(albedo, W, H, 6);
        dilate(normal, W, H, 6);
        for (std::size_t i = 0; i < leafFlag.size(); ++i)
            normal[i * 4 + 3] = (leafFlag[i] > 0) ? leafFlag[i] : 255;

        uploadMipped(sp.impAlbedo, albedo, W, H, true);
        uploadMipped(sp.impNormal, normal, W, H, false);
        sp.impAspect   = 2.0f * maxR / 1.04f;
        sp.impSideFrac = static_cast<float>(vw * kViews) / static_cast<float>(W);
        std::fprintf(stderr, "impostor %s: %dx%d, aspect %.2f\n", sp.name.c_str(), W, H,
                     sp.impAspect);
    }

    // --- Put everything back.
    gpu.bindFramebuffer(prevFbo);
    gpu.setViewport(prevVp);
    gpu.setRenderState(prevState);
    gpu.setClearColor(prevClear);
    if (vao) gpu.destroy(vao);
    gpu.destroy(fbo);
    gpu.destroy(depth);
    gpu.destroy(tex[0]);
    gpu.destroy(tex[1]);
    gpu.bindTexture(0, {});
}

// Past the meshes, the impostors: one card a tree, standing square to the sun,
// so the shadow it throws is the tree's silhouette stretched along the ground
// exactly as the mesh's would be. The mesh shadows only ever cover the near set
// (sp.inst, gathered out to impostorStart + 20 m): the cards take over at `from`.
// Inside the cascade pass, which put the state back afterwards.
void VegetationSystem::drawImpostorShadows(const glm::mat4& lightSpace, glm::vec2 camXZ,
                                           float from) {
    // The sun's heading, out of the cascade's own matrix: an orthographic
    // projection maps its depth axis back to the direction the light runs.
    const glm::vec3 run = glm::vec3(glm::inverse(lightSpace) * glm::vec4(0, 0, 1, 0));
    glm::vec2 h(run.x, run.z);
    h = (glm::length(h) > 1e-4f) ? glm::normalize(h) : glm::vec2(1.0f, 0.0f);
    rhi::Device& gpu = rhi::device();
    gpu.setCullEnabled(false);   // a card is seen from either side
    m_impostorShadow.bind();
    m_impostorShadow.setMat4("uLightSpace", lightSpace);
    m_impostorShadow.setVec3("uEye", glm::vec3(camXZ.x, 0.0f, camXZ.y));
    m_impostorShadow.setFloat("uFrom", from);
    m_impostorShadow.setFloat("uTo", impostorShadowDist);
    m_impostorShadow.setVec3("uAcross", glm::vec3(-h.y, 0.0f, h.x));
    m_impostorShadow.setFloat("uViews", static_cast<float>(kViews));
    m_impostorShadow.setInt("uAlbedo", 0);
    wind::apply(m_impostorShadow, wind, 1.0f);
    for (const TreeSpecies& sp : m_species) {
        if (!sp.enabled || sp.farCount == 0 || !sp.impAlbedo) continue;
        m_impostorShadow.setFloat("uAspect", sp.impAspect);
        m_impostorShadow.setFloat("uSideFrac", sp.impSideFrac);
        gpu.bindTexture(0, sp.impAlbedo);
        gpu.bindVertexArray(sp.farVAO);
        gpu.draw(rhi::Primitive::TriangleStrip, 0, 4, sp.farCount);
        m_shadowInst += sp.farCount;
    }
}

void VegetationSystem::drawImpostors(const FrameContext& c) {
    if (!terrainPresent || !treeEnabled || !m_impostor.isValid()) return;
    rhi::Device& gpu = rhi::device();
    gpu.setCullEnabled(false);
    m_impostor.bind();
    m_impostor.setMat4("uViewProj", c.viewProj);
    m_impostor.setVec3("uCamPos", c.camPos);
    m_impostor.setVec3("uViewPos", c.camPos);
    m_impostor.setVec3("uLightDir", c.lightDir);
    m_impostor.setVec3("uLightColor", c.lightColor);
    m_impostor.setVec3("uAmbient", c.ambient);
    m_impostor.setVec3("uFogColor", c.fogColor);
    m_impostor.setVec3("uFogSunColor", c.fogSunColor);
    m_impostor.setFloat("uFogDensity", c.fogDensity);
    m_impostor.setFloat("uFogHeightFalloff", c.fogHeightFalloff);
    m_impostor.setFloat("uFogHeight", c.fogHeight);
    applySunShadows(m_impostor, c);
    m_impostor.setFloat("uBrightness", treeBrightness);
    m_impostor.setFloat("uContrast", treeContrast);
    m_impostor.setFloat("uHue", glm::radians(treeHue));
    m_impostor.setFloat("uViews", static_cast<float>(kViews));
    m_impostor.setFloat("uStart", impostorStart);
    m_impostor.setFloat("uFadeWidth", 15.0f);
    m_impostor.setFloat("uEnd", forestRadius);
    wind::apply(m_impostor, wind, 1.0f);
    m_impostor.setInt("uAlbedo", 0);
    m_impostor.setInt("uNormal", 1);
    for (const TreeSpecies& sp : m_species) {
        if (!sp.enabled || sp.farCount == 0 || !sp.impAlbedo) continue;
        m_impostor.setFloat("uAspect", sp.impAspect);
        m_impostor.setFloat("uSideFrac", sp.impSideFrac);
        gpu.bindTexture(0, sp.impAlbedo);
        gpu.bindTexture(1, sp.impNormal);
        gpu.bindVertexArray(sp.farVAO);
        gpu.draw(rhi::Primitive::TriangleStrip, 0, 4, sp.farCount);
    }
    gpu.bindVertexArray({});
    gpu.bindTexture(1, {});
    gpu.setCullEnabled(true);
}
