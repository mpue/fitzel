#include "fitzel/render/Renderer.hpp"

#include "fitzel/graphics/Material.hpp"
#include "fitzel/graphics/Mesh.hpp"
#include "fitzel/graphics/EnvironmentIBL.hpp"
#include "fitzel/scene/Camera.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

namespace fitzel {

namespace {

// Minimal depth-only shader for the shadow passes -- embedded so the engine
// stays self-contained (no app-provided asset required).
constexpr const char* kDepthVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uLightSpace;
void main() { gl_Position = uLightSpace * uModel * vec4(aPos, 1.0); }
)";

constexpr const char* kDepthFrag = R"(#version 330 core
void main() {}
)";

// Point-shadow pass: write normalized distance-to-light into an R32F cubemap.
constexpr const char* kCubeVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uVP;
out vec3 vWorld;
void main() { vec4 w = uModel * vec4(aPos, 1.0); vWorld = w.xyz; gl_Position = uVP * w; }
)";

constexpr const char* kCubeFrag = R"(#version 330 core
in vec3 vWorld;
layout(location = 0) out float oDist;
uniform vec3  uLightPos;
uniform float uFar;
void main() { oDist = length(vWorld - uLightPos) / uFar; }
)";

// Extract the 6 world-space frustum planes from a view-projection matrix
// (Gribb-Hartmann). Each plane is (nx, ny, nz, d) with the normal pointing
// inward, so a point p is inside when dot(plane.xyz, p) + plane.w >= 0.
std::array<glm::vec4, 6> frustumPlanes(const glm::mat4& m) {
    // Rows of the column-major matrix.
    const glm::vec4 r0{m[0][0], m[1][0], m[2][0], m[3][0]};
    const glm::vec4 r1{m[0][1], m[1][1], m[2][1], m[3][1]};
    const glm::vec4 r2{m[0][2], m[1][2], m[2][2], m[3][2]};
    const glm::vec4 r3{m[0][3], m[1][3], m[2][3], m[3][3]};

    std::array<glm::vec4, 6> planes{
        r3 + r0, r3 - r0, // left, right
        r3 + r1, r3 - r1, // bottom, top
        r3 + r2, r3 - r2, // near, far
    };
    for (glm::vec4& p : planes) {
        p /= glm::length(glm::vec3(p));
    }
    return planes;
}

// Test a world-space AABB (transformed from a local AABB by `model`) against
// the frustum. Conservative (false positives possible, never false negatives).
bool aabbVisible(const std::array<glm::vec4, 6>& planes, const glm::mat4& model,
                 const glm::vec3& localMin, const glm::vec3& localMax) {
    // Transform the 8 corners to world space and take their AABB.
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner{
            (i & 1) ? localMax.x : localMin.x,
            (i & 2) ? localMax.y : localMin.y,
            (i & 4) ? localMax.z : localMin.z};
        const glm::vec3 w = glm::vec3(model * glm::vec4(corner, 1.0f));
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
    }

    for (const glm::vec4& plane : planes) {
        // Positive vertex: the AABB corner farthest along the plane normal.
        const glm::vec3 pv{
            plane.x >= 0.0f ? hi.x : lo.x,
            plane.y >= 0.0f ? hi.y : lo.y,
            plane.z >= 0.0f ? hi.z : lo.z};
        if (glm::dot(glm::vec3(plane), pv) + plane.w < 0.0f) {
            return false; // fully outside this plane
        }
    }
    return true;
}

} // namespace

const glm::vec4 Renderer::kNoClip = glm::vec4(0.0f, 1.0f, 0.0f, 1.0e6f);

Renderer::Renderer(int shadowResolution, int cascades)
    : m_csm(shadowResolution, cascades),
      m_depthShader(Shader::fromSource(kDepthVert, kDepthFrag)),
      m_cubeDistShader(Shader::fromSource(kCubeVert, kCubeFrag)) {}

