#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

// The waves things MAKE on open water: a boat's bow wave and the V it drags
// behind it, the rings a crate throws when it lands, a wader's ripples.
//
// The ocean (Ocean.hpp) is the sea the weather raised and knows nothing about
// what is in it. This is the other half: a small patch of water around the
// camera on which a damped wave equation runs, with whatever is in the water
// pressing its shape into it. Its height is added to the ocean's.
//
// WHY AN IMPRINT AND NOT A FORCE. A floating body pushes water aside as much as
// it is deep in it, and only CHANGES in that push make waves -- a crate at rest
// on a calm lake leaves it calm. So each frame the shape of every body in the
// water (a dent as deep as it sits) is compared with last frame's, and the
// difference is what the water feels: arriving, moving and bobbing all make
// waves, sitting still makes none. A body faster than the waves themselves
// outruns its own rings and drags them into a wedge -- a wake, from nothing but
// the equation.
//
// The patch follows the camera in whole cells, so what is on it never moves
// against the world; past its edge it fades into the open sea.
namespace ocean {

class WaveSim {
public:
    // One thing in the water this frame. `id` ties it to its last imprint.
    struct Body {
        std::uint64_t id = 0;
        glm::vec2 xz{0.0f};
        float radius = 0.5f;   // across the waterline, metres
        float depth  = 0.0f;   // how deep it sits, metres (0 = out of the water)
    };

    explicit WaveSim(int cells = 256, float cellSize = 0.5f);

    // The author's dials.
    float waveSpeed = 4.0f;   // m/s: slower waves make a wider wake behind a boat
    float damping   = 0.35f;  // per second: how soon a ring dies away
    float strength  = 1.0f;   // how hard bodies push the water

    // Keep the patch around (x, z). Moves in whole cells; heights stay put.
    void follow(glm::vec2 xz);
    // Advance by dt (fixed sub-steps inside), with these bodies in the water.
    void step(float dt, const std::vector<Body>& bodies);
    void clear();

    float heightAt(float x, float z) const;
    // Height and its slope (d/dx, d/dz) per cell, for the renderer.
    const std::vector<glm::vec4>& texels() const { return m_tex; }
    int   cells() const { return m_n; }
    float cellSize() const { return m_cell; }
    glm::vec2 origin() const;               // world xz of cell (0, 0)'s lower corner
    std::uint64_t generation() const { return m_gen; }

private:
    void imprint(const Body& b, float sign);
    void shift(int dx, int dz);

    int   m_n;
    float m_cell;
    glm::ivec2 m_originCell{0};             // world cell index of cell (0, 0)
    std::vector<float> m_h, m_v;
    std::vector<glm::vec4> m_tex;
    float m_accum = 0.0f;
    std::uint64_t m_gen = 0;
    std::unordered_map<std::uint64_t, Body> m_last;
};

} // namespace ocean
