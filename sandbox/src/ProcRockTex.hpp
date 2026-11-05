#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "ProcGraph.hpp"

// Rock texture, the painting half: a rock's maps painted straight onto its own
// surface rather than tiled over it. Every face is given coordinates of its own
// (Smart UV project, packed onto one tile), and every texel of the tile is
// worked out from where it lies on the rock in 3D -- so the pattern runs on
// across the seams between the pieces of the tile, and the painting knows what
// a tiling texture cannot: which way the surface faces (lichen where the sky
// is, moss where water stays), whether it is an edge (worn light) or a hollow
// (dirt), and how close to the ground it is (earth splashed up the foot).
//
// No GPU and no files: this makes pixels. The node puts them in its stream
// (proc::Bake), and the editor makes a library material of them.
namespace proc::rock {

// The rocks it knows. Stored by index, so new ones are only ever appended.
enum class Rock { Granite = 0, Sandstone, Limestone, Basalt, Slate };
const std::vector<std::string>& rockLabels();

struct Look {
    int       rock   = 0;
    glm::vec3 tint{1.0f};      // multiplies the colour
    float     grain  = 0.012f; // metres: crystals, sand, pits
    float     bumps  = 1.0f;   // how strongly the normal map stands out
    float     layers = 0.5f;   // bands of sediment (the rock decides how much they show)
    float     cracks = 0.3f;
    float     wear   = 0.5f;   // edges rubbed light
    float     dirt   = 0.5f;   // in the hollows, down the sides, up from the ground
    float     lichen = 0.3f;
    float     moss   = 0.2f;
    int       size   = 1024;   // texels along each side of the maps
    int       seed   = 1;
};

// Give every face of `m` coordinates of its own and paint the maps for them.
// The same mesh and look give the same maps and coordinates back from a cache
// without painting again -- a graph is cooked several times a frame while its
// window is open. `hash` gets what they were made from.
std::shared_ptr<const BakedMaps> bake(EditMesh& m, const Look& look, std::uint64_t* hash = nullptr);

// The height (metres out of the surface) the normal map is made from, at a
// point facing `n` with that convexity and that share of the height above
// the bottom. For proccheck, which reads the normal map back the way the
// shader does and compares.
float heightAt(const Look& look, const glm::vec3& p, const glm::vec3& n, float convex, float low);

} // namespace proc::rock
