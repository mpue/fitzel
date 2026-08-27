#include "VolumetricFog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/CascadedShadowMap.hpp>

#include "FogNoise.hpp"

namespace {

// The fullscreen vertex stage every post-style pass in this program shares.
constexpr const char* kVert = "assets/shaders/sky.vert";

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
    // The march has a vertex stage of its own (the proxy box); only the
    // upsample, which really is one fullscreen quad, uses the shared one.
    m_march    = fitzel::Shader::fromFiles("assets/shaders/volfog.vert",
                                           "assets/shaders/volfog.frag");
    m_upsample = fitzel::Shader::fromFiles(kVert, "assets/shaders/volfogup.frag");
    if (!m_march.isValid() || !m_upsample.isValid()) {
        std::fprintf(stderr, "Failed to load volumetric fog shaders\n");
        return false;
    }
    m_box      = fitzel::Mesh::cube();   // the unit proxy, one draw per volume
    m_noiseTex = bakeFogNoise();
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

// Everything that belongs to ONE volume. The frame-wide uniforms (the depth
// buffer, the noise, the cascades, the eye) are already on the program by the
// time this runs and are deliberately not touched here -- they are the same for
// every volume, and setting them per draw is the difference between a fog volume
// costing a draw call and costing a state change.
void VolumetricFog::drawVolume(const Volume& v, const Params& p, float lightStep) {
    const Medium& m = v.medium;
    const glm::mat4 inv = glm::inverse(v.model);

    m_march.setMat4("uModel", v.model);
    m_march.setMat4("uInvModel", inv);
    m_march.setFloat("uEdge", std::clamp(m.edge, 0.01f, 1.0f));
    m_march.setFloat("uHeightFalloff", std::max(m.heightFalloff, 0.0f));

    m_march.setFloat("uDensity", m.density);
    m_march.setVec3("uColor", m.color);
    m_march.setFloat("uCoverage", std::clamp(m.coverage, 0.0f, 0.95f));

    m_march.setFloat("uNoiseScale", std::max(m.noiseScale, 1e-5f));
    m_march.setFloat("uNoiseVertical", std::clamp(m.verticalDetail, 0.25f, 12.0f));
    m_march.setFloat("uDetail", std::clamp(m.detail, 0.0f, 0.95f));
    m_march.setFloat("uWarp", std::max(m.warp, 0.0f));
    m_march.setVec3("uWind", m.wind);

    m_march.setFloat("uG", std::clamp(m.anisotropy, -0.9f, 0.9f));
    m_march.setFloat("uSunIntensity", m.sunIntensity);
    m_march.setFloat("uAmbientIntensity", m.ambientIntensity);
    m_march.setInt("uSelfShadow", m.selfShadow ? 1 : 0);
    m_march.setInt("uShafts", m.shafts ? 1 : 0);
    m_march.setFloat("uLightStep", lightStep);
    // The same world-space step toward the sun, mapped into the box's space, so
    // the light march can carry both without a matrix multiply per tap.
    m_march.setVec3("uSunDirLocal", glm::mat3(inv) * p.sunDir);
    m_march.setInt("uSteps", m.steps);

    m_box.draw();
}

void VolumetricFog::render(const fitzel::RenderTarget& hdr,
                           const std::vector<Volume>& volumes, const Settings& s,
                           const Params& p, fitzel::Mesh& fsQuad,
                           const fitzel::CascadedShadowMap* shadows) {
    if (!m_ok) return;

    // --- The frame's draw list ----------------------------------------------
    m_draw.clear();
    for (const Volume& v : volumes)
        if (v.medium.density > 1e-5f) m_draw.push_back(v);
    // The scene-wide box is one more volume, not a second code path. It goes in
    // last and is sorted with the rest, so a placed bank inside it composites
    // against it correctly rather than being drawn over it.
    if (s.enabled && s.medium.density > 1e-5f) {
        glm::vec3 lo, hi;
        worldBox(s, p.camPos, lo, hi);
        Volume v;
        v.model = glm::translate(glm::mat4(1.0f), (lo + hi) * 0.5f) *
                  glm::scale(glm::mat4(1.0f), glm::max(hi - lo, glm::vec3(0.01f)));
        v.medium = s.medium;
        m_draw.push_back(v);
    }
    if (m_draw.empty()) return;

    // Back to front, because that is the order the accumulation below composites
    // in. Sorted by the distance to the box's centre, which is not exact for
    // volumes that interpenetrate -- but two overlapping bodies of mist have no
    // correct order anyway, and being consistent frame to frame matters more
    // than being right about a case that has no right answer.
    std::sort(m_draw.begin(), m_draw.end(), [&](const Volume& a, const Volume& b) {
        const glm::vec3 da = glm::vec3(a.model[3]) - p.camPos;
        const glm::vec3 db = glm::vec3(b.model[3]) - p.camPos;
        return glm::dot(da, da) > glm::dot(db, db);
    });
    // Over budget: drop the FAR ones, which are the front of the list.
    if (static_cast<int>(m_draw.size()) > kMaxVolumes)
        m_draw.erase(m_draw.begin(), m_draw.end() - kMaxVolumes);

    const int scale = std::clamp(s.resScale, 1, 4);
    resize(hdr.width() / scale, hdr.height() / scale);

    // --- The march, at a fraction of the pane -------------------------------
    m_fogRT.bind();
    // Depth test OFF, and the proxy boxes are drawn BACK faces only. Between
    // them that is what makes a volume behave whether the eye is outside it or
    // standing in the middle of it: the back faces are there either way (the
    // front ones are clipped away by the near plane once you step inside), and
    // occlusion by the scene is not the depth buffer's job here -- the march
    // reads the depth TEXTURE and stops at whatever the scene put there, which
    // is also how a volume can be half behind a wall.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);

    // Cleared to "nothing scattered, everything behind survives", which is the
    // identity of the accumulation operator below.
    float oldClear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldClear);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(oldClear[0], oldClear[1], oldClear[2], oldClear[3]);

