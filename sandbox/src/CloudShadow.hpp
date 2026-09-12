#pragma once

#include <functional>

#include <glm/glm.hpp>

#include <fitzel/graphics/Shader.hpp>

// --- Shadows of the clouds -------------------------------------------------------
//
// The sky has marched a cumulus deck for a long time, and the ground never knew:
// the sun shone through every cloud onto a uniformly lit valley. Cloud shadows
// are what make a landscape look like it has weather -- dark patches drifting
// across the meadows, a mountainside going dim and bright again -- and, because
// they move, the largest single thing that makes it look alive from far away.
//
// Once a frame, a small fullscreen pass marches the sky's own cloud density
// (cumulus.glsl) from every point of a ground plane towards the sun into a
// 512 x 512 map covering 16 km around the eye; every lit surface looks itself
// up there (cloudshadow.glsl), sliding along the sun's ray to the plane first.
class CloudShadow {
public:
    // What the sky is drawing this frame, in the sky shader's own terms.
    struct Params {
        float     time = 0.0f;
        float     coverage = 0.7f;   // sky.frag's uCoverage (a threshold)
        float     density = 1.0f;
        float     scale = 0.001f;
        float     speed = 4.0f;
        float     bottom = 900.0f, top = 2600.0f;
        glm::vec3 sunDir{0.0f, 1.0f, 0.0f};
        glm::vec3 eye{0.0f};
        float     groundY = 0.0f;     // the plane: roughly the valley floor
    };

    ~CloudShadow();

    // Render the map. `drawQuad` draws the fullscreen quad (sky.vert's input).
    // Leaves the map bound on kUnit.
    void render(const Params& p, const std::function<void()>& drawQuad);
    // Nothing casts this frame (no cumulus, night): consumers see full sun.
    void disable();

    // Where consumers find it (FrameRender.hpp publishes this to the shaders).
    static constexpr int kUnit = 31;
    static constexpr int kRes  = 512;
    static constexpr float kSize = 16000.0f;

private:
    fitzel::Shader m_shader;
    unsigned m_fbo = 0, m_tex = 0;
    bool     m_tried = false;
};
