#include "TreeGlb.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <fitzel/asset/Vfs.hpp>

namespace treeglb {

namespace {

// --- Built-in textures ------------------------------------------------------------

std::uint32_t hash2(int x, int y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Value noise that tiles with period (px, py) lattice cells.
float tileNoise(float x, float y, int px, int py, std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    auto v = [&](int i, int j) {
        const int a = ((i % px) + px) % px, b = ((j % py) + py) % py;
        return static_cast<float>(hash2(a, b, seed) & 0xffffu) / 65535.0f;
    };
    const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = v(x0, y0) + (v(x0 + 1, y0) - v(x0, y0)) * sx;
    const float b = v(x0, y0 + 1) + (v(x0 + 1, y0 + 1) - v(x0, y0 + 1)) * sx;
    return a + (b - a) * sy;
}

void pngWrite(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(ctx);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

Image encode(std::vector<std::uint8_t> rgba, int w, int h) {
    Image im;
    im.w = w; im.h = h; im.mime = "image/png";
    stbi_write_png_to_func(pngWrite, &im.bytes, w, h, 4, rgba.data(), w * 4);
    im.rgba = std::move(rgba);
    return im;
}

std::uint8_t to8(float v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Transparent texels take the colour of the nearest opaque one, so the mip
// chain does not pull a dark fringe into every cut-out edge.
void bleed(std::vector<std::uint8_t>& px, int w, int h) {
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<std::uint8_t> src = px;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                std::uint8_t* d = &px[static_cast<std::size_t>((y * w + x) * 4)];
                if (src[static_cast<std::size_t>((y * w + x) * 4 + 3)] > 0 || d[3] > 0) continue;
                int r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        const std::uint8_t* s = &src[static_cast<std::size_t>((yy * w + xx) * 4)];
                        if (s[3] == 0 && !(s[0] | s[1] | s[2])) continue;
                        r += s[0]; g += s[1]; b += s[2]; ++n;
                    }
                if (n > 0) {
                    d[0] = static_cast<std::uint8_t>(r / n);
                    d[1] = static_cast<std::uint8_t>(g / n);
                    d[2] = static_cast<std::uint8_t>(b / n);
                }
            }
    }
}

float segDist(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay;
    const float t = std::clamp(((px - ax) * vx + (py - ay) * vy) / std::max(vx * vx + vy * vy, 1e-6f), 0.0f, 1.0f);
    const float dx = px - (ax + vx * t), dy = py - (ay + vy * t);
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

Image builtinBark() {
    const int w = 256, h = 512;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w * h * 4));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(w);
            const float v = static_cast<float>(y) / static_cast<float>(h);
            // Furrows: long in v, narrow in u, wandering a little.
            const float warp = tileNoise(u * 6.0f, v * 3.0f, 6, 3, 7u) * 0.6f;
            const float ridge = tileNoise(u * 14.0f + warp * 3.0f, v * 4.0f, 14, 4, 11u);
            const float fine = tileNoise(u * 48.0f, v * 24.0f, 48, 24, 13u);
            float f = std::pow(std::abs(ridge * 2.0f - 1.0f), 0.6f);
            f = 0.75f * f + 0.25f * fine;
            const float r = 0.16f + 0.26f * f, g = 0.13f + 0.22f * f, b = 0.10f + 0.17f * f;
            std::uint8_t* d = &px[static_cast<std::size_t>((y * w + x) * 4)];
            d[0] = to8(r); d[1] = to8(g); d[2] = to8(b); d[3] = 255;
        }
    return encode(std::move(px), w, h);
}

Image builtinLeaf() {
    const int w = 128, h = 256;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w * h * 4), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.0f - 1.0f; // -1..1
            const float v = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(h);     // 0 base .. 1 tip
            std::uint8_t* d = &px[static_cast<std::size_t>((y * w + x) * 4)];
            // The stalk, then the blade: widest a third of the way up, pointed.
            const float stalk = 0.1f;
            if (v < stalk) {
                if (std::abs(u) < 0.045f) { d[0] = to8(0.30f); d[1] = to8(0.36f); d[2] = to8(0.14f); d[3] = 255; }
                continue;
            }
            const float t = (v - stalk) / (1.0f - stalk);
            const float half = 0.92f * std::pow(std::sin(3.14159f * std::pow(t, 0.75f)), 0.85f) * (1.0f - 0.25f * t);
            const float serr = 0.03f * std::abs(std::sin(t * 60.0f));   // a faint serration
            if (std::abs(u) > half - serr) continue;
            const float edge = std::abs(u) / std::max(half, 1e-3f);
            float r = 0.20f + 0.08f * edge, g = 0.40f + 0.1f * edge, b = 0.10f + 0.03f * edge;
            const float n = tileNoise(u * 20.0f + 40.0f, v * 40.0f, 1000, 1000, 5u);
            r += 0.04f * (n - 0.5f); g += 0.06f * (n - 0.5f);
            if (std::abs(u) < 0.025f) { r += 0.12f; g += 0.12f; b += 0.06f; }          // midrib
            const float vein = std::fmod(t * 9.0f - std::abs(u) * 3.5f + 10.0f, 1.0f);
            if (vein < 0.07f) { r += 0.05f; g += 0.06f; b += 0.02f; }                   // side veins
            d[0] = to8(r); d[1] = to8(g); d[2] = to8(b); d[3] = 255;
        }
    bleed(px, w, h);
    return encode(std::move(px), w, h);
}