void Renderer::setViewport(int width, int height) {
    m_vpWidth  = width;
    m_vpHeight = height;
}

void Renderer::begin(const Camera& camera, float aspect,
                     const DirectionalLight& light) {
    m_camera = &camera;
    m_aspect = aspect;
    m_light  = light;
    m_queue.clear();
}

void Renderer::submit(const Mesh& mesh, const Material& material,
                      const glm::mat4& model, bool castsPointShadow,
                      bool reflective, float opacity, bool forceTransparent) {
    m_queue.push_back({&mesh, &material, model, castsPointShadow, reflective,
                       opacity, forceTransparent});
}

void Renderer::prepareShadows(const ShadowCaster& extra) {
    if (!m_camera) return;

    m_csm.update(*m_camera, m_aspect, m_light.direction);

    const int cascades = m_csm.cascadeCount();
    for (int i = 0; i < cascades; ++i) {
        m_csm.beginCascade(i);
        m_depthShader.bind();
        m_depthShader.setMat4("uLightSpace", m_csm.lightMatrices()[i]);
        for (const auto& r : m_queue) {
            m_depthShader.setMat4("uModel", r.model);
            r.mesh->draw();
        }
        if (extra) extra(m_csm.lightMatrices()[i]);
    }
    m_csm.end(m_vpWidth, m_vpHeight);
}

void Renderer::preparePointShadows() {
    // Shadow-casting point lights first, so their indices line up with the
    // cubemaps and with the lit shader's first uShadowCount lights.
    std::stable_partition(m_pointLights.begin(), m_pointLights.end(),
                          [](const PointLight& l) { return l.castShadows; });
    m_shadowedCount = 0;
    for (const PointLight& l : m_pointLights)
        if (l.castShadows) ++m_shadowedCount;
    m_shadowedCount = std::min(m_shadowedCount, kMaxShadowedPoints);
    if (m_shadowedCount == 0) return;

    while (static_cast<int>(m_pointShadows.size()) < m_shadowedCount)
        m_pointShadows.emplace_back(512);

    const glm::vec3* dirs = CubeShadowMap::faceDirs();
    const glm::vec3* ups  = CubeShadowMap::faceUps();

    glDisable(GL_CLIP_DISTANCE0);
    glEnable(GL_DEPTH_TEST);
    // Cull front faces so single-sided ground doesn't self-shadow (only closed
    // casters write their far side); avoids acne blacking out the lit surface.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    m_cubeDistShader.bind();
    for (int k = 0; k < m_shadowedCount; ++k) {
        const PointLight& l = m_pointLights[k];
        const float far = std::max(l.range, 0.5f);
        const glm::mat4 pr = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, far);
        m_cubeDistShader.setVec3("uLightPos", l.position);
        m_cubeDistShader.setFloat("uFar", far);
        for (int f = 0; f < 6; ++f) {
            m_pointShadows[k].beginFace(f);
            const glm::mat4 vp = pr * glm::lookAt(l.position, l.position + dirs[f], ups[f]);
            m_cubeDistShader.setMat4("uVP", vp);
            for (const auto& r : m_queue) {
                if (!r.castsPointShadow) continue; // e.g. the ground
                m_cubeDistShader.setMat4("uModel", r.model);
                r.mesh->draw();
            }
        }
    }
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_vpWidth, m_vpHeight); // restore from the 512^2 cube faces
}

