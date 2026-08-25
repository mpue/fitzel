#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

// The shape of a cumulus, as a baked density volume.
//
// This is the modelling half of the cloud system and it holds no GL state, reads
// no files and knows nothing about the sky it will hang in -- the same split
// RoadSide keeps from RoadSystem. It turns a recipe into a cube of density; what
// that cube is drawn with is CloudField's business.
//
// WHY A CLUSTER OF SPHERES, and not noise. A cumulus is defined by three things
// that a thresholded noise field cannot produce, however it is tuned:
//
//   * it is DETACHED -- one cloud, standing on its own with sky around it. A
//     threshold over a continuous field gives connected regions, always.
//   * it has a SHARP OUTLINE. Noise fades; an edge has to be built.
//   * it is a CAULIFLOWER -- bulges growing out of bulges. Eroding a smooth
//     shape carves structure OUT of it, which is the opposite operation and
//     reads as weathering, not as growth.
//
// Spheres give all three for free. A cloud here is one blob with smaller blobs
// budding off it, and smaller ones off those, three or four generations deep --
// which is what a cumulus physically IS: thermals rising into a cloud and each
// one pushing a new turret out of its surface. Every bud is a rounded surface of
// its own, so every bud gets its own lit crown and its own shaded underside, and
// that is the whole read of the reference photograph.
//
// The flat base is not modelled, it is CUT: everything condenses at one altitude
// (the lifting condensation level), which is why a field of cumulus looks like
// it is standing on a sheet of glass. So the blobs are placed to hang below the
// base and the volume is sliced off there.
namespace cloudshape {

// The three cumulus species this covers, which differ only in how tall they are
// allowed to grow before they stop -- one rule set, three sets of numbers.
//
//   Humilis    -- fair weather. Wider than tall, barely any build. The flat
//                 scattered puffs of a calm afternoon.
//   Mediocris  -- as tall as it is wide, with real turrets. The default.
//   Congestus  -- towering. Stacked turrets, hard cauliflower crown, the shape
//                 in the reference photo.
enum class Species { Humilis, Mediocris, Congestus };

inline const char* speciesName(Species s) {
    switch (s) {
        case Species::Humilis:   return "Humilis";
        case Species::Mediocris: return "Mediocris";
        case Species::Congestus: return "Congestus";
    }
    return "Cumulus";
}

// What to grow. Everything else about a cloud -- where it hangs, how big it is
// in metres, which way it faces -- belongs to the instance, not to the shape.
struct Recipe {
    Species       species = Species::Mediocris;
    std::uint32_t seed    = 1;

    // How many generations of buds. 3 is lumpy, 4 is a cauliflower, 5 is a
    // cauliflower whose finest bumps are smaller than a voxel -- so this is
    // bounded by the bake resolution, not by taste (see bake()).
    int levels = 4;

    // How hard the buds push out of their parent. 0 leaves them buried and the
    // cloud is a smooth lump; 1 puts their centres on the parent's surface, so
    // each one stands half proud. Above that they start to detach.
    float budge = 0.98f;

    // How much smaller each generation is than the last. Around 0.5 is what a
    // real cauliflower does; higher makes a mound of near-equal balls, lower
    // makes a smooth parent with a light dusting of pimples.
    float falloff = 0.52f;
};

// A baked cloud: density on a cube grid, plus what shape that cube stands for.
//
// The cloud always FILLS the cube -- it is normalised into it -- and `aspect`
// says how tall the cube is against its width in the world. That way none of the
// resolution is spent on empty air, and a flat humilis gets just as many voxels
// across its bulges as a towering congestus does. The instance stretches the
// cube back into proportion when it draws it.
struct Volume {
    int  res    = 0;    // voxels per side
    float aspect = 1.0f; // world height / world width
    // Density, one byte per voxel, indexed [(z * res + y) * res + x].
    // x/z are horizontal, y is up, y = 0 is the flat base.
    std::vector<unsigned char> density;

    bool valid() const {
        return res > 0 &&
               density.size() == static_cast<std::size_t>(res) * res * res;
    }
    // Byte at a voxel, 0 outside the grid.
    unsigned char at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= res || y >= res || z >= res) return 0;
        return density[(static_cast<std::size_t>(z) * res + y) * res + x];
    }
};

// Grow the cluster and rasterise it. `res` is voxels per side; 96 to 128 is the
// useful range -- below that the finest generation of buds is smaller than a
// voxel and the cauliflower turns back into a lump, above it the bake time and
// the atlas grow faster than the picture improves.
//
// Deterministic in `recipe.seed`: the same recipe always gives the same cloud,
// which is what lets a scene store a seed instead of four megabytes of volume.
Volume bake(const Recipe& recipe, int res);

// The blobs a recipe grows, without rasterising them -- exposed for the check
// tool, which wants to be able to say how many there are and how small they got,
// and for tests. Positions are in the normalised cube (0..1 on x/z, 0 at the
// base on y); `r` is in the same units.
struct Blob {
    glm::vec3 c;
    float     r;
    int       level;  // 0 = trunk, 1 = its buds, ...
};
std::vector<Blob> grow(const Recipe& recipe);

} // namespace cloudshape