    // Compositing one volume in FRONT of everything already accumulated:
    //   rgb = scatter + accumulated * T      a = accumulated * T
    // which is src*ONE + dst*SRC_ALPHA for colour and dst*SRC_ALPHA for alpha.
    // The same operator the buffer is later blended onto the scene with, because
    // it is the same question asked twice.
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_SRC_ALPHA);

    m_march.bind();
    hdr.bindDepthTexture(0);
    m_march.setInt("uDepth", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, m_noiseTex);
    m_march.setInt("uNoise", 1);

    // The cascades, bound once for every volume in the frame.
    int cascades = 0;
    if (shadows) {
        shadows->bindTextureArray(2);
        cascades = std::min(shadows->cascadeCount(), 4);
        for (int i = 0; i < cascades; ++i) {
            m_march.setMat4(Indexed("uLightSpace", i), shadows->lightMatrices()[i]);
            m_march.setFloat(Indexed("uCascadeSplits", i), shadows->splitDistances()[i]);
        }
    }
    m_march.setInt("uShadowMap", 2);
    // Zero switches the lookup off in the shader, which is what a frame without
    // sun shadows needs -- the sampler still has a texture of the right type
    // bound above whenever there is one at all.
    m_march.setInt("uCascadeCount", cascades);

    m_march.setMat4("uViewProj", p.viewProj);
    m_march.setMat4("uInvViewProj", glm::inverse(p.viewProj));
    m_march.setVec2("uTargetSize", {static_cast<float>(m_w), static_cast<float>(m_h)});
    m_march.setVec3("uCamPos", p.camPos);
    m_march.setVec3("uCamFwd", p.camFwd);
    m_march.setFloat("uTime", p.time);
    m_march.setVec3("uSunDir", p.sunDir);
    m_march.setVec3("uSunColor", p.sunColor);
    m_march.setVec3("uAmbient", p.ambient);

    for (const Volume& v : m_draw) {
        // The light march has to cross a meaningful part of a bank to mean
        // anything, and what "meaningful" is scales with the volume: an eighth
        // of its height, so a shallow ground mist and a tall wall of fog both
        // get three useful taps. The height is the model's Y column, because the
        // proxy cube is one unit tall.
        const float boxH = glm::length(glm::vec3(v.model[1]));
        drawVolume(v, p, std::max(boxH * 0.125f, 0.5f));
    }

    // --- Back onto the scene ------------------------------------------------
    // src + dst*srcAlpha: the in-scatter is added, what is behind is attenuated
    // by the transmittance the march carried out. Alpha of the target is left
    // alone -- nothing downstream reads it, and writing it here would only make
    // that a thing to remember.
    hdr.bind();
    glDisable(GL_CULL_FACE);
    glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
    m_upsample.bind();
    m_fogRT.bindColorTexture(0);
    m_upsample.setInt("uFog", 0);
    m_upsample.setVec2("uTexel", {1.0f / static_cast<float>(m_w),
                                  1.0f / static_cast<float>(m_h)});
    fsQuad.draw();

    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // the state everything else assumes
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}