Image builtinNeedles() {
    // A conifer spray: a twig with side twiglets, every one of them covered in
    // short needles -- the shape that reads as spruce from a few metres away.
    // (Long needles fanned off one stem, the first attempt, read as fern.)
    const int w = 128, h = 256;
    std::vector<float> cover(static_cast<std::size_t>(w * h), 0.0f);
    std::vector<float> tone(static_cast<std::size_t>(w * h), 0.0f);
    std::vector<std::uint8_t> wood(static_cast<std::size_t>(w * h), 0);
    auto line = [&](float ax, float ay, float bx, float by, float width, float t, bool isWood) {
        const int x0 = std::max(0, static_cast<int>(std::min(ax, bx) - width - 1.0f));
        const int x1 = std::min(w - 1, static_cast<int>(std::max(ax, bx) + width + 1.0f));
        const int y0 = std::max(0, static_cast<int>(std::min(ay, by) - width - 1.0f));
        const int y1 = std::min(h - 1, static_cast<int>(std::max(ay, by) + width + 1.0f));
        for (int yy = y0; yy <= y1; ++yy)
            for (int xx = x0; xx <= x1; ++xx) {
                const float dd = segDist(static_cast<float>(xx) + 0.5f, static_cast<float>(yy) + 0.5f, ax, ay, bx, by);
                const float c = std::clamp(width - dd + 0.5f, 0.0f, 1.0f);
                const std::size_t i = static_cast<std::size_t>(yy * w + xx);
                if (c > cover[i] || (isWood && c > 0.5f)) {
                    cover[i] = std::max(cover[i], c);
                    tone[i] = t;
                    wood[i] = isWood ? 1 : wood[i];
                }
            }
    };
    std::uint32_t n = 0;
    auto needled = [&](float ax, float ay, float bx, float by, float needle) {
        const float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) return;
        const float ux = dx / len, uy = dy / len, px = -uy, py = ux;
        for (float s = 0.0f; s < len; s += 1.6f) {
            const float taper = 0.55f + 0.45f * std::min(1.0f, (len - s) / 18.0f);
            for (int side = -1; side <= 1; side += 2) {
                ++n;
                const float r = static_cast<float>(hash2(static_cast<int>(n), side, 21u) % 1000) / 1000.0f;
                const float ang = (50.0f + 20.0f * r) * 3.14159f / 180.0f;
                const float l = needle * taper * (0.8f + 0.4f * r);
                const float cx = ax + ux * s, cy = ay + uy * s;
                const float ex = cx + (ux * std::cos(ang) + px * static_cast<float>(side) * std::sin(ang)) * l;
                const float ey = cy + (uy * std::cos(ang) + py * static_cast<float>(side) * std::sin(ang)) * l;
                line(cx, cy, ex, ey, 0.7f, 0.75f + 0.5f * r, false);
            }
        }
        line(ax, ay, bx, by, 1.0f, 1.0f, true);
    };
    const float cx = 0.5f * static_cast<float>(w);
    // Side twiglets, alternating, shorter towards the tip.
    int k = 0;
    for (float y = static_cast<float>(h) - 22.0f; y > 30.0f; y -= 15.0f, ++k) {
        const float along = 1.0f - y / static_cast<float>(h);
        const float len = 52.0f * (1.0f - 0.65f * along);
        const float side = (k % 2) ? 1.0f : -1.0f;
        const float ang = 42.0f * 3.14159f / 180.0f;
        needled(cx, y, cx + side * std::sin(ang) * len, y - std::cos(ang) * len, 7.5f);
    }
    needled(cx, static_cast<float>(h) - 2.0f, cx, 6.0f, 8.5f);   // the main twig
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w * h * 4), 0);
    for (int i = 0; i < w * h; ++i) {
        const std::size_t s = static_cast<std::size_t>(i);
        if (cover[s] < 0.5f) continue;
        std::uint8_t* d = &px[s * 4];
        // The twig in a dark olive: in brown, the light the leaf shader lets
        // through the card turned every twig into a red streak.
        if (wood[s]) { d[0] = to8(0.12f); d[1] = to8(0.14f); d[2] = to8(0.08f); }
        else { d[0] = to8(0.075f * tone[s]); d[1] = to8(0.17f * tone[s]); d[2] = to8(0.11f * tone[s]); }
        d[3] = 255;
    }
    bleed(px, w, h);
    return encode(std::move(px), w, h);
}
Image readImage(const std::string& path) {
    Image im;
    if (path.empty()) return im;
    std::vector<std::uint8_t> bytes = fitzel::vfs::read(path);
    if (bytes.size() < 8) return im;
    if (bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') im.mime = "image/png";
    else if (bytes[0] == 0xFF && bytes[1] == 0xD8) im.mime = "image/jpeg";
    else return im;
    int w = 0, h = 0, ch = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &ch)) return im;
    im.bytes = std::move(bytes);
    im.w = w; im.h = h;
    return im;
}

