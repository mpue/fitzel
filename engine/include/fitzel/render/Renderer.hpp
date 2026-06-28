#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "fitzel/graphics/Shader.hpp"
#include "fitzel/graphics/CascadedShadowMap.hpp"

namespace fitzel {

class Camera;
class Mesh;
class Material;

struct DirectionalLight {
    glm::vec3 direction{0.5f, 1.0f, 0.35f}; // points *towards* the light
    glm::vec3 color{1.0f, 0.97f, 0.9f};
};

// A high-level renderer that drives cascaded shadow mapping and a forward lit
// pass. The app submits (mesh, material, model) tuples between begin() and
// end(); the renderer renders all cascades, then the lit scene, feeding the
// shadow uniforms to each material's shader automatically.
//
// Materials' shaders are expected to declare the shadow/lighting uniforms the
// renderer sets (see sandbox/assets/shaders/lit.frag for the contract).
class Renderer {
public:
    // The texture unit the cascade depth array is bound to. Materials must not
    // use this unit for their own textures.
    static constexpr int kShadowMapUnit = 7;

    explicit Renderer(int shadowResolution = 2048, int cascades = 4);

    void setViewport(int width, int height);

    void begin(const Camera& camera, float aspect, const DirectionalLight& light);
    void submit(const Mesh& mesh, const Material& material, const glm::mat4& model);
    void end();

    CascadedShadowMap&       shadows()       { return m_csm; }
    const CascadedShadowMap& shadows() const { return m_csm; }

private:
    struct Renderable {
        const Mesh*     mesh;
        const Material* material;
        glm::mat4       model;
    };

    CascadedShadowMap m_csm;
    Shader            m_depthShader;
    std::vector<Renderable> m_queue;

    const Camera*    m_camera = nullptr;
    float            m_aspect = 1.0f;
    DirectionalLight m_light;
    int              m_vpWidth  = 1;
    int              m_vpHeight = 1;
};

} // namespace fitzel
