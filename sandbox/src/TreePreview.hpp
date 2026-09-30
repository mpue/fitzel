#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>

#include "TreeGen.hpp"

// The tree generator's studio: one tree on a patch of ground under a sun, with a
// real shadow map -- a crown that does not shade itself reads as a flat cut-out
// and would be judged as one -- and lit the way tree.frag lights the forest, so
// what looks right here looks right out there. Renders into its own target; the
// panel shows it with ImGui::Image, the check harness reads it back.
class TreePreview {
public:
    TreePreview() = default;
    ~TreePreview();
    TreePreview(const TreePreview&)            = delete;
    TreePreview& operator=(const TreePreview&) = delete;

    // Shaders. Needs a live GL context; false if one failed to compile.
    bool init();
    void setMesh(const treegen::Mesh& m);
    // `barkNormal` may be an invalid (empty) texture: no relief. It must be in
    // glTF's convention, green up (see treeglb::flipGreen).
    void setTextures(fitzel::Texture bark, fitzel::Texture leaf, fitzel::Texture barkNormal);
    void setReliefStrength(float s) { m_relief = s; }

    struct View {
        float yaw = 35.0f, pitch = 10.0f;    // camera about the tree (deg)
        float zoom = 1.0f;                   // 1 = the whole tree fills the frame
        float lift = 0.0f;                   // look-at height, fraction of the tree (0 = middle)
        float sunYaw = 50.0f, sunPitch = 42.0f;
    };
    // Draw at w x h and return the colour texture. Puts back the GL state it
    // touches, so it can run in the middle of the editor's frame.
    std::uint32_t render(int w, int h, const View& v);
    // The last render, RGBA, top row first.
    std::vector<std::uint8_t> readRGBA() const;
    int width()  const { return m_target ? m_target->width() : 0; }
    int height() const { return m_target ? m_target->height() : 0; }

private:
    struct Part {
        std::uint32_t vao = 0, vbo = 0, ibo = 0;
        int count = 0;
    };
    static void upload(Part& p, const std::vector<float>& v, const std::vector<std::uint32_t>& ix);
    static void release(Part& p);
    void drawParts(const fitzel::Shader& sh) const;

    fitzel::Shader m_lit, m_depth;
    Part m_bark, m_leaves, m_ground;
    fitzel::Texture m_barkTex, m_leafTex, m_barkNrm;
    float m_relief = 1.0f;
    std::unique_ptr<fitzel::RenderTarget> m_target, m_shadow;
    glm::vec3 m_lo{-1.0f, 0.0f, -1.0f}, m_hi{1.0f, 2.0f, 1.0f};
    float m_groundR = 1.0f;
};