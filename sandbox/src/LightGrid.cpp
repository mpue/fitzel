#include "LightGrid.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

#include <fitzel/render/Renderer.hpp>

namespace lightgrid {
namespace {

// A cap on what one Bake button press may cost. Chosen as a wall-clock promise
// rather than a memory one: at a hundred and twenty-eight rays a probe, this is
// a couple of minutes on a desktop, and a grid that takes longer than somebody
// will wait for is a grid nobody bakes twice.
constexpr int kMaxProbes = 200000;

const char kMagic[6] = {'F', 'G', 'R', 'I', 'D', '1'};

} // namespace

glm::vec3 Grid::positionOf(int ix, int iy, int iz) const {
    const glm::vec3 n(static_cast<float>(nx), static_cast<float>(ny),
                      static_cast<float>(nz));
    const glm::vec3 t((static_cast<float>(ix) + 0.5f) / n.x,
                      (static_cast<float>(iy) + 0.5f) / n.y,
                      (static_cast<float>(iz) + 0.5f) / n.z);
    return lo + t * (hi - lo);
}

std::vector<float> Grid::channel(int c) const {
    std::vector<float> out(static_cast<std::size_t>(count()) * 4, 0.0f);
    for (int i = 0; i < count(); ++i) {
        const pathtrace::ProbeSh& p = probes[static_cast<std::size_t>(i)];
        const std::size_t o = static_cast<std::size_t>(i) * 4;
        out[o + 0] = p.sh0[c];
        out[o + 1] = p.shX[c];
        out[o + 2] = p.shY[c];
        out[o + 3] = p.shZ[c];
    }
    return out;
}

Grid layout(const pathtrace::Scene& scene, const Settings& settings) {
    Grid g;
    if (scene.triangles.empty()) return g;

    constexpr float inf = std::numeric_limits<float>::max();
    glm::vec3 lo(inf), hi(-inf);
    for (const pathtrace::Triangle& t : scene.triangles) {
        lo = glm::min(lo, glm::min(t.p0, glm::min(t.p1, t.p2)));
        hi = glm::max(hi, glm::max(t.p0, glm::max(t.p1, t.p2)));
    }

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

bool save(const Grid& grid, const std::filesystem::path& file) {
    if (!grid.valid()) return false;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out) return false;

    out.write(kMagic, sizeof(kMagic));
    const int dims[3] = {grid.nx, grid.ny, grid.nz};
    out.write(reinterpret_cast<const char*>(dims), sizeof(dims));
    out.write(reinterpret_cast<const char*>(&grid.lo), sizeof(glm::vec3));
    out.write(reinterpret_cast<const char*>(&grid.hi), sizeof(glm::vec3));
    for (const pathtrace::ProbeSh& p : grid.probes) {
        const float v[12] = {p.sh0.x, p.sh0.y, p.sh0.z, p.shX.x, p.shX.y, p.shX.z,
                             p.shY.x, p.shY.y, p.shY.z, p.shZ.x, p.shZ.y, p.shZ.z};
        out.write(reinterpret_cast<const char*>(v), sizeof(v));
    }
    return static_cast<bool>(out);
}

bool load(Grid& grid, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;

    char magic[sizeof(kMagic)] = {};
    in.read(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return false;

    int dims[3] = {0, 0, 0};
    in.read(reinterpret_cast<char*>(dims), sizeof(dims));
    Grid g;
    g.nx = dims[0]; g.ny = dims[1]; g.nz = dims[2];
    if (g.nx < 2 || g.ny < 2 || g.nz < 2) return false;
    // A cap on trust as much as on memory: this file may be older than the code
    // reading it, and a corrupt header should fail rather than allocate.
    if (static_cast<long long>(g.nx) * g.ny * g.nz > kMaxProbes) return false;

    in.read(reinterpret_cast<char*>(&g.lo), sizeof(glm::vec3));
    in.read(reinterpret_cast<char*>(&g.hi), sizeof(glm::vec3));
    g.probes.resize(static_cast<std::size_t>(g.count()));
    for (pathtrace::ProbeSh& p : g.probes) {
        float v[12] = {};
        in.read(reinterpret_cast<char*>(v), sizeof(v));
        if (!in) return false;
        p.sh0 = {v[0], v[1], v[2]};
        p.shX = {v[3], v[4], v[5]};
        p.shY = {v[6], v[7], v[8]};
        p.shZ = {v[9], v[10], v[11]};
        p.valid = true;
    }
    grid = std::move(g);
    return true;
}

std::filesystem::path pathFor(const std::filesystem::path& scenePath) {
    if (scenePath.empty()) return {};
    return scenePath.parent_path() / "lightgrids" /
           (scenePath.stem().string() + ".fgrid");
}

void Runtime::upload() {
    if (!grid.valid()) {
        for (fitzel::Texture3D& t : tex) t = fitzel::Texture3D{};
        return;
    }
    for (int c = 0; c < 3; ++c)
        tex[c] = fitzel::Texture3D::create(grid.nx, grid.ny, grid.nz,
                                           grid.channel(c));
}

void Runtime::apply(fitzel::Renderer& renderer) const {
    const bool on = enabled && grid.valid() && tex[0].isValid();
    renderer.setLightGrid(on ? &tex[0] : nullptr, on ? &tex[1] : nullptr,
                          on ? &tex[2] : nullptr, grid.lo, grid.hi, intensity);
}

void Runtime::syncTo(const std::filesystem::path& scenePath,
                     fitzel::Renderer& renderer) {
    // Checked against the path every frame rather than hooked into the loader:
    // the grid belongs to the scene, and a single frame of the previous scene's
    // light lying over the new one is the kind of thing that gets hunted as a
    // lighting bug for an hour.
    const std::string key = scenePath.string();
    if (key != scene) {
        scene = key;
        grid  = Grid{};
        const std::filesystem::path f = pathFor(scenePath);
        if (!f.empty() && load(grid, f)) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%d x %d x %d baked probes loaded",
                          grid.nx, grid.ny, grid.nz);
            status = msg;
        } else {
            status.clear();
        }
        upload();
    }
    apply(renderer);
}

} // namespace lightgrid
