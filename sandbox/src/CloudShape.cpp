#include "CloudShape.hpp"

#include <algorithm>
#include <cmath>

namespace cloudshape {
namespace {

// A small deterministic generator. Not std::mt19937: the same recipe has to give
// the same cloud on every machine and in every build, and a scene that stores a
// seed instead of a volume is relying on exactly that.
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed * 2654435761u + 1u : 1u) {}
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float unit() { return static_cast<float>(next() & 0xFFFFFFu) / 16777216.0f; }
    float range(float a, float b) { return a + (b - a) * unit(); }
    int   pick(int a, int b) { return a + static_cast<int>(next() % static_cast<std::uint32_t>(b - a + 1)); }
};

// What separates the three species: how high the trunk stacks and how hard it
// keeps growing once it is up there. Everything else -- budding, rasterising --
// is shared, which is the point of them being one family.
struct Build {
    int   trunk;        // blobs in the rising chain
    float r0, r1;       // trunk radius at the bottom and at the top
    float rise;         // how far up the chain climbs, in cloud widths
    float spread;       // how far the bottom blobs scatter sideways
    int   buds;         // buds per blob, per generation
};

Build buildFor(Species s) {
    switch (s) {
        // Flat and wide: almost no chain, a lot of sideways scatter. What is
        // left is a row of low mounds, which is fair-weather cumulus.
        case Species::Humilis:   return {2, 0.34f, 0.26f, 0.16f, 0.30f, 5};
        // As tall as it is wide.
        case Species::Mediocris: return {3, 0.32f, 0.22f, 0.42f, 0.20f, 5};
        // The tower: a long chain, narrowing as it climbs, and more buds so the
        // crown packs into a hard cauliflower instead of a few loose balls.
        case Species::Congestus: return {5, 0.33f, 0.17f, 0.95f, 0.16f, 6};
    }
    return {3, 0.32f, 0.22f, 0.42f, 0.20f, 5};
}

// A unit vector biased upward. `up` 0 is any direction at all, 1 is straight up.
// Buds grow the way the thermal that made them was going, so they lean up -- but
// not entirely, or the cloud becomes a column of beads with smooth flanks.
glm::vec3 budDir(Rng& rng, float up) {
    const float theta = rng.range(0.0f, 6.2831853f);
    // cos of the angle off +Y. Pulled toward 1 by `up`, and never fully
    // downward: a bud on the underside would hang below the condensation level,
    // where in the real thing there is nothing to condense.
    const float lo = glm::mix(-0.55f, 0.55f, up);
    const float c  = glm::mix(lo, 1.0f, std::pow(rng.unit(), glm::mix(1.0f, 0.5f, up)));
    const float sn = std::sqrt(std::max(0.0f, 1.0f - c * c));
    return {sn * std::cos(theta), c, sn * std::sin(theta)};
}

} // namespace

