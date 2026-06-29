#include "fitzel/render/Renderer.hpp"

#include "fitzel/graphics/Material.hpp"
#include "fitzel/graphics/Mesh.hpp"
#include "fitzel/scene/Camera.hpp"

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
                           const glm::vec3& eye, const glm::vec4& clipPlane) {
    const glm::mat4 viewProj = proj * view;
    const int cascades = m_csm.cascadeCount();

    m_csm.bindTextureArray(kShadowMapUnit);
    glEnable(GL_CLIP_DISTANCE0);

    for (const auto& r : m_queue) {
        r.material->apply(); // binds shader + material params/textures
        Shader* s = r.material->shader();

        s->setMat4("uModel", r.model);
        s->setMat4("uView", view);
        s->setMat4("uViewProj", viewProj);
        s->setVec3("uViewPos", eye);
        s->setVec4("uClipPlane", clipPlane);
        s->setVec3("uLightDir", m_light.direction);
        s->setVec3("uLightColor", m_light.color);
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
