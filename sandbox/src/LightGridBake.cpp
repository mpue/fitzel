#include "LightGrid.hpp"

#include <algorithm>
#include <cmath>
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

// Laying a grid over a harvested scene is part of the bake: it reads the
// tracer's Scene (instances and all), which a shipped game never links.
Grid layout(const pathtrace::Scene& scene, const Settings& settings) {
    Grid g;
    // Instances included: a scene can be all placed copies and no world
    // triangles at all, and its grid still has to cover it.
    glm::vec3 lo, hi;
    if (!scene.bounds(lo, hi)) return g;

    const float pad = std::max(0.0f, settings.padding);
    lo -= glm::vec3(pad);
    hi += glm::vec3(pad);
    // Vertically the grid is cut to what a surface can actually sample: from a
    // little below the lowest geometry to a little above the highest. A racing
    // world is wide and shallow, and probes far over the tarmac are probes
    // nothing will ever look up.
    hi.y = (hi.y - pad) + std::max(0.0f, settings.headroom);

    const glm::vec3 size = glm::max(hi - lo, glm::vec3(1.0f));
    const int res = std::clamp(settings.resolution, 2, 128);

    // Cells kept roughly cubic: the longest HORIZONTAL axis gets `res`, and the
    // others are scaled to match its cell size. Horizontal rather than longest
    // overall, because a scene with one tall tower in it should not spend its
    // whole budget on the tower.
    const float horizontal = std::max(size.x, size.z);
    const float cell = horizontal / static_cast<float>(res);
    g.nx = std::clamp(static_cast<int>(std::lround(size.x / cell)), 2, 256);
    g.ny = std::clamp(static_cast<int>(std::lround(size.y / cell)), 2, 64);
    g.nz = std::clamp(static_cast<int>(std::lround(size.z / cell)), 2, 256);

    // Back off uniformly rather than clipping one axis, so an over-large scene
    // gets a coarser grid rather than a lopsided one.
    while (g.nx * g.ny * g.nz > kMaxProbes) {
        g.nx = std::max(2, g.nx * 3 / 4);
        g.ny = std::max(2, g.ny * 3 / 4);
        g.nz = std::max(2, g.nz * 3 / 4);
        if (g.nx == 2 && g.ny == 2 && g.nz == 2) break;
    }

    g.lo = lo;
    g.hi = hi;
    g.probes.assign(static_cast<std::size_t>(g.count()), pathtrace::ProbeSh{});
    return g;
}


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
