#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include "fitzel/rhi/Types.hpp"

namespace fitzel {

// An omnidirectional shadow map for a point light: a cube of linear distance
// (distance-to-light / far, R32F). The six faces land in six layers of a 2D
// array texture the renderer owns -- one array for all its lights, so the lit
// shader needs one sampler for every point shadow instead of one per light
// (lit.frag, uShadowArr). This holds the framebuffer and depth buffer the faces
// render through. Move-only.
class CubeShadowMap {
public:
    explicit CubeShadowMap(int resolution = 512);
    ~CubeShadowMap();

    CubeShadowMap(const CubeShadowMap&)            = delete;
    CubeShadowMap& operator=(const CubeShadowMap&) = delete;
    CubeShadowMap(CubeShadowMap&& other) noexcept;
    CubeShadowMap& operator=(CubeShadowMap&& other) noexcept;

    // Render the six faces into layers first..first+5 of `arrayTexture`.
    void renderIntoLayers(rhi::TextureHandle arrayTexture, int firstLayer) {
        m_array = arrayTexture;
        m_firstLayer = firstLayer;
    }

    // Bind the framebuffer with cube face `face` (0..5) as the colour target; sets the
    // viewport and clears the depth, and the colour to "far" unless told not
    // to (drawing over what is already there).
    void beginFace(int face, bool clearColour = true);

    // Copy one whole layer from `src` to the same layer of `dst` (both arrays
    // of this map's size and format), through this map's framebuffer.
    void copyLayer(rhi::TextureHandle src, rhi::TextureHandle dst, int layer);

    int resolution() const { return m_res; }

    // The six cube-face view directions / up vectors for a light at the origin.
    static const glm::vec3* faceDirs();
    static const glm::vec3* faceUps();

private:
    rhi::FramebufferHandle m_fbo;
    rhi::TextureHandle     m_depth;
    int           m_res   = 0;
    rhi::TextureHandle m_array;      // renderIntoLayers: not owned
    int           m_firstLayer = 0;
};

} // namespace fitzel
