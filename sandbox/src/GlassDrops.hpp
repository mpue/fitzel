#pragma once
// Water running down panes of glass: one LensDrops film per object with a
// Glass drops component, laid on the object's box instead of on the camera.
// Beads land at the component's rate, some of them run, the film thins away;
// lit.frag's glass reads the result (uGlassDrops) and bends what is behind it
// through every drop. The frame's state only -- Play starts with dry glass.
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "SceneTypes.hpp"

namespace fitzel { class AssetDatabase; class Mesh; class Texture; }

// Where a pane's water is and how lit.frag reads it.
struct GlassDropsView {
    const fitzel::Texture* tex = nullptr;   // R wet thickness, G dried (film size)
    glm::vec3 origin{0.0f};    // the pane's top left corner (world)
    glm::vec3 du{0.0f};        // world -> u: u = dot(p - origin, du)
    glm::vec3 dv{0.0f};        // world -> v, v running DOWN the pane
    glm::vec2 texel{0.0f};     // 1 / the texture's size
    float     height = 0.0f;   // world units of drop height per unit of thickness
    float     reach  = 0.0f;   // how far behind the glass a drop's bend lands
};

class GlassDrops {
public:
    GlassDrops();
    ~GlassDrops();
    GlassDrops(const GlassDrops&)            = delete;
    GlassDrops& operator=(const GlassDrops&) = delete;

    // Once a frame, before the scene is submitted. Leaves the framebuffer, the
    // viewport and the blend/depth state as it found them.
    void update(const std::vector<Entity>& entities, float dt, fitzel::Mesh& fsQuad,
                fitzel::AssetDatabase& assets);
    void clear();
    // Object `id`'s water, or null when it has none.
    const GlassDropsView* of(int id) const;

private:
    struct Pane;
    std::unordered_map<int, std::unique_ptr<Pane>> m_panes;
    std::mt19937 m_rng{77u};
};
