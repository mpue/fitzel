#pragma once

#include <deque>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>

namespace fitzel {
class Shader;
class Renderer;
}

// Contrail ("Kondensstreifen"): a soft, camera-facing ribbon that streams behind
// a racing craft and dissolves over a second or two. One trail per craft id
// (its entity id). Modelled on SkidSystem -- a strip of recent world points is
// turned into a triangle ribbon each frame -- but this one lives in the air,
// always billboards toward the camera (so the chase cam never sees it edge-on),
// and tapers to nothing as its points age out.
class TrailSystem {
public:
    explicit TrailSystem(fitzel::Shader& lit);

    // Record a craft's current world position. Call once per frame per craft
    // while it races; a new point is only laid once it has moved `minStep`.
    void emit(int id, const glm::vec3& pos);

    // Age every trail, drop spent points and rebuild the camera-facing ribbons.
    // Trails that fully age out are forgotten.
    void update(float dt, const glm::vec3& camPos);

    // Draw all ribbons (translucent, laid straight into world space).
    void render(fitzel::Renderer& renderer);

    void drop(int id) { m_trails.erase(id); } // forget one craft's trail
    void clear() { m_trails.clear(); }        // drop all (Play start/stop)

    // --- Tunables (editor) ---------------------------------------------------
    bool  enabled = true;
    float life    = 1.6f;   // seconds a point lives before it vanishes
    float width   = 0.30f;  // head half-width (m); tapers to 0 down the tail
    float minStep = 0.7f;   // min spacing between recorded points (m)
    float opacity = 0.5f;   // ribbon alpha
    float glow    = 2.0f;   // emissive strength -> self-illuminated, not dimmed by light
    glm::vec3 color{0.90f, 0.95f, 1.0f}; // faint cool white

private:
    struct Pt { glm::vec3 pos; float age; };
    struct Trail {
        std::deque<Pt>              pts;
        std::vector<fitzel::Vertex> verts; // triangle soup (non-indexed, double-sided)
        fitzel::Mesh                mesh;
    };
    std::unordered_map<int, Trail> m_trails;
    fitzel::Material               m_mat;
};
