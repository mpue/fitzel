// VegetationSystem's impostors: baking a species into a card atlas and drawing
// the forest field with it. Split out of VegetationSystem.cpp only for size.

#include "VegetationSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

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
void uploadMipped(unsigned tex, std::vector<unsigned char> level, int w, int h,
                  bool keepCoverage) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    const auto coverage = [](const std::vector<unsigned char>& p, float s) {
        std::size_t in = 0, n = p.size() / 4;
        for (std::size_t i = 3; i < p.size(); i += 4)
            if (p[i] * s >= 127.5f) ++in;
        return n ? static_cast<float>(in) / static_cast<float>(n) : 0.0f;
    };
    const float target = coverage(level, 1.0f);
    int mip = 0;
    for (;;) {
        glTexImage2D(GL_TEXTURE_2D, mip, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     level.data());
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
        if (!sp.enabled || sp.lods.empty() || sp.impAlbedo == 0) continue;
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
    if (lod.cpuVerts.empty() || lod.vbo == 0) return;

    // The crown's reach around the trunk (the mesh is unit height, base at 0).
    float maxR = 0.05f;
    for (std::size_t i = 0; i + 7 < lod.cpuVerts.size(); i += 8)
        maxR = std::max(maxR, std::sqrt(lod.cpuVerts[i] * lod.cpuVerts[i] +
                                        lod.cpuVerts[i + 2] * lod.cpuVerts[i + 2]));
    maxR *= 1.03f;
    const float aspect = glm::clamp(2.0f * maxR / 1.04f, 0.2f, 2.5f);
    const int   vw = std::max(8, static_cast<int>(std::round(kViewH * aspect)));
    const int   W  = vw * kViews, H = kViewH;

    // --- GL state we are about to disturb.
    GLint prevFbo = 0, prevVp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prevCull  = glIsEnabled(GL_CULL_FACE);
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLfloat prevClear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);

    GLuint tex[2], rbo = 0, fbo = 0, vao = 0;
    glGenTextures(2, tex);
    for (GLuint t : tex) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[0], 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, tex[1], 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (complete) {
        glViewport(0, 0, W, H);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glDisable(GL_CULL_FACE);   // leaf cards are seen from both sides
        glDisable(GL_BLEND);

        // The mesh without its instance attributes: a VAO of its own over the
        // same buffers.
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, lod.vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lod.ibo);
        const GLsizei ms = 8 * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, ms, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, ms, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, ms, (void*)(6 * sizeof(float)));

        const glm::mat4 proj = glm::ortho(-maxR, maxR, -0.02f, 1.02f, -(maxR + 1.0f),
                                          maxR + 1.0f);
        m_impostorBake.bind();
        m_impostorBake.setMat4("uProj", proj);
        m_impostorBake.setInt("uTex", 0);
        for (int v = 0; v < kViews; ++v) {
            glViewport(v * vw, 0, vw, H);
            m_impostorBake.setFloat("uYaw", static_cast<float>(v) * 1.5707963f);
            for (const TreeLOD::Prim& p : lod.prims) {
                if (p.hasTex) p.tex.bind(0);
                m_impostorBake.setInt("uHasTex", p.hasTex ? 1 : 0);
                m_impostorBake.setInt("uAlphaCutout", p.cutout ? 1 : 0);
                glDrawElements(GL_TRIANGLES, p.count, GL_UNSIGNED_INT,
                               reinterpret_cast<const void*>(
                                   static_cast<std::uintptr_t>(p.first) * sizeof(std::uint32_t)));
            }
        }
        glBindVertexArray(0);

        // Back to the CPU for the dilation and the coverage-keeping mips.
        std::vector<unsigned char> albedo(static_cast<std::size_t>(W) * H * 4);
        std::vector<unsigned char> normal(albedo.size());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, albedo.data());
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, normal.data());

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

        if (!sp.impAlbedo) glGenTextures(1, &sp.impAlbedo);
        if (!sp.impNormal) glGenTextures(1, &sp.impNormal);
        uploadMipped(sp.impAlbedo, albedo, W, H, true);
        uploadMipped(sp.impNormal, normal, W, H, false);
        sp.impAspect = 2.0f * maxR / 1.04f;
        std::fprintf(stderr, "impostor %s: %dx%d, aspect %.2f\n", sp.name.c_str(), W, H,
                     sp.impAspect);
    }

    // --- Put everything back.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
    if (prevBlend) glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
    if (vao) glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteTextures(2, tex);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void VegetationSystem::drawImpostors(const FrameContext& c) {
    if (!terrainPresent || !treeEnabled || !m_impostor.isValid()) return;
    glDisable(GL_CULL_FACE);
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
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sp.impAlbedo);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sp.impNormal);
        glBindVertexArray(sp.farVAO);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, sp.farCount);
    }
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_CULL_FACE);
}
