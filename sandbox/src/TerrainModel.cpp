#include "TerrainModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include "SolidBuild.hpp"

using fitzel::AssetId;

namespace terrainmodel {

namespace {

using solidbuild::Builder;
using solidbuild::DV2;
using solidbuild::DV3;

// --- A heightmap image ------------------------------------------------------------------

// Read once (again when the file changes), as heights 0..1, row 0 the north edge.
struct Image {
    int w = 0, h = 0;
    std::vector<float> v;
    std::filesystem::file_time_type stamp{};
};

std::shared_ptr<const Image> imageOf(const std::string& path) {
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<const Image>> cache;
    if (path.empty()) return nullptr;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(path);
    if (it != cache.end() && it->second->stamp == stamp) return it->second;
    auto im = std::make_shared<Image>();
    im->stamp = stamp;
    int n = 0;
    if (stbi_is_16_bit(path.c_str())) {
        if (stbi_us* d = stbi_load_16(path.c_str(), &im->w, &im->h, &n, 1)) {
            im->v.assign(d, d + static_cast<std::size_t>(im->w) * static_cast<std::size_t>(im->h));
            for (float& x : im->v) x /= 65535.0f;
            stbi_image_free(d);
        }
    } else if (stbi_uc* d = stbi_load(path.c_str(), &im->w, &im->h, &n, 1)) {
        im->v.assign(d, d + static_cast<std::size_t>(im->w) * static_cast<std::size_t>(im->h));
        for (float& x : im->v) x /= 255.0f;
        stbi_image_free(d);
    }
    if (im->v.empty() || im->w < 2 || im->h < 2) return nullptr;
    cache[path] = im;
    return im;
}

// Bilinear, u across (west to east) and v down (north to south), both 0..1.
float sample(const Image& im, double u, double v) {
    const double x = std::clamp(u, 0.0, 1.0) * (im.w - 1), y = std::clamp(v, 0.0, 1.0) * (im.h - 1);
    const int x0 = std::min(static_cast<int>(x), im.w - 2), y0 = std::min(static_cast<int>(y), im.h - 2);
    const double fx = x - x0, fy = y - y0;
    auto at = [&](int i, int j) { return static_cast<double>(im.v[static_cast<std::size_t>(j) * im.w + i]); };
    const double a = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx;
    const double b = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx;
    return static_cast<float>(a * (1 - fy) + b * fy);
}

// --- Contours -------------------------------------------------------------------------

// Where the ground stands at `level` or higher, as closed outlines (mm, the
// print's x and y): marching squares over the grid, a saddle joined across the
// cell when its middle is high. The grid is walled round by a ring of samples
// lying on its own edge and lower than anything, so a contour that runs off the
// square comes back along the square's edge -- through every edge sample, at no
// width. Outlines run with the high ground on their left: the outer ones
// counter-clockwise, the holes in them clockwise.
std::vector<std::vector<DV2>> contours(const std::vector<float>& h, int nx, int ny, double Wmm, double Dmm,
                                       double level) {
    const int NX = nx + 2, NY = ny + 2;
    auto px = [&](int i) { return -0.5 * Wmm + Wmm * std::clamp(i, 0, nx - 1) / (nx - 1); };
    auto py = [&](int j) { return -0.5 * Dmm + Dmm * std::clamp(j, 0, ny - 1) / (ny - 1); };
    auto real = [&](int I, int J) { return I >= 1 && I <= nx && J >= 1 && J <= ny; };
    auto val = [&](int I, int J) {
        return real(I, J) ? static_cast<double>(h[static_cast<std::size_t>((J - 1) * nx + (I - 1))]) : -1e30;
    };
    auto pos = [&](int I, int J) { return DV2(px(I - 1), py(J - 1)); };
    auto in = [&](int I, int J) { return val(I, J) >= level; };

    // A crossing on each edge of the padded grid, once: horizontal edges even
    // ids, vertical odd.
    std::vector<DV2> at(static_cast<std::size_t>(2 * NX * NY));
    std::vector<int> next(static_cast<std::size_t>(2 * NX * NY), -1);
    auto cross = [&](int I0, int J0, int I1, int J1, int id) {
        const double va = val(I0, J0), vb = val(I1, J1);
        double t = (level - va) / (vb - va);
        // Never on a sample: two crossings meeting there would be one corner.
        if (real(I0, J0) && real(I1, J1)) t = std::clamp(t, 0.01, 0.99);
        at[static_cast<std::size_t>(id)] = pos(I0, J0) + (pos(I1, J1) - pos(I0, J0)) * std::clamp(t, 0.0, 1.0);
        return id;
    };
    for (int J = 0; J + 1 < NY; ++J)
        for (int I = 0; I + 1 < NX; ++I) {
            const int ci[4] = {I, I + 1, I + 1, I};
            const int cj[4] = {J, J, J + 1, J + 1};
            bool inside[4];
            for (int k = 0; k < 4; ++k) inside[k] = in(ci[k], cj[k]);
            // The cell's edges counter-clockwise, and their crossings in order:
            // +1 leaving the high ground, -1 coming onto it.
            const int eid[4] = {2 * (J * NX + I), 2 * (J * NX + I + 1) + 1, 2 * ((J + 1) * NX + I),
                                2 * (J * NX + I) + 1};
            int ids[4], kinds[4], n = 0;
            for (int k = 0; k < 4; ++k) {
                const int k1 = (k + 1) % 4;
                if (inside[k] == inside[k1]) continue;
                ids[n] = cross(ci[k], cj[k], ci[k1], cj[k1], eid[k]);
                kinds[n] = inside[k] ? +1 : -1;
                ++n;
            }
            if (n == 2) {
                const int ex = kinds[0] > 0 ? 0 : 1;
                next[static_cast<std::size_t>(ids[ex])] = ids[1 - ex];
            } else if (n == 4) {
                double mid = 0.0;
                for (int k = 0; k < 4; ++k) mid += 0.25 * val(ci[k], cj[k]);
                for (int k = 0; k < 4; ++k) {
                    if (kinds[k] < 0) continue;
                    const int pair = mid >= level ? (k + 1) % 4 : (k + 3) % 4;
                    next[static_cast<std::size_t>(ids[k])] = ids[pair];
                }
            }
        }
    std::vector<std::vector<DV2>> loops;
    std::vector<char> used(next.size(), 0);
    for (std::size_t s = 0; s < next.size(); ++s) {
        if (next[s] < 0 || used[s]) continue;
        std::vector<DV2> loop;
        for (int id = static_cast<int>(s); id >= 0 && !used[static_cast<std::size_t>(id)];
             id = next[static_cast<std::size_t>(id)]) {
            used[static_cast<std::size_t>(id)] = 1;
            const DV2 p = at[static_cast<std::size_t>(id)];
            if (loop.empty() || loop.back() != p) loop.push_back(p);
        }
        while (loop.size() > 1 && loop.front() == loop.back()) loop.pop_back();
        // Corners within a ten-thousandth of a millimetre of the line through
        // their neighbours say nothing a printer could print, and a fill would
        // make slivers of them: out (the outline stands alone, nothing else
        // shares its corners).
        for (bool again = true; again && loop.size() > 3;) {
            again = false;
            for (std::size_t i = 0; i < loop.size() && loop.size() > 3; ++i) {
                const DV2 a = loop[(i + loop.size() - 1) % loop.size()], q = loop[i], c = loop[(i + 1) % loop.size()];
                const double len = glm::length(c - a);
                const double off = len > 0.0 ? std::abs((q.x - a.x) * (c.y - a.y) - (q.y - a.y) * (c.x - a.x)) / len
                                             : glm::length(q - a);
                if (off < 1e-4) {
                    loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(i));
                    again = true;
                }
            }
        }
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    }
    return loops;
}

double signedArea(const std::vector<DV2>& l) {
    double a = 0.0;
    for (std::size_t i = 0; i < l.size(); ++i) {
        const DV2& p = l[i];
        const DV2& q = l[(i + 1) % l.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

// The outlines carried from z0 to z1 as one closed slab: walls along every
// outline (the high ground on its left, so outward), the outlines filled at
// both heights round their holes.
void slab(Builder& b, const std::vector<std::vector<DV2>>& loops, double z0, double z1) {
    std::vector<solidbuild::Ring> outerLo, outerHi, holeLo, holeHi;
    for (const std::vector<DV2>& l : loops) {
        solidbuild::Ring lo, hi;
        for (const DV2& p : l) {
            lo.push_back(b.vid({p.x, p.y, z0}));
            hi.push_back(b.vid({p.x, p.y, z1}));
        }
        for (std::size_t i = 0; i < l.size(); ++i) {
            const std::size_t j = (i + 1) % l.size();
            b.face({lo[i], lo[j], hi[j], hi[i]});
        }
        if (signedArea(l) > 0.0) {
            outerLo.push_back(lo);
            outerHi.push_back(hi);
        } else {
            holeLo.push_back(lo);
            holeHi.push_back(hi);
        }
    }
    b.region(outerHi, holeHi, true);
    b.region(outerLo, holeLo, false);
}

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

const char* zoneName(int z) {
    static const char* names[] = {"meadow", "hills", "rock", "snow"};
    return names[std::clamp(z, 0, 3)];
}

} // namespace

Params sane(const Params& in) {
    Params p = in;
    p.source       = static_cast<Source>(std::clamp(static_cast<int>(in.source), 0, static_cast<int>(Source::Count) - 1));
    p.style        = static_cast<Style>(std::clamp(static_cast<int>(in.style), 0, static_cast<int>(Style::Count) - 1));
    p.width        = std::clamp(in.width, 10.0f, 100000.0f);
    p.depth        = std::clamp(in.depth, 10.0f, 100000.0f);
    p.imageWidth   = std::clamp(in.imageWidth, 10.0f, 1.0e6f);
    p.imageRelief  = std::clamp(in.imageRelief, 1.0f, 10000.0f);
    p.printWidth   = std::clamp(in.printWidth, 20.0f, 400.0f);
    p.exaggeration = std::clamp(in.exaggeration, 0.1f, 20.0f);
    p.base         = std::clamp(in.base, 0.6f, 30.0f);
    p.resolution   = std::clamp(in.resolution, 16, 400);
    p.bands        = std::clamp(in.bands, 2, 24);
    return p;
}

glm::vec2 regionOf(const Params& in) {
    const Params p = sane(in);
    if (p.source == Source::Scene) return {p.width, p.depth};
    const std::shared_ptr<const Image> im = imageOf(p.image);
    if (!im) return {0.0f, 0.0f};
    return {p.imageWidth, p.imageWidth * static_cast<float>(im->h) / static_cast<float>(im->w)};
}

glm::vec3 bandColor(const Params& p, float h, float landLo, float landHi, bool water) {
    if (water) return p.waterColor;
    const float f = landHi > landLo ? (h - landLo) / (landHi - landLo) : 0.0f;
    switch (std::clamp(static_cast<int>(std::floor(f * 4.0f + 1e-4f)), 0, 3)) {
    case 0:  return p.lowColor;
    case 1:  return p.midColor;
    case 2:  return p.highColor;
    default: return p.peakColor;
    }
}

Model build(const Params& in, const Heights& scene) {
    const Params p = sane(in);
    Model out;
    const glm::vec2 region = regionOf(p);
    std::shared_ptr<const Image> im;
    if (p.source == Source::Image) {
        im = imageOf(p.image);
        if (!im) {
            out.ok = false;
            out.error = p.image.empty() ? "no heightmap image chosen" : "the image could not be read";
            return out;
        }
    } else if (!scene) {
        out.ok = false;
        out.error = "the scene has no terrain";
        return out;
    }
    // The grid: samples across, as many front to back as keep them square.
    const double W = region.x, D = region.y;
    const double s = p.printWidth / W;   // mm a metre
    const double Wmm = p.printWidth, Dmm = D * s;
    int nx = p.resolution;
    if (im) nx = std::min(nx, im->w);
    const int ny = std::clamp(static_cast<int>(std::lround((nx - 1) * D / W)) + 1, 2, 800);
    out.nx = nx;
    out.ny = ny;
    out.mmPerMetre = static_cast<float>(s);
    out.heights.resize(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny));
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const double u = static_cast<double>(i) / (nx - 1), v = static_cast<double>(j) / (ny - 1);
            float h;
            if (im) {
                h = p.imageRelief * sample(*im, u, 1.0 - v);   // the image's top row is the north edge
            } else {
                const double x = p.centreX - 0.5 * W + W * u;
                const double z = p.centreZ + 0.5 * D - D * v;   // the print's +y is the scene's -z
                h = scene(static_cast<float>(x), static_cast<float>(z));
            }
            out.heights[static_cast<std::size_t>(j * nx + i)] = h;
        }
    const auto [mn, mx] = std::minmax_element(out.heights.begin(), out.heights.end());
    out.lo = *mn;
    out.hi = *mx;
    // A lake where the ground dips below the water; the model's floor its surface.
    const bool lake = p.water && p.waterLevel > out.lo;
    const double zero = lake ? std::min(static_cast<double>(p.waterLevel), static_cast<double>(out.hi)) : out.lo;
    const double k = s * p.exaggeration;
    auto zOf = [&](double h) { return p.base + std::max(0.0, h - zero) * k; };
    const double landLo = zero, landHi = std::max(static_cast<double>(out.hi), zero + 1e-3);

    if (p.style == Style::Relief) {
        Builder b;
        std::vector<int> top(out.heights.size());
        auto at = [&](int i, int j) {
            return DV3(-0.5 * Wmm + Wmm * i / (nx - 1), -0.5 * Dmm + Dmm * j / (ny - 1),
                       zOf(out.heights[static_cast<std::size_t>(j * nx + i)]));
        };
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) top[static_cast<std::size_t>(j * nx + i)] = b.vid(at(i, j));
        auto T = [&](int i, int j) { return top[static_cast<std::size_t>(j * nx + i)]; };
        // The ground, two triangles a cell, cut along the diagonal that bends least.
        for (int j = 0; j + 1 < ny; ++j)
            for (int i = 0; i + 1 < nx; ++i) {
                const int a = T(i, j), bb = T(i + 1, j), c = T(i + 1, j + 1), d = T(i, j + 1);
                const double dac = std::abs(b.verts[static_cast<std::size_t>(a)].z - b.verts[static_cast<std::size_t>(c)].z);
                const double dbd = std::abs(b.verts[static_cast<std::size_t>(bb)].z - b.verts[static_cast<std::size_t>(d)].z);
                if (dac <= dbd) {
                    b.face({a, bb, c});
                    b.face({a, c, d});
                } else {
                    b.face({a, bb, d});
                    b.face({bb, c, d});
                }
            }
        // Four walls down to the bottom, a strip of upright quads under the
        // samples (a wall filled as one face would make slivers wherever the
        // land runs on in a straight line), and the bottom under them.
        auto down = [&](int i, int j) {
            const DV3 v = b.verts[static_cast<std::size_t>(T(i, j))];
            return b.vid({v.x, v.y, 0.0});
        };
        solidbuild::Ring bottom;   // counter-clockwise from above
        for (int i = 0; i + 1 < nx; ++i) {
            b.face({down(i, 0), down(i + 1, 0), T(i + 1, 0), T(i, 0)});
            bottom.push_back(down(i, 0));
        }
        for (int j = 0; j + 1 < ny; ++j) {
            b.face({down(nx - 1, j), down(nx - 1, j + 1), T(nx - 1, j + 1), T(nx - 1, j)});
            bottom.push_back(down(nx - 1, j));
        }
        for (int i = nx - 1; i > 0; --i) {
            b.face({down(i, ny - 1), down(i - 1, ny - 1), T(i - 1, ny - 1), T(i, ny - 1)});
            bottom.push_back(down(i, ny - 1));
        }
        for (int j = ny - 1; j > 0; --j) {
            b.face({down(0, j), down(0, j - 1), T(0, j - 1), T(0, j)});
            bottom.push_back(down(0, j));
        }
        b.region({bottom}, {}, false);
        out.ok = b.error.empty();
        out.error = b.error;
        out.volume = solidbuild::volume(b);
        out.parts.push_back({"Terrain", p.reliefColor, solidbuild::toEditMesh(b)});
        // Where filament changes colour it as the terraces would be.
        if (lake) out.changes.push_back({static_cast<float>(zOf(zero)) + 0.2f, "meadow green (blue below: the lakes)"});
        for (int zn = 1; zn < 4; ++zn)
            out.changes.push_back({static_cast<float>(zOf(landLo + (landHi - landLo) * zn / 4.0)), zoneName(zn)});
    } else {
        // Terraces: band 0 the base (the water, with a lake) under everything,
        // then the land split evenly from its lowest to its highest ground.
        const int n = p.bands;
        std::vector<double> L(static_cast<std::size_t>(n + 1));
        L[0] = zero;
        L[static_cast<std::size_t>(n)] = landHi;
        if (lake) {
            for (int b = 1; b < n; ++b) L[static_cast<std::size_t>(b)] = landLo + (landHi - landLo) * (b - 1) / (n - 1);
        } else {
            for (int b = 1; b < n; ++b) L[static_cast<std::size_t>(b)] = landLo + (landHi - landLo) * b / n;
        }
        for (int band = 0; band < n; ++band) {
            Builder b;
            const double z0 = band == 0 ? 0.0 : zOf(L[static_cast<std::size_t>(band)]);
            const double z1 = zOf(L[static_cast<std::size_t>(band + 1)]);
            if (z1 - z0 < 1e-3) continue;
            std::vector<std::vector<DV2>> loops;
            if (band == 0) {
                loops.push_back({{-0.5 * Wmm, -0.5 * Dmm}, {0.5 * Wmm, -0.5 * Dmm}, {0.5 * Wmm, 0.5 * Dmm}, {-0.5 * Wmm, 0.5 * Dmm}});
            } else {
                loops = contours(out.heights, nx, ny, Wmm, Dmm, L[static_cast<std::size_t>(band)]);
            }
            if (loops.empty()) continue;
            slab(b, loops, z0, z1);
            const bool water = band == 0 && lake;
            const glm::vec3 col = bandColor(p, static_cast<float>(L[static_cast<std::size_t>(band)]),
                                            static_cast<float>(landLo), static_cast<float>(landHi), water);
            out.levels.push_back(static_cast<float>(L[static_cast<std::size_t>(band)]));
            out.levelColors.push_back(col);
            if (!b.error.empty() && out.ok) {
                out.ok = false;
                out.error = "band " + std::to_string(band + 1) + ": " + b.error;
            }
            out.volume += solidbuild::volume(b);
            char name[32];
            std::snprintf(name, sizeof name, "Band %d", band + 1);
            out.parts.push_back({name, col, solidbuild::toEditMesh(b)});
        }
    }
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const Part& part : out.parts)
        for (const glm::vec3& v : part.mesh.verts) {
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
    if (!out.parts.empty()) out.size = 1000.0f * glm::vec3(hi.x - lo.x, hi.z - lo.z, hi.y - lo.y);
    return out;
}

