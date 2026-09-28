#include "LightGrid.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

#include <fitzel/asset/Vfs.hpp>
#include <fitzel/render/Renderer.hpp>

namespace lightgrid {
namespace {

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
    // Through the VFS: in an exported game the grid is an archive entry, and an
    // ifstream finds nothing there -- the scene then fell back to the HDRI's
    // ambient without a word, a different colour of light than it was baked
    // with (the scaper town went yellow in the export).
    const std::vector<std::uint8_t> bytes = fitzel::vfs::read(file.generic_string());
    std::size_t at = 0;
    auto take = [&](void* dst, std::size_t n) {
        if (bytes.size() - at < n) return false;
        std::memcpy(dst, bytes.data() + at, n);
        at += n;
        return true;
    };

    char magic[sizeof(kMagic)] = {};
    if (!take(magic, sizeof(magic))) return false;
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return false;

    int dims[3] = {0, 0, 0};
    if (!take(dims, sizeof(dims))) return false;
    Grid g;
    g.nx = dims[0]; g.ny = dims[1]; g.nz = dims[2];
    if (g.nx < 2 || g.ny < 2 || g.nz < 2) return false;
    // A cap on trust as much as on memory: this file may be older than the code
    // reading it, and a corrupt header should fail rather than allocate.
    if (static_cast<long long>(g.nx) * g.ny * g.nz > kMaxProbes) return false;

    if (!take(&g.lo, sizeof(glm::vec3)) || !take(&g.hi, sizeof(glm::vec3))) return false;
    g.probes.resize(static_cast<std::size_t>(g.count()));
    for (pathtrace::ProbeSh& p : g.probes) {
        float v[12] = {};
        if (!take(v, sizeof(v))) return false;
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