// --- The .glb -------------------------------------------------------------------------

Image flipGreen(const Image& in) {
    Image out;
    if (!in.valid()) return out;
    std::vector<std::uint8_t> px;
    int w = in.w, h = in.h;
    if (!in.rgba.empty()) {
        px = in.rgba;
    } else {
        int ch = 0;
        unsigned char* d = stbi_load_from_memory(in.bytes.data(), static_cast<int>(in.bytes.size()),
                                                 &w, &h, &ch, 4);
        if (!d) return out;
        px.assign(d, d + static_cast<std::size_t>(w) * h * 4);
        stbi_image_free(d);
    }
    for (std::size_t i = 1; i < px.size(); i += 4) px[i] = static_cast<std::uint8_t>(255 - px[i]);
    return encode(std::move(px), w, h);
}

bool writeGlb(const std::string& path, const treegen::Mesh& mesh, const treegen::Params& params,
              const Image& bark, const Image& leaf, const Image& barkNormal, std::string* err) {
    using nlohmann::json;
    std::vector<std::uint8_t> bin;
    json views = json::array(), accessors = json::array();

    auto align = [&] { while (bin.size() % 4) bin.push_back(0); };
    auto addView = [&](const void* data, std::size_t bytes, int target) {
        align();
        json v = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes}};
        if (target) v["target"] = target;
        const auto* p = static_cast<const std::uint8_t*>(data);
        bin.insert(bin.end(), p, p + bytes);
        views.push_back(v);
        return static_cast<int>(views.size()) - 1;
    };
    // One part: de-interleave the eight floats into the three attributes.
    auto addPart = [&](const std::vector<float>& verts, const std::vector<std::uint32_t>& idx,
                       int material) {
        const std::size_t n = verts.size() / 8;
        std::vector<float> pos(n * 3), nrm(n * 3), uv(n * 2);
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (std::size_t i = 0; i < n; ++i) {
            for (int k = 0; k < 3; ++k) {
                pos[i * 3 + k] = verts[i * 8 + k];
                nrm[i * 3 + k] = verts[i * 8 + 3 + k];
                lo[k] = std::min(lo[k], pos[i * 3 + k]);
                hi[k] = std::max(hi[k], pos[i * 3 + k]);
            }
            uv[i * 2]     = verts[i * 8 + 6];
            uv[i * 2 + 1] = verts[i * 8 + 7];
        }
        const int vp = addView(pos.data(), pos.size() * 4, 34962);
        const int vn = addView(nrm.data(), nrm.size() * 4, 34962);
        const int vu = addView(uv.data(), uv.size() * 4, 34962);
        const int vi = addView(idx.data(), idx.size() * 4, 34963);
        const int a0 = static_cast<int>(accessors.size());
        accessors.push_back({{"bufferView", vp}, {"componentType", 5126}, {"count", n}, {"type", "VEC3"},
                             {"min", {lo[0], lo[1], lo[2]}}, {"max", {hi[0], hi[1], hi[2]}}});
        accessors.push_back({{"bufferView", vn}, {"componentType", 5126}, {"count", n}, {"type", "VEC3"}});
        accessors.push_back({{"bufferView", vu}, {"componentType", 5126}, {"count", n}, {"type", "VEC2"}});
        accessors.push_back({{"bufferView", vi}, {"componentType", 5125}, {"count", idx.size()}, {"type", "SCALAR"}});
        return json{{"attributes", {{"POSITION", a0}, {"NORMAL", a0 + 1}, {"TEXCOORD_0", a0 + 2}}},
                    {"indices", a0 + 3}, {"material", material}, {"mode", 4}};
    };

    json prims = json::array(), materials = json::array(), textures = json::array(), images = json::array();
    json samplers = json::array({
        {{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}},
        {{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 33071}, {"wrapT", 33071}},
    });
    auto addTexture = [&](const Image& im, int sampler) {
        if (!im.valid()) return -1;
        const int view = addView(im.bytes.data(), im.bytes.size(), 0);
        images.push_back({{"bufferView", view}, {"mimeType", im.mime}});
        textures.push_back({{"sampler", sampler}, {"source", static_cast<int>(images.size()) - 1}});
        return static_cast<int>(textures.size()) - 1;
    };
    if (!mesh.barkIdx.empty()) {
        json m = {{"name", "Bark"},
                  {"pbrMetallicRoughness", {{"metallicFactor", 0.0}, {"roughnessFactor", 0.95}}}};
        const int t = addTexture(bark, 0);
        if (t >= 0) m["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", t}};
        else m["pbrMetallicRoughness"]["baseColorFactor"] = {0.12, 0.09, 0.07, 1.0};
        const int n = addTexture(barkNormal, 0);
        if (n >= 0) m["normalTexture"] = {{"index", n}, {"scale", params.barkNormalStrength}};
        materials.push_back(m);
        prims.push_back(addPart(mesh.bark, mesh.barkIdx, static_cast<int>(materials.size()) - 1));
    }
    if (!mesh.leafIdx.empty()) {
        json m = {{"name", "Leaves"}, {"alphaMode", "MASK"}, {"alphaCutoff", 0.5}, {"doubleSided", true},
                  {"pbrMetallicRoughness", {{"metallicFactor", 0.0}, {"roughnessFactor", 0.7}}}};
        const int t = addTexture(leaf, 1);
        if (t >= 0) m["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", t}};
        else m["pbrMetallicRoughness"]["baseColorFactor"] = {0.05, 0.16, 0.03, 1.0};
        materials.push_back(m);
        prims.push_back(addPart(mesh.leaves, mesh.leafIdx, static_cast<int>(materials.size()) - 1));
    }
    if (prims.empty()) { if (err) *err = "The tree is empty."; return false; }
    align();

    json tree;
    treegen::toJson(params, tree);
    json j = {
        {"asset", {{"version", "2.0"}, {"generator", "fitzel tree generator"},
                   {"extras", {{"fitzelTree", tree}}}}},
        {"scene", 0},
        {"scenes", json::array({{{"nodes", {0}}}})},
        {"nodes", json::array({{{"mesh", 0}, {"name", params.name}}})},
        {"meshes", json::array({{{"name", params.name}, {"primitives", prims}}})},
        {"materials", materials},
        {"accessors", accessors},
        {"bufferViews", views},
        {"buffers", json::array({{{"byteLength", bin.size()}}})},
    };
    if (!images.empty()) { j["images"] = images; j["textures"] = textures; j["samplers"] = samplers; }

    std::string text = j.dump();
    while (text.size() % 4) text.push_back(' ');
    const std::uint32_t total = 12 + 8 + static_cast<std::uint32_t>(text.size()) + 8 +
                                static_cast<std::uint32_t>(bin.size());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "Cannot write " + path; return false; }
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    u32(0x46546C67u); u32(2u); u32(total);
    u32(static_cast<std::uint32_t>(text.size())); u32(0x4E4F534Au);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    u32(static_cast<std::uint32_t>(bin.size())); u32(0x004E4942u);
    f.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    if (!f) { if (err) *err = "Write failed: " + path; return false; }
    return true;
}

bool readParams(const std::string& path, treegen::Params& out) {
    const std::vector<std::uint8_t> d = fitzel::vfs::read(path);
    if (d.size() < 20) return false;
    std::uint32_t magic = 0, len = 0, type = 0;
    std::memcpy(&magic, d.data(), 4);
    std::memcpy(&len, d.data() + 12, 4);
    std::memcpy(&type, d.data() + 16, 4);
    if (magic != 0x46546C67u || type != 0x4E4F534Au || 20ull + len > d.size()) return false;
    try {
        const nlohmann::json j = nlohmann::json::parse(d.begin() + 20, d.begin() + 20 + len);
        const auto a = j.find("asset");
        if (a == j.end() || !a->contains("extras") || !(*a)["extras"].contains("fitzelTree")) return false;
        out = treegen::fromJson((*a)["extras"]["fitzelTree"]);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace treeglb