void Renderer::prepareEnvProbe(const glm::vec3& pos, const SkyDrawer& drawSky) {
    if (!m_camera) return;

    const glm::mat4 proj = CubeRenderTarget::faceProjection(0.2f, 4000.0f);

    glDisable(GL_CLIP_DISTANCE0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    for (int f = 0; f < 6; ++f) {
        m_envWrite->beginFace(f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const glm::mat4 view = CubeRenderTarget::faceView(pos, f);
        if (drawSky) drawSky(glm::inverse(proj * view), pos);
        // Linear (untonemapped) so reflections match the HDR scene; skip the
        // reflective surfaces themselves and sample last frame's probe.
        renderScene(view, proj, pos, kNoClip, /*tonemap=*/false,
                    /*skipReflective=*/true);
    }
    m_envWrite->generateMipmaps();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_vpWidth, m_vpHeight);
    // The freshly captured cube becomes the one the lit passes sample this frame.
    std::swap(m_envRead, m_envWrite);
}

void Renderer::renderScene(const glm::mat4& view, const glm::mat4& proj,
                           const glm::vec3& eye, const glm::vec4& clipPlane,
                           bool tonemap, bool skipReflective) {
    const glm::mat4 viewProj = proj * view;
    const int cascades = m_csm.cascadeCount();

    const std::array<glm::vec4, 6> planes = frustumPlanes(viewProj);
    m_lastDrawn  = 0;
    m_lastCulled = 0;

    m_csm.bindTextureArray(kShadowMapUnit);
    glEnable(GL_CLIP_DISTANCE0);

    auto drawOne = [&](const Renderable& r) {
        if (!aabbVisible(planes, r.model,
                         r.mesh->boundsMin(), r.mesh->boundsMax())) {
            ++m_lastCulled;
            return;
        }
        ++m_lastDrawn;

        Shader* s = r.material->shader();
        // Baseline: reflection off + fully opaque. A Material only uploads the
        // uniforms it defines, so without these a reflective/transparent material
        // would leave uReflectivity/uAlpha set on the shared program and later
        // draws (terrain, road) would inherit it. uAlpha carries per-object
        // opacity (1 for the opaque queue).
        s->bind();
        s->setFloat("uReflectivity", 0.0f);
        s->setFloat("uAlpha", r.opacity);
        s->setInt("uGlass", 0);
        s->setInt("uHasNormalMap", 0);
        s->setInt("uAlphaCutout", 0); // baseline: material re-enables if Cutout

        r.material->apply(); // binds shader + material params/textures

        s->setMat4("uModel", r.model);
        s->setMat4("uView", view);
        s->setMat4("uViewProj", viewProj);
        s->setVec3("uViewPos", eye);
        s->setVec4("uClipPlane", clipPlane);
        s->setVec3("uLightDir", m_light.direction);
        s->setVec3("uLightColor", m_light.color);
        s->setVec3("uAmbient", m_light.ambient);
        s->setVec3("uFogColor", m_fog.color);
        s->setVec3("uFogSunColor", m_fog.sunColor);
        s->setFloat("uFogDensity", m_fog.density);
        s->setFloat("uFogHeightFalloff", m_fog.heightFalloff);
        s->setFloat("uFogHeight", m_fog.height);
        s->setFloat("uExposure", m_exposure);
        s->setInt("uTonemap", tonemap ? 1 : 0);
        s->setInt("uCascadeCount", cascades);
        s->setInt("uShadowMap", kShadowMapUnit);

        // Point lights (no-ops on shaders that don't declare these uniforms).
        const int pc = std::min(static_cast<int>(m_pointLights.size()), kMaxPointLights);
        s->setInt("uPointCount", pc);
        for (int i = 0; i < pc; ++i) {
            const std::string idx = std::to_string(i);
            s->setVec3("uPointPos[" + idx + "]", m_pointLights[i].position);
            s->setVec3("uPointColor[" + idx + "]", m_pointLights[i].color);
            s->setFloat("uPointRange[" + idx + "]", m_pointLights[i].range);
        }
        // Point-shadow cubemaps. Always give ALL four cube samplers their own
        // units (12..15) -- even the unused ones -- so none is left aliasing
        // unit 0, where uTexture (a sampler2D) lives. A samplerCube and a
        // sampler2D pointing at the same unit is a type clash that makes the
        // driver drop the whole draw once the cube is sampled, so every lit
        // surface (the terrain) would vanish when point shadows switch on.
        s->setInt("uShadowCount", m_shadowedCount);
        for (int k = 0; k < kMaxShadowedPoints; ++k) {
            const std::string ks = std::to_string(k);
            if (k < m_shadowedCount) {
                m_pointShadows[k].bindTexture(kPointShadowUnit + k);
                s->setFloat("uShadowFar" + ks, std::max(m_pointLights[k].range, 0.5f));
                s->setFloat("uShadowBias" + ks, m_pointLights[k].shadowBias);
            } else if (m_shadowedCount > 0) {
                // Bind a real cubemap so the unit stays a complete cube texture.
                m_pointShadows[0].bindTexture(kPointShadowUnit + k);
            }
            s->setInt("uShadowCube" + ks, kPointShadowUnit + k);
        }

        // Environment probe for reflective materials. Bound for every lit draw
        // (even non-reflective ones) so this samplerCube never aliases unit 0.
        m_envRead->bindTexture(kEnvProbeUnit);
        s->setInt("uEnvProbe", kEnvProbeUnit);
        s->setFloat("uEnvMaxLod", static_cast<float>(m_envRead->mipLevels() - 1));

        // Image-based lighting from an HDRI. Bind the irradiance + prefilter
        // cubemaps (or the probe cube as a harmless fallback so these samplerCubes
        // never alias unit 0); the shader only reads them when uUseIBL == 1.
        const bool useIbl = m_ibl && m_ibl->valid() && m_iblEnabled;
        if (useIbl) {
            m_ibl->bindIrradiance(kIrradianceUnit);
            m_ibl->bindPrefilter(kPrefilterUnit);
        } else {
            m_envRead->bindTexture(kIrradianceUnit);
            m_envRead->bindTexture(kPrefilterUnit);
        }
        s->setInt("uIrradiance", kIrradianceUnit);
        s->setInt("uPrefilter", kPrefilterUnit);
        s->setInt("uUseIBL", useIbl ? 1 : 0);
        s->setFloat("uIBLIntensity", m_iblIntensity);
        s->setFloat("uPrefilterMaxLod",
                    useIbl ? static_cast<float>(m_ibl->prefilterMipLevels() - 1) : 0.0f);

        for (int i = 0; i < cascades; ++i) {
            const std::string idx = std::to_string(i);
            s->setMat4("uLightSpace[" + idx + "]", m_csm.lightMatrices()[i]);
            s->setFloat("uCascadeSplits[" + idx + "]", m_csm.splitDistances()[i]);
        }

        r.mesh->draw();
    };

    // Opaque queue first, then transparent surfaces back-to-front with alpha
    // blending and depth writes off (so they blend over the opaque scene and
    // each other without occluding). Reflective probe pass still excludes them.
    std::vector<const Renderable*> opaque, transparent;
    for (const auto& r : m_queue) {
        if (skipReflective && r.reflective) continue;
        (r.opacity < 0.999f || r.forceTransparent ? transparent : opaque).push_back(&r);
    }
    for (const Renderable* r : opaque) drawOne(*r);
    if (!transparent.empty()) {
        std::sort(transparent.begin(), transparent.end(),
            [&](const Renderable* a, const Renderable* b) {
                const glm::vec3 da = glm::vec3(a->model[3]) - eye;
                const glm::vec3 db = glm::vec3(b->model[3]) - eye;
                return glm::dot(da, da) > glm::dot(db, db); // farthest first
            });
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (const Renderable* r : transparent) drawOne(*r);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    glDisable(GL_CLIP_DISTANCE0);
}

void Renderer::end() {
    if (!m_camera) return;
    prepareShadows();
    renderScene(m_camera->viewMatrix(),
                m_camera->projectionMatrix(m_aspect),
                m_camera->position(), kNoClip);
    m_queue.clear();
}

} // namespace fitzel
