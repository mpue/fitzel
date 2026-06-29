#include "fitzel/render/Renderer.hpp"

#include "fitzel/graphics/Material.hpp"
#include "fitzel/graphics/Mesh.hpp"
#include "fitzel/scene/Camera.hpp"

#include <array>
#include <string>

#include <glad/gl.h>

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
      m_depthShader(Shader::fromSource(kDepthVert, kDepthFrag)) {}

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
                      const glm::mat4& model) {
    m_queue.push_back({&mesh, &material, model});
}

void Renderer::prepareShadows() {
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
    }
    m_csm.end(m_vpWidth, m_vpHeight);
}

void Renderer::renderScene(const glm::mat4& view, const glm::mat4& proj,
                           const glm::vec3& eye, const glm::vec4& clipPlane,
                           bool tonemap) {
    const glm::mat4 viewProj = proj * view;
    const int cascades = m_csm.cascadeCount();

    const std::array<glm::vec4, 6> planes = frustumPlanes(viewProj);
    m_lastDrawn  = 0;
    m_lastCulled = 0;

    m_csm.bindTextureArray(kShadowMapUnit);
    glEnable(GL_CLIP_DISTANCE0);

    for (const auto& r : m_queue) {
        if (!aabbVisible(planes, r.model,
                         r.mesh->boundsMin(), r.mesh->boundsMax())) {
            ++m_lastCulled;
            continue;
        }
        ++m_lastDrawn;

        r.material->apply(); // binds shader + material params/textures
        Shader* s = r.material->shader();

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

        for (int i = 0; i < cascades; ++i) {
            const std::string idx = std::to_string(i);
            s->setMat4("uLightSpace[" + idx + "]", m_csm.lightMatrices()[i]);
            s->setFloat("uCascadeSplits[" + idx + "]", m_csm.splitDistances()[i]);
        }

        r.mesh->draw();
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
