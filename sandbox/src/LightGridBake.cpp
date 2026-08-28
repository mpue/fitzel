#include "LightGrid.hpp"

#include <algorithm>
#include <vector>

// The bake, kept out of the runtime half on purpose.
//
// Filling a grid needs the whole path tracer; sampling one needs a texture
// lookup. A shipped game only ever does the second, so this file is compiled
// into the editor and not into the player -- which is what keeps two thousand
// lines of renderer out of a binary that would never call a line of it.
namespace lightgrid {
namespace {

// Probes that landed inside solid geometry have nothing worth keeping: they saw
// the inside of a shell. Left alone they are black spots that the hardware's
// interpolation then smears into every surface near them, which reads as dirt
// along the foot of every wall. Filled from whatever valid neighbours they
// have, repeatedly, so the fill reaches a few cells into a wall.
void fillBuried(Grid& g) {
    const auto index = [&](int x, int y, int z) {
        return static_cast<std::size_t>((z * g.ny + y) * g.nx + x);
    };
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<pathtrace::ProbeSh> next = g.probes;
        bool changed = false;
        for (int z = 0; z < g.nz; ++z)
            for (int y = 0; y < g.ny; ++y)
                for (int x = 0; x < g.nx; ++x) {
                    const std::size_t i = index(x, y, z);
                    if (g.probes[i].valid) continue;
                    pathtrace::ProbeSh sum;
                    int n = 0;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int cx = x + dx, cy = y + dy, cz = z + dz;
                                if (cx < 0 || cy < 0 || cz < 0 ||
                                    cx >= g.nx || cy >= g.ny || cz >= g.nz)
                                    continue;
                                const pathtrace::ProbeSh& p =
                                    g.probes[index(cx, cy, cz)];
                                if (!p.valid) continue;
                                sum.sh0 += p.sh0;
                                sum.shX += p.shX;
                                sum.shY += p.shY;
                                sum.shZ += p.shZ;
                                ++n;
                            }
                    if (n == 0) continue;
                    const float inv = 1.0f / static_cast<float>(n);
                    next[i].sh0   = sum.sh0 * inv;
                    next[i].shX   = sum.shX * inv;
                    next[i].shY   = sum.shY * inv;
                    next[i].shZ   = sum.shZ * inv;
                    next[i].valid = true;
                    changed = true;
                }
        g.probes.swap(next);
        if (!changed) break;
    }
}

} // namespace

bool bake(Grid& grid, const pathtrace::Scene& scene, const Settings& settings,
          const std::function<bool(float)>& progress) {
    if (!grid.valid()) return false;

    std::vector<glm::vec3> points;
    points.reserve(static_cast<std::size_t>(grid.count()));
    for (int z = 0; z < grid.nz; ++z)
        for (int y = 0; y < grid.ny; ++y)
            for (int x = 0; x < grid.nx; ++x)
                points.push_back(grid.positionOf(x, y, z));

    pathtrace::BakeSettings b;
    b.rays       = std::max(8, settings.rays);
    b.maxBounces = std::max(1, settings.bounces);
    b.includeSun = false;   // see the header: the sun does not hold still

    bool cancelled = false;
    auto forward = [&](float p) {
        if (!progress) return true;
        const bool go = progress(p);
        if (!go) cancelled = true;
        return go;
    };

    grid.probes = pathtrace::bakeProbes(scene, points, b, forward);
    if (cancelled || grid.probes.size() != points.size()) {
        // A half-baked grid is worse than none: the finished half would light
        // its part of the world and the rest would go black, which reads as a
        // lighting bug rather than as an interrupted bake.
        grid.probes.clear();
        return false;
    }
    fillBuried(grid);
    return true;
}

} // namespace lightgrid