AssetId ensureMaterial(std::vector<MaterialDef>& mats, const glm::vec3& c) {
    char name[64];
    std::snprintf(name, sizeof name, "Terrain #%02X%02X%02X", toByte(c.r), toByte(c.g), toByte(c.b));
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = c;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = c;
    md.reflectivity = 0.0f;
    md.roughness    = 0.6f;   // printed plastic: a dull sheen
    mats.push_back(md);
    return md.assetId;
}

std::string autoName(const Params& in) {
    const Params p = sane(in);
    const char* kind = p.style == Style::Relief ? "Terrain relief" : "Terrain contours";
    if (p.source == Source::Image) return std::string(kind) + " " + std::filesystem::path(p.image).filename().string();
    char b[96];
    std::snprintf(b, sizeof b, "%s %.0f m", kind, p.width);
    return b;
}

std::vector<Entity> entities(const Params& in, const Model& m, std::vector<MaterialDef>& mats, int& counter,
                             const glm::vec3& at) {
    const Params p = sane(in);
    if (m.parts.empty()) return {};
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const Part& part : m.parts)
        for (const glm::vec3& v : part.mesh.verts) {
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
    const glm::vec3 mid = 0.5f * (lo + hi);
    std::vector<Entity> out;
    Entity root;
    root.type = EntityType::Empty;
    root.name = autoName(p);
    root.id = counter++;
    root.parent = -1;
    // The root in the middle of the model, so its pick box is the model's: `at`
    // is where its bottom stands.
    root.half = glm::max(0.5f * (hi - lo), glm::vec3(1e-4f));
    root.localCenter = root.center = at + glm::vec3(0.0f, root.half.y, 0.0f);
    {
        auto tc = std::make_unique<TerrainModelComponent>();
        tc->params = p;
        root.components.items.push_back(std::move(tc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;
    const glm::vec3 rootC = out.front().center;
    for (const Part& part : m.parts) {
        EditMesh mesh = part.mesh;
        for (glm::vec3& v : mesh.verts) v -= mid;
        const glm::vec3 shift = editmesh::recenter(mesh);
        glm::vec3 mn, mx;
        mesh.bounds(mn, mx);
        Entity e;
        e.type = EntityType::Box;
        e.name = part.name;
        e.id = counter++;
        e.parent = rootId;
        e.half = glm::max(0.5f * (mx - mn), glm::vec3(1e-4f));
        e.localCenter = shift;
        e.center = rootC + shift;
        auto mc = std::make_unique<MeshComponent>();
        mc->mesh = std::move(mesh);
        mc->touch();
        e.components.items.push_back(std::move(mc));
        auto mt = std::make_unique<MaterialComponent>();
        mt->material = ensureMaterial(mats, part.color);
        e.components.items.push_back(std::move(mt));
        auto pc = std::make_unique<PhysicsComponent>();
        pc->dynamic = false;
        e.components.items.push_back(std::move(pc));
        out.push_back(std::move(e));
    }
    return out;
}

// --- Persistence -------------------------------------------------------------------

namespace {
nlohmann::json colJson(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
glm::vec3 jsonCol(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
} // namespace

void saveParams(nlohmann::json& j, const Params& p) {
    j["source"]       = static_cast<int>(p.source);
    j["centreX"]      = p.centreX;
    j["centreZ"]      = p.centreZ;
    j["width"]        = p.width;
    j["depth"]        = p.depth;
    j["image"]        = p.image;
    j["imageWidth"]   = p.imageWidth;
    j["imageRelief"]  = p.imageRelief;
    j["printWidth"]   = p.printWidth;
    j["exaggeration"] = p.exaggeration;
    j["base"]         = p.base;
    j["resolution"]   = p.resolution;
    j["style"]        = static_cast<int>(p.style);
    j["bands"]        = p.bands;
    j["water"]        = p.water;
    j["waterLevel"]   = p.waterLevel;
    j["reliefColor"]  = colJson(p.reliefColor);
    j["waterColor"]   = colJson(p.waterColor);
    j["lowColor"]     = colJson(p.lowColor);
    j["midColor"]     = colJson(p.midColor);
    j["highColor"]    = colJson(p.highColor);
    j["peakColor"]    = colJson(p.peakColor);
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;
    p.source       = static_cast<Source>(j.value("source", static_cast<int>(d.source)));
    p.centreX      = j.value("centreX", d.centreX);
    p.centreZ      = j.value("centreZ", d.centreZ);
    p.width        = j.value("width", d.width);
    p.depth        = j.value("depth", d.depth);
    p.image        = j.value("image", d.image);
    p.imageWidth   = j.value("imageWidth", d.imageWidth);
    p.imageRelief  = j.value("imageRelief", d.imageRelief);
    p.printWidth   = j.value("printWidth", d.printWidth);
    p.exaggeration = j.value("exaggeration", d.exaggeration);
    p.base         = j.value("base", d.base);
    p.resolution   = j.value("resolution", d.resolution);
    p.style        = static_cast<Style>(j.value("style", static_cast<int>(d.style)));
    p.bands        = j.value("bands", d.bands);
    p.water        = j.value("water", d.water);
    p.waterLevel   = j.value("waterLevel", d.waterLevel);
    p.reliefColor  = jsonCol(j.value("reliefColor", nlohmann::json()), d.reliefColor);
    p.waterColor   = jsonCol(j.value("waterColor", nlohmann::json()), d.waterColor);
    p.lowColor     = jsonCol(j.value("lowColor", nlohmann::json()), d.lowColor);
    p.midColor     = jsonCol(j.value("midColor", nlohmann::json()), d.midColor);
    p.highColor    = jsonCol(j.value("highColor", nlohmann::json()), d.highColor);
    p.peakColor    = jsonCol(j.value("peakColor", nlohmann::json()), d.peakColor);
    p = sane(p);
}

namespace {
// Scene files name components by type id; this lets a saved model come back as
// one, parameters and all.
struct RegisterTerrainModel {
    RegisterTerrainModel() {
        components::registerType({"terrainModel", "Terrain model",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<TerrainModelComponent>()); },
            /*addable=*/false});
    }
} g_registerTerrainModel;
} // namespace

} // namespace terrainmodel
