#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/rhi/Types.hpp>
#include <fitzel/world/Model.hpp>

// The retargeting window's studio: two figures on a floor with a metre grid --
// the motion's own figure on the left, the character playing it on the right --
// lit, with their shadows on the floor (where a foot is planted shows there
// first), and the humanoid bones drawn over them. Renders into its own target;
// the panel shows it with ImGui::Image, retargetpanelcheck reads it back.
// Puts back every bit of device state it touches, so it can run mid-frame.
class RetargetPreview {
public:
    RetargetPreview() = default;
    ~RetargetPreview() = default;
    RetargetPreview(const RetargetPreview&)            = delete;
    RetargetPreview& operator=(const RetargetPreview&) = delete;

    // The shader. Needs a live device (a window); false if it did not compile.
    bool init();

    // Side 0 is the source, side 1 the character. Uploads the meshes and their
    // colour maps; null empties the side. `model` must outlive its use here:
    // its bind vertices are skinned again for every new pose.
    void setModel(int side, const fitzel::ModelData* model);
    // Pose one side: the joint palette in sampleSkeleton's convention (empty =
    // bind pose) and where the figure stands (model -> studio).
    void setPose(int side, const std::vector<glm::mat4>& palette, const glm::mat4& place);

    // The bone overlay, in studio space.
    struct Segment {
        glm::vec3 a{0.0f}, b{0.0f};
        glm::vec4 color{1.0f};
        float     width = 0.02f;   // metres
    };
    void setBones(std::vector<Segment> segs) { m_segs = std::move(segs); }

    // A wooden mannequin for a side whose file is a bare skeleton (most
    // motion files are): rounded limbs between joints, in studio space.
    struct Capsule {
        glm::vec3 a{0.0f}, b{0.0f};
        float     r = 0.05f;
    };
    void setMannequin(int side, std::vector<Capsule> caps);

    struct View {
        glm::vec3 target{0.0f, 0.9f, 0.0f};   // looked at
        float     pitch = 8.0f;               // degrees above the horizon
        float     dist  = 5.0f;               // metres
        float     floorR = 6.0f;              // the floor fades out by here
        bool      bones = true;
        // A camera of the caller's own instead of the turntable above (the Hold
        // Pose Studio looks from the player's eye): its position, where it looks,
        // its up and its vertical field of view. No floor, no shadows.
        bool      free = false;
        glm::vec3 eye{0.0f}, at{0.0f, 0.0f, -1.0f}, up{0.0f, 1.0f, 0.0f};
        float     fov = 30.0f;                // degrees, vertical
        float     nearZ = 0.05f, farZ = 0.0f; // farZ 0: from the distance
        bool      floor = true;
        // How opaque each side is drawn (1 = solid). A see-through side is drawn
        // after the solid one, so what is inside it shows.
        float     alpha[2] = {1.0f, 1.0f};
    };
    // Draw at w x h and return the colour texture.
    fitzel::rhi::TextureHandle render(int w, int h, const View& v);
    // Where a studio point landed in the last render (pixels, top-left origin).
    glm::vec2 project(const glm::vec3& p) const;
    // The ray through pixel (x, y) of the last render (top-left origin): from
    // the camera, unit direction.
    void ray(float x, float y, glm::vec3& origin, glm::vec3& dir) const;
    // The last render's camera (for a gizmo drawn over the picture).
    const glm::mat4& viewMatrix() const { return m_viewM; }
    const glm::mat4& projMatrix() const { return m_projM; }
    std::vector<std::uint8_t> readRGBA() const;
    int width() const { return m_target ? m_target->width() : 0; }
    int height() const { return m_target ? m_target->height() : 0; }

private:
    struct Part {
        fitzel::Mesh    mesh;
        fitzel::Texture tex;
        bool            hasTex = false, cutout = false;
        glm::vec4       color{1.0f};
        std::size_t     prim = 0;
    };
    struct Side {
        const fitzel::ModelData* model = nullptr;
        std::vector<Part>        parts;
        std::vector<glm::mat4>   palette;
        glm::mat4                place{1.0f};
        bool                     dirty = true;
        std::vector<Capsule>     caps;          // the mannequin (studio space), if any
        fitzel::Mesh             mannequin;
        bool                     capsDirty = false;
    };
    Side                   m_side[2];
    std::vector<Segment>   m_segs;
    fitzel::Mesh           m_floor, m_bones;
    bool                   m_haveBones = false;
    fitzel::Shader         m_sh;
    std::unique_ptr<fitzel::RenderTarget> m_target;
    glm::mat4              m_viewProj{1.0f};
    glm::vec3              m_eye{0.0f};
    glm::mat4              m_viewM{1.0f}, m_projM{1.0f};
    std::vector<fitzel::Vertex> m_scratch;
};

// The studio has no use for a 4K map on a figure 300 pixels tall: every colour
// map is halved until it fits 1024, the other maps are dropped. Call it on a
// loader thread, so the upload is quick and the RAM stays small.
void shrinkForStudio(fitzel::ModelData& md);