std::vector<Blob> grow(const Recipe& recipe) {
    Rng rng(recipe.seed);
    const Build b = buildFor(recipe.species);
    std::vector<Blob> blobs;

    // --- The trunk ----------------------------------------------------------
    // A chain of blobs climbing from the base, narrowing as it goes. Its lower
    // blobs are pushed sideways so the bottom of the cloud is a wide mass rather
    // than a stack of equal balls -- a cumulus is broad at the shoulder.
    //
    // Centres sit LOW: the first blob's centre is barely above the base plane, so
    // most of it hangs below and gets sliced off. That slice is the flat bottom.
    glm::vec3 lean(rng.range(-0.10f, 0.10f), 0.0f, rng.range(-0.10f, 0.10f));
    for (int i = 0; i < b.trunk; ++i) {
        const float t = (b.trunk > 1) ? static_cast<float>(i) / (b.trunk - 1) : 0.0f;
        const float r = glm::mix(b.r0, b.r1, t) * rng.range(0.88f, 1.12f);
        // The chain leans with height, the way a cloud does in wind shear.
        const glm::vec3 c(lean.x * t + rng.range(-b.spread, b.spread) * (1.0f - t),
                          b.rise * t + r * 0.45f,
                          lean.z * t + rng.range(-b.spread, b.spread) * (1.0f - t));
        blobs.push_back({c, r, 0});

        // Shoulders on the lower half. A cumulus is broad at the bottom and
        // tapers upward -- its mass is where the air came in, and the turret is
        // only what got through. A bare chain gives a column of equal balls
        // instead, which reads as a totem pole rather than as weather.
        if (t < 0.45f) {
            const int side = rng.pick(1, 2);
            for (int k = 0; k < side; ++k) {
                const float a = rng.range(0.0f, 6.2831853f);
                const float off = r * rng.range(0.55f, 1.05f);
                blobs.push_back({{c.x + std::cos(a) * off,
                                  c.y * rng.range(0.72f, 1.0f),
                                  c.z + std::sin(a) * off},
                                 r * rng.range(0.62f, 0.95f), 0});
            }
        }
    }

    // --- The buds -----------------------------------------------------------
    // Each generation buds off the last. This is the cauliflower: not detail
    // added to a surface, but the surface itself being made of smaller copies of
    // the same thing, which is what the shape actually is.
    const int levels = glm::clamp(recipe.levels, 1, 6);
    std::size_t genStart = 0, genEnd = blobs.size();
    for (int level = 1; level < levels; ++level) {
        const std::size_t nextStart = blobs.size();
        for (std::size_t i = genStart; i < genEnd; ++i) {
            const Blob parent = blobs[i];
            // Higher parents bud more steeply upward: the top of a cumulus is
            // where it is still growing, the flanks are where it has stopped.
            const float up = glm::clamp(parent.c.y / std::max(b.rise + b.r0, 1e-3f),
                                        0.0f, 1.0f);
            // ...and they bud MORE. A cumulus is not evenly cauliflowered: the
            // crown is where the thermal is still arriving and it boils, while
            // the flanks and the base have finished and are comparatively
            // smooth. Budding every blob equally is what made the first version
            // look like a bunch of grapes -- the same knobble everywhere, at the
            // same size, with no part of the cloud busier than any other.
            // The modulation goes AROUND the base count, not under it. Scaling
            // it down by vigour was tried and it compounds: three generations of
            // "a bit fewer" is a third as many blobs overall, and what came back
            // was a smooth knobbly root instead of a cauliflower. The crown
            // still boils harder than the flanks -- it just does so by budding
            // above the average rather than by the flanks budding below it.
            const float vigour = glm::mix(0.85f, 1.35f, up * up);
            int n = static_cast<int>(std::lround(
                static_cast<float>(std::max(2, b.buds - level + 1)) * vigour *
                rng.range(0.72f, 1.38f)));
            // Some blobs simply do not bud. Gaps are structure too: they are what
            // leaves a smooth flank next to a boiling one. Kept low -- this is
            // seasoning, and at the rate it was first set it ate the cloud.
            if (rng.unit() < 0.09f * (1.0f - up)) n = 0;
            for (int k = 0; k < n; ++k) {
                const glm::vec3 d = budDir(rng, glm::mix(0.35f, 0.9f, up));
                // Wide spread on the radius. Real turrets come in every size at
                // once -- a big shoulder next to fine curd -- and a tight range
                // here is what made every bump the same bump. The occasional
                // near-parent-sized one is the point of the upper end.
                const float r = parent.r * recipe.falloff * rng.range(0.70f, 1.45f);
                // How far it stands out varies too: some buds are barely
                // separate swellings, others are almost their own turret.
                const float push = recipe.budge * rng.range(0.75f, 1.15f);
                const glm::vec3 c = parent.c + d * (parent.r * push);
                blobs.push_back({c, r, level});
            }
        }
        genStart = nextStart;
        genEnd   = blobs.size();
    }
    return blobs;
}

