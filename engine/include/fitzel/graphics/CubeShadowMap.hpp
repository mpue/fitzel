#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace fitzel {

// An omnidirectional shadow map for a point light: a cube of linear distance
// (distance-to-light / far, stored in an R32F cubemap). Render the scene into
// all six faces from the light, then sample it in the lit shader. Move-only.
class CubeShadowMap {
public:
    explicit CubeShadowMap(int resolution = 512);
    ~CubeShadowMap();

    CubeShadowMap(const CubeShadowMap&)            = delete;
    CubeShadowMap& operator=(const CubeShadowMap&) = delete;
    CubeShadowMap(CubeShadowMap&& other) noexcept;
    CubeShadowMap& operator=(CubeShadowMap&& other) noexcept;

    // Bind the FBO with cube face `face` (0..5) as the colour target; sets the
    // viewport and clears to "far".
    void beginFace(int face);

    // Bind the distance cubemap texture to a texture unit.
    void bindTexture(std::uint32_t unit) const;

    // Render the six faces into layers first..first+5 of a 2D array texture
    // instead of into a cubemap of its own. The browser build does this for
    // all its lights in ONE array (Renderer::preparePointShadows): WebGL2's 16
    // samplers per shader leave room for one point-shadow sampler, not four.
    void renderIntoLayers(std::uint32_t arrayTexture, int firstLayer) {
        m_array = arrayTexture;
        m_firstLayer = firstLayer;
    }

    int resolution() const { return m_res; }

    // The six cube-face view directions / up vectors for a light at the origin.
    static const glm::vec3* faceDirs();
    static const glm::vec3* faceUps();

private:
    std::uint32_t m_fbo   = 0;
    std::uint32_t m_cube  = 0;
    std::uint32_t m_depth = 0;
    int           m_res   = 0;
    std::uint32_t m_array = 0;       // renderIntoLayers: not owned
    int           m_firstLayer = 0;
};

} // namespace fitzel
