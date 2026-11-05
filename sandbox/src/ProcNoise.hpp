#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// Noise for the procedural graphs: smooth random fields over space, which the
// Noise node moves points by, Select points picks them by and Rock texture
// paints with. Every one is a pure function of the position and a seed -- the
// same point gives the same number on every cook and every machine -- so a
// boulder keeps its shape when the scene is opened again, and two corners
// that sit at the same place always move the same way.
//
// Sizes are in metres, never frequencies: `size` is how far apart the bumps
// of the first octave are, which is the number one can judge by looking.
namespace proc::noise {

// Gradient noise (Perlin's improved kind): about -1..1, smooth, zero at every
// whole-number point. The gradients are hashed from the lattice point and the
// seed rather than looked up in a table, so every seed is its own pattern.
float perlin(const glm::vec3& p, int seed);

// Value noise: a random number at every lattice point, blended smoothly. 0..1.
float value(const glm::vec3& p, int seed);

// Cellular noise (Worley): one point scattered in every unit cell, `jitter`
// of the way from its middle; how far `p` is from the nearest of them and the
// next nearest, which cell the nearest is in (a number to colour it by), and
// where that point is.
struct Cells {
    float         f1 = 0.0f, f2 = 0.0f;
    std::uint32_t id = 0;
    glm::vec3     at{0.0f};
};
Cells cells(const glm::vec3& p, int seed, float jitter = 1.0f);

// The same number in [0, 1) for the same cell id and salt.
float cellRandom(std::uint32_t id, std::uint32_t salt);

// The kinds a fractal sums. Stored by index, so new ones are only ever appended.
enum class Kind {
    Perlin = 0,   // rolling hills
    Ridged,       // sharp crests between smooth valleys: weathered ridges
    Billow,       // round lumps with creases between them
    Cells,        // a dome over every cell: pebbles, cobbles, scales
    Cracks,       // flat, with lines sunk where the cells meet: crazed mud, cracks
    Value,        // blotchy, softer than Perlin
};
const std::vector<std::string>& kindLabels();

// Octaves of one kind added up, each `lacunarity` times finer and `roughness`
// times weaker than the one before. `warp` bends the position by a coarser
// noise first (in units of `size`), which is what turns a regular lattice of
// bumps into something that looks grown rather than printed.
struct Fractal {
    Kind      kind       = Kind::Perlin;
    float     size       = 2.0f;    // metres between the features of the first octave
    int       octaves    = 4;
    float     roughness  = 0.5f;
    float     lacunarity = 2.0f;
    float     warp       = 0.0f;
    glm::vec3 offset{0.0f};          // the pattern slid through space, in metres
    int       seed       = 1;
    float     crackWidth = 0.1f;     // Cracks: how wide a line is, as a share of a cell
};
// About -1..1 for every kind except Cracks: 0 on the cells, down to -1 along
// the lines between them -- so a crack only ever goes in.
float fractal(const Fractal& f, const glm::vec3& p);

} // namespace proc::noise
