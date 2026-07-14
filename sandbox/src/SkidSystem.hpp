#pragma once

#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>

namespace fitzel {
class Shader;
class PhysicsWorld;
class Renderer;
}

// Tyre skid marks: a growing, dark, alpha-blended ribbon laid on the ground under
// each wheel while it slips -- locked braking, wheelspin, or a lateral drift. The
// geometry is built from each wheel's Jolt contact point (so it hugs terrain and
// roads alike), one trail per wheel; a strip breaks when the wheel lifts off or
// stops slipping and a fresh one begins on the next slip. Buffers are capped so a
// long play session can't grow them without bound.
class SkidSystem {
public:
    explicit SkidSystem(fitzel::Shader& lit);

    // Sample all four wheels and extend the skid ribbons. Call once per frame,
    // right AFTER physics->step() -- contact/slip is only valid then. A no-op when
    // there is no vehicle or skids are disabled.
    void update(const fitzel::PhysicsWorld& physics);

    // Draw the accumulated skid ribbons (alpha-blended, laid just above ground).
    void render(fitzel::Renderer& renderer);

    // Drop all skid geometry (call on Play start and stop).
    void clear();

    // --- Tunables (editor) ---------------------------------------------------
    bool  enabled    = true;
    float slipThresh = 0.45f;  // slip magnitude before a mark is laid
    float markHalfW  = 0.16f;  // half tyre-print width (m)
    float opacity    = 0.55f;  // strip alpha
    float minStep    = 0.12f;  // min contact travel before adding a segment (m)

private:
    struct Trail {
        std::vector<fitzel::Vertex> verts; // triangle soup (non-indexed)
        fitzel::Mesh                mesh;
        glm::vec3 prevL{0.0f}, prevR{0.0f};
        bool active = false; // laying a continuous strip right now
        bool dirty  = false; // verts changed -> re-upload before draw
    };
    Trail            m_trail[4];
    fitzel::Material m_mat;

    // Append one ribbon segment (two up-facing triangles) bridging the previous
    // rung to the new left/right edge; starts a fresh strip if none is active.
    void extend(Trail& t, const glm::vec3& pos, const glm::vec3& normal,
                const glm::vec3& lateral);
};
