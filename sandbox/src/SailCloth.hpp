#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>   // fitzel::Vertex (the struct only; no GL here)

// Canvas the wind moves: a ship's sails and flags, modelled once, posed every
// frame.
//
// WHY NOT A CLOTH SIMULATION. The soft-body cloth (SoftBodySystem) is the real
// thing, and right for a curtain or one flag: a few hundred particles. The
// sails of a ship of the line are thirty-five pieces of canvas and forty
// thousand corners, pinned along yards that roll with the ship -- a simulation
// that size costs a frame by itself, and its pins are fixed in the world. What a
// sail does in a gale is also far more regular than what a curtain does: it is
// tied along one edge and held out by the wind, its belly breathes as the gusts
// come and go, and ripples run out to its free edges. That can be POSED rather
// than solved, from one number per corner worked out once.
//
// THE NUMBER IS HOW FAR THE CORNER IS FROM WHERE THE CLOTH IS TIED. A sail is
// tied where it touches the rest of the model -- along its yard, to its stay, at
// a flag's hoist -- so the edges that lie within reach of a spar or a rope are
// held, and every other corner gets its distance from them measured ACROSS the
// cloth (Dijkstra over the mesh, not a straight line: a corner beside the yard
// on the far side of a fold is not held by it). Each piece of cloth -- each
// separate island of the mesh -- is then posed by that distance: nothing moves
// where it is tied, the belly swells and slackens with the gusts, and ripples
// travel from the tie to the free edge the way they run down a flag. A piece
// that touches nothing is tied at its top.
//
// The modelled shape is the rest shape: a sail made billowing stays billowing,
// and the wind moves it about that shape rather than redrawing it.
namespace sail {

struct Cloth {
    // --- Built once (build) -------------------------------------------------
    // The model's corners shared where they are the same corner (position,
    // normal and UV alike), and the triangles over them: what is drawn.
    std::vector<fitzel::Vertex> rest;
    std::vector<std::uint32_t>  indices;
    // The cloth as a surface: each corner's NODE is its position, so the two
    // sides of a UV seam move as one.
    std::vector<std::uint32_t>  node;       // per corner
    std::vector<glm::vec3>      nodePos;    // per node, model space
    std::vector<std::uint32_t>  nodeTris;   // triangles over nodes, for normals
    std::vector<float>          weight;     // per node: 0 tied .. 1 the free edge
    std::vector<int>            island;     // per node
    std::vector<glm::vec4>      wave;       // per node: sin a, cos a, sin b, cos b
    std::vector<std::uint8_t>   held;       // per node: tied there (diagnostics)

    struct Island {
        glm::vec3 centroid{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};  // area-weighted, model space
        float     length  = 0.0f;  // furthest any of it is from its tie, model units
        float     wave    = 2.0f;  // ripple length, model units
        float     phase1  = 0.0f, phase2 = 0.0f, sway = 0.0f;   // running clocks
        int       nodes   = 0;
        int       seeds   = 0;     // nodes tied by touching something
    };
    std::vector<Island> islands;

    // Build from a triangle list (three corners per triangle, as models are
    // drawn) and the surface of what holds it, both in the model's own space.
    // `hold` is how close an edge must come to that surface to be tied, and
    // `unit` how many metres one model unit is (the model's stretch), so the
    // ripples are a size in the world. False when there is no cloth in it.
    bool build(const std::vector<fitzel::Vertex>& triangles,
               const std::vector<glm::vec3>& holders, float hold, float unit);

    // --- Posed every frame (pose) -------------------------------------------
    struct Air {
        glm::vec3 dir{1.0f, 0.0f, 0.0f};   // where the wind goes, model space, unit
        float     speed = 0.0f;            // m/s
        const float* gust = nullptr;       // per island: 0.35 lull .. 1.5 gust (null: 1)
    };
    // `billow` (m) is how far the belly breathes, `flutter` (m) how far the
    // free edges shake, both at a fresh gale; `dt` moves the clocks on.
    // Writes the posed corners -- position and normal -- into `out`.
    void pose(const Air& air, float billow, float flutter, float dt,
              std::vector<fitzel::Vertex>& out);

    float unit = 1.0f;   // metres per model unit

private:
    // pose()'s scratch: each node's direction at rest and posed position and
    // normal, and which corners face against their winding.
    std::vector<glm::vec3>   m_restN, m_pos, m_nrm;
    std::vector<std::int8_t> m_flip;
};

} // namespace sail
