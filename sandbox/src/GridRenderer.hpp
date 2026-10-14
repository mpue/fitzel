#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include <fitzel/graphics/Shader.hpp>

#include "FrameRender.hpp"

// The editor's construction grid: a lattice drawn on the 3D cursor's plane --
// horizontal, or upright through the cursor in an orthographic side view --
// anti-aliased per pixel and faded out with distance so it meets the
// horizon as tone rather than as a moire field.
//
// Drawn LAST, into the finished image, after tonemapping, bloom, SSAO, motion
// blur and FXAA. A drawing aid is not part of the scene: it must not dim at
// dusk, bloom in the sun, smear when the camera whips round, or change colour
// with the exposure -- all of which it did while it lived in the HDR pass. The
// price is that the depth buffer is no longer attached by then, so the occlusion
// test against the world is done in the shader against the scene's depth
// texture (see `sceneDepthUnit`).
//
// It is the 3D cursor's grid made visible, not decoration. `cell` is the
// cursor's snap step and the lattice is anchored at the world origin, which is
// where that snap rounds to -- so every crossing you can see is a position an
// object will actually land on, and "snap to grid" is a move you can aim rather
// than a number you trust. `plane` follows the cursor's height, so the grid also
// answers the other half of the question: which plane am I placing on.
//
// Editor-only: the player has no cursor and nothing to place.
class GridRenderer : public RendererBase {
public:
    GridRenderer() = default;
    ~GridRenderer() override;
    GridRenderer(const GridRenderer&)            = delete;
    GridRenderer& operator=(const GridRenderer&) = delete;

    // Compile the shader (needs a live GL context). False if it failed.
    bool init();

    // Draw into the currently-bound target -- which is the finished image, so the
    // caller must have bound the scene's depth texture to `sceneDepthUnit` and
    // set `viewportPx` first, or the occlusion test has nothing to read. Uses no
    // depth buffer of its own and leaves GL state as it found it.
    void draw(const FrameContext& ctx) override;

    // The scene's depth, for hiding the plane behind solid geometry: the texture
    // unit the caller bound it to, and the size of the image being drawn into
    // (the two together turn a pixel into a depth lookup).
    glm::vec2 viewportPx{1.0f};
    int       sceneDepthUnit = 0;

    bool      enabled = true;
    float     cell    = 1.0f;   // fine cell size (m); bold line every ten
    // The axis the grid's plane is square to: 1 (Y) is the ground plane; 0 (X)
    // and 2 (Z) stand it upright, for the side and front views, where a ground
    // grid is seen edge-on and draws nothing.
    int       axis    = 1;
    float     plane   = 0.0f;   // where on `axis` the grid lies (world metres)
    // Drawn this far above `plane`. On flat terrain the ground IS the cursor's
    // plane, and a grid in the same plane loses the depth test to it every other
    // pixel -- it shimmers. A few centimetres are invisible as an offset and end
    // that; the snapping still happens at `plane`, only the picture is lifted
    // (towards the eye, whichever side of the plane it is on).
    float     lift    = 0.03f;
    float     fade    = 220.0f; // distance at which it has gone entirely (m)
    // An orthographic lens looks along one direction everywhere, so the grazing
    // fade reads that rather than the eye-to-pixel ray (which in ortho means
    // nothing: the eye can sit anywhere along the view line).
    bool      ortho   = false;
    glm::vec3 viewDir{0.0f, 0.0f, -1.0f};
    glm::vec3 color{0.62f, 0.66f, 0.72f};
    float     opacity = 1.0f;
    bool      axes    = true;   // colour the two lines through the world origin
    // The cursor's cell is tinted; set from the 3D cursor each frame. Only the
    // two in-plane coordinates are read -- `plane` places the grid along `axis`.
    glm::vec3 cursor{0.0f};
    bool      highlightCursorCell = true;

private:
    fitzel::Shader m_shader;
    std::uint32_t  m_vao = 0; // empty: the quad's corners come from gl_VertexID
};
