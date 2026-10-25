#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/rhi/Types.hpp>

struct MaterialDef;

// The Materials panel's preview: the selected material on a sphere, a cube or a
// flat tile, lit like a studio, drawn offscreen into a square texture the panel
// shows with ImGui::Image. The surface follows lit.frag's rules
// (matpreview.frag), so the preview is what the scene draws.
//
// Runs in the middle of the editor's frame: every piece of device state it
// touches is put back (the same discipline as TreePreview).
class MaterialPreview {
public:
    enum class Shape { Sphere, Cube, Tile };

    MaterialPreview() = default;
    ~MaterialPreview();
    MaterialPreview(const MaterialPreview&)            = delete;
    MaterialPreview& operator=(const MaterialPreview&) = delete;

    // Shaders and shapes. Needs a live device (a window); false if a shader failed.
    bool init();
    bool ready() const { return m_shader.isValid(); }

    // Draw `md` at size x size; `yawDeg` turns the shape. Returns the colour
    // texture, none if not ready. Rows are bottom-up (GL): show it with
    // uv0 = (0,1), uv1 = (1,0).
    fitzel::rhi::TextureHandle render(const MaterialDef& md, int size, Shape shape, float yawDeg);

private:
    struct Part {
        fitzel::rhi::VertexArrayHandle vao;
        fitzel::rhi::BufferHandle      vbo, ibo;
        int count = 0;
    };
    static void upload(Part& p, const std::vector<float>& v, const std::vector<std::uint32_t>& ix);
    static void release(Part& p);

    fitzel::Shader                        m_shader;
    std::unique_ptr<fitzel::RenderTarget> m_target;
    Part m_sphere, m_cube, m_tile, m_quad;
};