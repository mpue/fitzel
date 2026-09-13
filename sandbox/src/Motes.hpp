#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Shader.hpp>

#include "FrameRender.hpp"
#include "Wind.hpp"

// --- What floats in the sunlight -----------------------------------------------------
//
// Pollen, dust and the odd seed on its parachute, drifting through the air
// around the eye. Nobody sees them looking down-sun: they show when the light
// comes towards you -- a shaft through the crowns, the meadow against the low
// sun -- which is exactly what the shader does with them (forward scattering,
// the sun's shadow per mote). Carried by the one wind everything else sways
// in, so they stream the way the grass leans.
//
// A box of air that travels with the eye and wraps (what leaves on one side
// comes in on the other), faded out towards its edge: a few hundred motes
// are always the ones near enough to be seen. One instanced draw, additive
// into the HDR buffer, depth-tested against the scene.
class Motes {
public:
    bool  enabled = true;
    int   count   = 2400;
    float radius  = 18.0f;          // half the box, metres

    bool init();

    struct World {
        glm::vec3 eye{0.0f};
        std::function<float(float, float)> ground;
        float waterLevel = -1.0e4f;  // over the lake the air starts at the surface
        const wind::State* wind = nullptr;
        float weather  = 0.0f;      // 0 clear .. 1 storm (rain washes the air)
    };
    void update(float dt, const World& w);
    // `pxScale`: pixels per metre at one metre (viewport height * 0.5 *
    // proj[1][1]); a mote is never drawn smaller than a pixel, its light
    // spread over the pixel instead.
    void draw(const FrameContext& c, float pxScale);
    // Into the motion pass (PostChain::beginMotion), after draw(): marks the
    // pixels the motes lit so the temporal AA does not average them away, and
    // leaves their depth for the depth of field.
    void drawReactive(const glm::mat4& viewProj);
    int drawn() const { return static_cast<int>(m_data.size() / kFloats); }

private:
    struct Mote {
        glm::vec3 pos{0.0f};
        float ground = 0.0f;        // terrain under it, refreshed now and then
        float above  = 1.0f;        // metres over the ground
        float size   = 0.003f;      // radius, metres
        float kind   = 0.0f;        // 0 pollen, 1 seed fluff, 2 dust
        float phase  = 0.0f;
    };
    static constexpr int kFloats = 6;   // pos3 size kind phase

    void place(Mote& m, const World& w, bool anywhere);
    static float floorAt(const World& w, float x, float z) {
        return std::max(w.ground ? w.ground(x, z) : 0.0f, w.waterLevel);
    }

    std::vector<Mote>  m_motes;
    std::vector<float> m_data;
    std::mt19937       m_rng{9001u};
    float uni() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng); }
    float m_time = 0.0f;
    float m_weather = 0.0f;
    int   m_frame = 0;
    bool  m_drawnThisFrame = false;

    fitzel::Shader m_shader;
    std::uint32_t  m_vao = 0, m_inst = 0;
};
