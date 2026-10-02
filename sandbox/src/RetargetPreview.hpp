#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/world/Model.hpp>

// The retargeting window's studio: two figures on a floor with a metre grid --
// the motion's own figure on the left, the character playing it on the right --
// lit, with their shadows on the floor (where a foot is planted shows there
// first), and the humanoid bones drawn over them. Renders into its own target;
// the panel shows it with ImGui::Image, retargetpanelcheck reads it back.
// Puts back every bit of GL state it touches, so it can run mid-frame.
class RetargetPreview {
public:
    RetargetPreview() = default;
    ~RetargetPreview() = default;
    RetargetPreview(const RetargetPreview&)            = delete;
    RetargetPreview& operator=(const RetargetPreview&) = delete;

    // The shader. Needs a live GL context; false if it did not compile.
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
    };
    // Draw at w x h and return the colour texture.
    std::uint32_t render(int w, int h, const View& v);
    // Where a studio point landed in the last render (pixels, top-left origin).
    glm::vec2 project(const glm::vec3& p) const;
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
    std::vector<fitzel::Vertex> m_scratch;
};