Volume bake(const Recipe& recipe, int res) {
    Volume vol;
    res = glm::clamp(res, 16, 256);
    vol.res = res;

    std::vector<Blob> blobs = grow(recipe);
    if (blobs.empty()) return vol;

    // --- Fit the cluster into the unit cube ---------------------------------
    // The cloud fills the cube; `aspect` carries the proportion it was grown at.
    // Nothing is spent on empty air, so a flat humilis gets the same number of
    // voxels across its bulges as a tall congestus -- and the instance stretches
    // the cube back into shape when it draws it.
    float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f, maxY = -1e9f;
    for (const Blob& bl : blobs) {
        minX = std::min(minX, bl.c.x - bl.r); maxX = std::max(maxX, bl.c.x + bl.r);
        minZ = std::min(minZ, bl.c.z - bl.r); maxZ = std::max(maxZ, bl.c.z + bl.r);
        maxY = std::max(maxY, bl.c.y + bl.r);
    }
    // y = 0 is the base plane and the cloud is cut there, so the vertical extent
    // is measured from it and not from the lowest blob.
    const float wx = std::max(maxX - minX, 1e-3f);
    const float wz = std::max(maxZ - minZ, 1e-3f);
    const float w  = std::max(wx, wz);
    const float h  = std::max(maxY, 1e-3f);
    vol.aspect = h / w;

    // Leave a two-voxel margin all round. Trilinear filtering reaches half a
    // voxel past the edge, and later these cubes sit next to each other in one
    // atlas texture -- density flush against the boundary would bleed into the
    // neighbouring cloud.
    const float margin = 2.5f / static_cast<float>(res);
    const float sx = (1.0f - 2.0f * margin) / w;
    const float sy = (1.0f - 2.0f * margin) / h;
    const glm::vec3 cx((minX + maxX) * 0.5f, 0.0f, (minZ + maxZ) * 0.5f);
    for (Blob& bl : blobs) {
        bl.c.x = (bl.c.x - cx.x) * sx + 0.5f;
        bl.c.z = (bl.c.z - cx.z) * sx + 0.5f;
        bl.c.y = bl.c.y * sy + margin;
        // Radii scale with the horizontal factor: the vertical one only stretches
        // where the blobs SIT, not what they are. Squashing the blobs themselves
        // would turn the buds into lozenges and lose the roundness that the whole
        // approach is for -- the aspect is carried by the box instead.
        bl.r *= sx;
    }

    // --- Rasterise ----------------------------------------------------------
    // Splatting, not sampling: each blob writes only the voxels inside its own
    // bounding box. A cluster is a few hundred blobs against a couple of million
    // voxels, so asking every voxel about every blob would be a thousand times
    // the work for the same answer.
    const std::size_t texels = static_cast<std::size_t>(res) * res * res;
    std::vector<float> field(texels, 0.0f);
    const float inv = 1.0f / static_cast<float>(res);

    for (const Blob& bl : blobs) {
        const int x0 = std::max(0, static_cast<int>(std::floor((bl.c.x - bl.r) * res)));
        const int x1 = std::min(res - 1, static_cast<int>(std::ceil((bl.c.x + bl.r) * res)));
        const int y0 = std::max(0, static_cast<int>(std::floor((bl.c.y - bl.r) * res)));
        const int y1 = std::min(res - 1, static_cast<int>(std::ceil((bl.c.y + bl.r) * res)));
        const int z0 = std::max(0, static_cast<int>(std::floor((bl.c.z - bl.r) * res)));
        const int z1 = std::min(res - 1, static_cast<int>(std::ceil((bl.c.z + bl.r) * res)));
        const float r2 = bl.r * bl.r;
        for (int z = z0; z <= z1; ++z) {
            const float pz = (static_cast<float>(z) + 0.5f) * inv - bl.c.z;
            for (int y = y0; y <= y1; ++y) {
                const float py = (static_cast<float>(y) + 0.5f) * inv - bl.c.y;
                const float rowXZ = pz * pz + py * py;
                if (rowXZ > r2) continue;
                std::size_t idx = (static_cast<std::size_t>(z) * res + y) * res + x0;
                for (int x = x0; x <= x1; ++x, ++idx) {
                    const float px = (static_cast<float>(x) + 0.5f) * inv - bl.c.x;
                    const float d2 = rowXZ + px * px;
                    if (d2 >= r2) continue;
                    // The classic metaball kernel: 1 at the centre, 0 with zero
                    // slope at the rim, so two neighbours merge into one surface
                    // instead of showing a crease where they cross.
                    const float q = 1.0f - d2 / r2;
                    field[idx] += q * q;
                }
            }
        }
    }

    // --- Surface + flat base ------------------------------------------------
    // An iso level turns the summed field into a body with a real edge. The
    // width of the ramp is what "sharp outline" costs: too wide and the cloud
    // fogs out at the rim the way the noise version did, too narrow and the
    // silhouette shows the voxel grid. About two voxels' worth is the balance.
    // The iso level decides how much the blobs MERGE, which is the difference
    // between a cauliflower and a boiled potato. Low, and neighbouring buds fuse
    // into one smooth mass -- the metaball field is generous and any two spheres
    // within reach of each other grow a fillet. Raised, the surface sits closer
    // to each sphere's own rim and the furrow between two buds survives, which
    // is what the eye reads the relief from. 0.42 was smooth enough that adding
    // blobs made the cloud LESS structured, which is the wrong direction for a
    // knob to run in.
    const float kIso  = 0.50f;
    const float kRamp = 0.15f;
    vol.density.assign(texels, 0);
    for (int z = 0; z < res; ++z) {
        for (int y = 0; y < res; ++y) {
            // The base cut. Everything condenses at one altitude, so the bottom
            // is a plane and not a curve -- the single strongest cue that a
            // cumulus is a cumulus and not a ball of cotton. One voxel of ramp
            // keeps it from aliasing into a staircase at a shallow viewing angle.
            const float ybase = static_cast<float>(y) * inv - margin;
            const float cut = glm::clamp(ybase * static_cast<float>(res) * 0.9f,
                                         0.0f, 1.0f);
            for (int x = 0; x < res; ++x) {
                const std::size_t i = (static_cast<std::size_t>(z) * res + y) * res + x;
                const float v = glm::smoothstep(kIso - kRamp, kIso + kRamp, field[i]);
                const float d = v * cut;
                vol.density[i] = static_cast<unsigned char>(
                    glm::clamp(d, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
    return vol;
}

} // namespace cloudshape
