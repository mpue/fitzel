#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "fitzel/graphics/Shader.hpp"
#include "fitzel/rhi/Types.hpp"

namespace fitzel {

// What is hidden behind what, asked of the picture itself.
//
// Frustum culling leaves everything the view points at, and from a street that
// is the whole town: every house behind the first row costs its uniforms, its
// draw and its vertices to end up behind a wall. Nothing about a level says
// which those are, and asking an author to mark occluders, or baking a
// visibility set, stops being true the moment a door opens or a town is
// generated. The depth buffer already knows, for whatever the scene is, moved
// however it moved: walls, hills, trees, anything that drew solid.
//
// So each view keeps a small copy of its own last depth:
//   1. captureDepth(): the finished solid depth, reduced on the GPU to about
//      256 pixels across, each keeping the FARTHEST depth under it -- a block is
//      only as solid as its most open pixel, so a crown of leaves with sky
//      between them stops nothing -- and read back without waiting (a fence).
//   2. beginFrame(): the newest copy that has landed is moved to where THIS
//      frame's camera sees it, sample by sample, because it is a frame or three
//      old by then. What nobody saw last frame comes out as a hole, and a hole
//      counts as open: turning round the corner shows no gap, whatever the speed.
//   3. carve(): what is moving cannot hide anything. A copy a few frames old
//      still has the car where it was, so its pixels are opened up, between the
//      depths it can have been at -- the wall behind it and the cockpit round
//      the eye stay in.
//   4. occluded(): a box is hidden when its nearest point is behind the farthest
//      depth over its whole rectangle, read from a pyramid of the result.
//
// The errors all go one way. A box that is wrongly kept costs what it cost
// before; a box wrongly hidden is back as soon as the next copy lands, because
// the copy holds what really was on screen and needs no separate re-test of what
// was culled. Only the main view of a frame asks: a shadow can fall from behind
// the wall that hides its caster, and the mirror and the probe look from places
// this depth never saw.
//
// It looks after its own cost too. A view that stops getting anything out of it
// (open country, a single room) captures every eighth frame, enough to notice
// the first town coming up, and goes back to every frame the moment it hides
// something; a camera and a copy that have not changed move nothing at all.
//
// Works on any backend that can read a texture and read back a float target. Not
// in the browser yet (no read-back of a one-channel float target in WebGL2):
// occluded() there simply never says yes.
class OcclusionCuller {
public:
    OcclusionCuller() = default;
    ~OcclusionCuller();
    OcclusionCuller(const OcclusionCuller&)            = delete;
    OcclusionCuller& operator=(const OcclusionCuller&) = delete;

    // How the last frames went, for the stats window and the harnesses.
    struct Stats {
        int   tested   = 0;      // occluded() calls of the last finished frame
        int   hidden   = 0;      // ...that said yes
        int   age      = -1;     // frames between the depth in use and now, -1 none
        int   interval = 1;      // captures every this many frames
        float updateMs = 0.0f;   // CPU: moving the copy, carving, the pyramid
    };
    const Stats& stats() const { return m_stats; }

    // Start a frame of this view, before any carve() or occluded(). `viewProj` is
    // the camera's matrix without its TAA jitter: a jitter is far below a block
    // of the copy, and leaving it out is what lets a still camera skip the move.
    void beginFrame(const glm::mat4& viewProj, const glm::vec3& eye);
    // Something solid that is moving, over all the places it can have been.
    void carve(const glm::vec3& lo, const glm::vec3& hi);
    // Nothing more to carve: occluded() may be asked from here on.
    void endCarve();
    // Is this world box hidden? No before endCarve(), with no usable depth, and
    // for anything the eye plane cuts through.
    bool occluded(const glm::vec3& lo, const glm::vec3& hi) const;
    // How many frames old the depth in use is, -1 for none. A mover's trail is
    // this long.
    int sourceAge() const { return m_stats.age; }

    // The view's solid depth, now that it is drawn: a depth texture of w x h
    // holding what beginFrame's camera sees. Leaves the framebuffer, viewport and
    // render state as it found them; the bound program and vertex array are the
    // next draw's to set, as ever.
    void captureDepth(rhi::TextureHandle depth, int w, int h);

    // What captureDepth() hands over once the read-back is in: reduced depth, w x h
    // floats 0..1, row 0 at the bottom, seen through `viewProj`. Public for the
    // harness, which has no GPU to ask for the CPU half.
    void feedDepth(const float* depth, int w, int h, const glm::mat4& viewProj,
                   const glm::vec3& eye);

private:
    struct Slot {
        rhi::BufferHandle buffer;
        rhi::FenceHandle  fence;
        int               w = 0, h = 0;
        glm::mat4         viewProj{1.0f};
        glm::vec3         eye{0.0f};
        std::int64_t      frame = 0;
    };
    static constexpr int kSlots = 4;

    void harvest();
    void reproject();
    bool initGpu();
    void releaseTarget();

    // The copy in use, as it came.
    std::vector<float> m_source;
    int                m_sourceW = 0, m_sourceH = 0;
    glm::mat4          m_sourceVP{1.0f};
    glm::vec3          m_sourceEye{0.0f};
    std::int64_t       m_sourceFrame = -1;
    bool               m_sourceFresh = false;

    // This frame's camera.
    glm::mat4    m_vp{1.0f};
    glm::mat4    m_lastMovedVP{0.0f};   // the camera m_moved was made for
    glm::vec3    m_eye{0.0f};
    std::int64_t m_frame = 0;

    // The copy moved to that camera, before and after the carving, and the
    // pyramid over the result: level i has a texel per 2^i of level 0.
    int                m_w = 0, m_h = 0;
    std::vector<float> m_moved;
    std::vector<unsigned char> m_samples;
    struct Level { int w = 0, h = 0; std::vector<float> depth; };
    std::vector<Level> m_levels;
    bool               m_ready = false;
    bool               m_carved = false;

    // Whether it is earning its keep.
    int m_interval = 1;
    int m_lean     = 0;   // frames in a row that hid next to nothing

    Stats       m_stats;
    mutable int m_tested = 0, m_hidden = 0;

    // The GPU half, made on the first capture.
    Shader                 m_reduce;
    rhi::TextureHandle     m_target;
    rhi::FramebufferHandle m_targetFbo;
    rhi::VertexArrayHandle m_noVertices;
    int                    m_targetW = 0, m_targetH = 0;
    std::array<Slot, kSlots> m_slots{};
    bool                   m_gpuTried = false;
};

} // namespace fitzel
