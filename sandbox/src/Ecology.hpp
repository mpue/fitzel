#pragma once

#include <cstdint>

#include <glm/glm.hpp>

// --- Where the forest is --------------------------------------------------------
//
// One answer to "how much forest stands here", asked by everyone who draws a
// tree or the lack of one: the tree placement near the eye, the impostor field
// out to a kilometre and a half, the far terrain's canopy colour past that, the
// forest floor under it and the grass that thins out in its shade. When each of
// those had its own rule the forest near the camera was an orchard, the one on
// the horizon a different forest, and the ring between them had none at all.
//
// The same function exists in assets/shaders/ecology.glsl, built on the same
// integer hash, so the GPU's canopy lands on the CPU's trees: uint arithmetic
// wraps identically on both sides, and the float part is a handful of
// multiply-adds. Change one, change the other.
//
// The shape of it is what real woodland looks like from a hill: stands a few
// hundred metres across with ragged edges, meadows between them, the odd
// solitary tree out in the open, forest climbing the slopes that are too steep
// to mow and thinning out towards the tree line, and none of it in the water.
namespace ecology {

struct Params {
    bool  enabled   = false;   // off: the old placement rule, for scenes before this
    float cover     = 0.45f;   // share of open, gentle ground that is forest
    float standSize = 340.0f;  // metres: how big a wood is
    float treeLine  = 750.0f;  // world height where the forest gives out
    float waterLevel = -1000.0f;
    float solitary  = 0.035f;  // density of lone trees out in the meadows
    float slopeLove = 0.9f;    // how much steeper ground favours forest over meadow
    // Where people live: discs of open land (x, z, radius), set by the host from
    // its towns. Nobody keeps a wood in the village: inside one there is no
    // stand, no forest floor and no shade on the grass, only the lone trees of
    // the gardens -- and the grass is mown (Sample::settled). A ragged rim, so
    // the gardens do not end on a circle drawn with a compass.
    static constexpr int kTowns = 8;
    int       towns = 0;
    glm::vec4 town[kTowns]{};

    // Everything that moves a tree, a forest floor or a blade of grass.
    bool operator==(const Params& o) const {
        if (enabled != o.enabled || cover != o.cover || standSize != o.standSize ||
            treeLine != o.treeLine || waterLevel != o.waterLevel || solitary != o.solitary ||
            slopeLove != o.slopeLove || towns != o.towns)
            return false;
        for (int i = 0; i < towns && i < kTowns; ++i)
            if (town[i] != o.town[i]) return false;
        return true;
    }
    bool operator!=(const Params& o) const { return !(*this == o); }
};

// How much `xz` lies in a town's open land, 0..1 (1 inside). Identical to
// ecology.glsl's ecoSettled.
float settled(const Params& p, glm::vec2 xz);

// Integer-hash value noise in [0,1). Identical to ecology.glsl.
float noise(glm::vec2 p, std::uint32_t seed);
float fbm(glm::vec2 p, int octaves, std::uint32_t seed);

struct Sample {
    float density = 0.0f;   // trees per 6 m cell, 0..1
    float forest  = 0.0f;   // 0 meadow .. 1 inside a stand (before the slope/height cuts)
    float edge    = 0.0f;   // 1 on a forest edge (where the shrubs grow)
    float settled = 0.0f;   // 1 in a town's open land (gardens, greens: mown)
};

// `h` = ground height, `ny` = the ground normal's y.
Sample sample(const Params& p, float x, float z, float h, float ny);

// ecology.glsl's uniforms, for a shader that includes it. `set(name, value)`
// is called with an int for uEcoOn and floats for the rest -- a template so a
// Shader and a Material (whose setters differ) can both be fed.
template <class SetInt, class SetFloat, class SetVec4>
void forEachUniform(const Params& p, SetInt setInt, SetFloat setFloat, SetVec4 setVec4) {
    static const char* const kTown[Params::kTowns] = {
        "uEcoTown[0]", "uEcoTown[1]", "uEcoTown[2]", "uEcoTown[3]",
        "uEcoTown[4]", "uEcoTown[5]", "uEcoTown[6]", "uEcoTown[7]"};
    const int n = p.towns < 0 ? 0 : (p.towns > Params::kTowns ? Params::kTowns : p.towns);
    setInt("uEcoTowns", n);
    for (int i = 0; i < n; ++i) setVec4(kTown[i], p.town[i]);
    setInt("uEcoOn", p.enabled ? 1 : 0);
    setFloat("uEcoCover", p.cover);
    setFloat("uEcoStandSize", p.standSize);
    setFloat("uEcoTreeLine", p.treeLine);
    setFloat("uEcoWater", p.waterLevel);
    setFloat("uEcoSolitary", p.solitary);
    setFloat("uEcoSlopeLove", p.slopeLove);
}

} // namespace ecology
