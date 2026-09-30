// treecheck -- the tree generator, end to end: grow every preset, check the
// mesh, write it as .glb, load it back the way the vegetation loads a species,
// and draw it in the generator's own studio to a PNG.
//
// The numbers catch what can be caught by arithmetic -- a NaN from a
// degenerate frame, an index past the end, a tree that is not the height asked
// for, a generator that is not deterministic, a leaf edit that reshuffles the
// limbs. The pictures answer the only question that matters and has no
// arithmetic answer: does it look like a tree.
//
//   treecheck <outDir> [--preset name] [--bark file] [--normal file] [--no-normal]
//             [--relief s] [--leaf file] [--cols N] [--rows N]
//             [--needles file] [--size WxH] [--yaw d] [--pitch d] [--zoom z] [--lift f]
//             [--seed n] [--no-glb]
//
// Run from the directory the shaders were copied to (build/release/bin).
// Exits non-zero if a check failed.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fitzel/graphics/Texture.hpp>
#include <fitzel/world/Model.hpp>

#include "../src/TreeGen.hpp"
#include "../src/TreeGlb.hpp"
#include "../src/TreePreview.hpp"

namespace fs = std::filesystem;

namespace {

int g_fail = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? " OK " : "FAIL", what, detail.empty() ? "" : " -- ",
                detail.c_str());
    if (!ok) ++g_fail;
}

bool meshSane(const treegen::Mesh& m, std::string& why) {
    for (const auto* part : {&m.bark, &m.leaves}) {
        const std::vector<float>& v = *part;
        if (v.size() % 8) { why = "vertex array not a multiple of 8"; return false; }
        for (std::size_t i = 0; i < v.size(); i += 8) {
            for (int k = 0; k < 8; ++k)
                if (!std::isfinite(v[i + k])) { why = "non-finite vertex"; return false; }
            const float nl = std::sqrt(v[i + 3] * v[i + 3] + v[i + 4] * v[i + 4] + v[i + 5] * v[i + 5]);
            if (std::abs(nl - 1.0f) > 1e-3f) { why = "normal not unit length"; return false; }
        }
    }
    const std::size_t nb = m.bark.size() / 8, nl = m.leaves.size() / 8;
    for (std::uint32_t i : m.barkIdx) if (i >= nb) { why = "bark index out of range"; return false; }
    for (std::uint32_t i : m.leafIdx) if (i >= nl) { why = "leaf index out of range"; return false; }
    if (m.barkIdx.size() % 3 || m.leafIdx.size() % 3) { why = "index count not a multiple of 3"; return false; }
    return true;
}

std::string slug(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back((std::isalnum(static_cast<unsigned char>(c)) != 0) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_');
    return o;
}

fitzel::Texture toTexture(const treeglb::Image& im, const std::string& path) {
    if (!im.rgba.empty()) return fitzel::Texture::fromPixels(im.rgba.data(), im.w, im.h, 4);
    return fitzel::Texture::fromFile(path, false);   // glTF convention: top row first
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: treecheck <outDir> [--preset name] [--bark f] [--leaf f] [--cols N] [--rows N]\n");
        return 2;
    }
    const fs::path outDir = argv[1];
    fs::create_directories(outDir);
    std::string only, barkPath, leafPath, needlePath, normalPath;
    bool useNormal = true;
    float relief = 1.0f;
    int cols = 0, rows = 0, W = 640, H = 800;
    long seed = -1;
    bool glb = true;
    TreePreview::View view;
    const std::string content = FITZEL_CONTENT_DIR;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--preset") only = next();
        else if (a == "--bark") barkPath = next();
        else if (a == "--normal") normalPath = next();
        else if (a == "--no-normal") useNormal = false;
        else if (a == "--relief") relief = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--leaf") leafPath = next();
        else if (a == "--needles") needlePath = next();
        else if (a == "--cols") cols = std::atoi(next().c_str());
        else if (a == "--rows") rows = std::atoi(next().c_str());
        else if (a == "--yaw") view.yaw = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--pitch") view.pitch = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--zoom") view.zoom = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--lift") view.lift = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--seed") seed = std::atol(next().c_str());
        else if (a == "--no-glb") glb = false;
        else if (a == "--size") { const std::string s = next(); std::sscanf(s.c_str(), "%dx%d", &W, &H); }
    }
    // The textures the editor ships with, when nothing else is given.
    if (barkPath.empty() && fs::exists(content + "/models/Bark001_2K-JPG_Color.jpg"))
        barkPath = content + "/models/Bark001_2K-JPG_Color.jpg";
    if (leafPath.empty() && fs::exists(content + "/models/LeafSet024_1K-JPG_Color-LeafSet024_1K-JPG_Opacity.png")) {
        leafPath = content + "/models/LeafSet024_1K-JPG_Color-LeafSet024_1K-JPG_Opacity.png";
        if (cols == 0) cols = 3;
        if (rows == 0) rows = 3;
    }
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;
    if (useNormal && normalPath.empty() && fs::exists(content + "/models/Bark001_2K-JPG_NormalGL.jpg"))
        normalPath = content + "/models/Bark001_2K-JPG_NormalGL.jpg";
    if (!useNormal) normalPath.clear();

    if (!glfwInit()) { std::printf("[FAIL] glfwInit\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "treecheck", nullptr, nullptr);
    if (!win) { std::printf("[FAIL] no GL 3.3 context\n"); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(glfwGetProcAddress)) { std::printf("[FAIL] gladLoadGL\n"); return 2; }

    int ran = 0;
    {
        TreePreview studio;
        check(studio.init(), "preview shaders compile");
        const treeglb::Image barkImg = barkPath.empty() ? treeglb::builtinBark() : treeglb::readImage(barkPath);
        const treeglb::Image leafImg = leafPath.empty() ? treeglb::builtinLeaf() : treeglb::readImage(leafPath);
        const treeglb::Image needleImg = needlePath.empty() ? treeglb::builtinNeedles() : treeglb::readImage(needlePath);
        check(barkImg.valid() && leafImg.valid() && needleImg.valid(), "textures read",
              barkPath + " | " + leafPath);
        const treeglb::Image normalImg = normalPath.empty() ? treeglb::Image{} : treeglb::readImage(normalPath);
        if (!normalPath.empty()) check(normalImg.valid(), "bark normal map read", normalPath);

        // Which way the relief leans. A map whose every texel tilts towards the
        // top of the image must tilt the bark towards where the top of the
        // bark IMAGE lies on the wood -- the stem's foot, since v runs up the
        // stem and the image's top row is v = 0 -- and so, under a noon sun,
        // darken it; the opposite map must brighten it. A flipped green in the
        // shader swaps the two, and a trunk lit from the wrong end looks
        // like bark turned inside out, which is exactly what nobody can name.
        {
            treegen::Params p = treegen::presets().front().params;
            p.leaves.enabled = false;
            const treegen::Mesh m = treegen::generate(p);
            studio.setMesh(m);
            auto solid = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
                std::vector<std::uint8_t> px(16 * 16 * 4);
                for (std::size_t i = 0; i < px.size(); i += 4) { px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255; }
                return fitzel::Texture::fromPixels(px.data(), 16, 16, 4);
            };
            TreePreview::View v;
            v.pitch = 5.0f; v.sunPitch = 80.0f; v.zoom = 0.6f; v.lift = -0.2f;
            auto luma = [&](std::uint8_t g) {
                studio.setTextures(solid(128, 128, 128), solid(128, 128, 128), solid(128, g, 200));
                studio.render(320, 400, v);
                const std::vector<std::uint8_t> px = studio.readRGBA();
                double s = 0.0;
                for (std::size_t i = 0; i < px.size(); i += 4) s += px[i] * 0.2126 + px[i + 1] * 0.7152 + px[i + 2] * 0.0722;
                return s / static_cast<double>(px.size() / 4);
            };
            const double flat = luma(128), towardsTop = luma(230), towardsBottom = luma(26);
            char buf[160];
            std::snprintf(buf, sizeof buf, "mean luma: tilt to image top %.2f < flat %.2f < tilt to image bottom %.2f",
                          towardsTop, flat, towardsBottom);
            check(towardsTop < flat - 0.05 && towardsBottom > flat + 0.05, "relief leans the glTF way", buf);
        }
        studio.setReliefStrength(relief);
        // The built-in stand-ins, to be looked at like the trees.
        for (const auto& [img, name] : {std::pair{treeglb::builtinBark(), "builtin_bark.png"},
                                        std::pair{treeglb::builtinLeaf(), "builtin_leaf.png"},
                                        std::pair{treeglb::builtinNeedles(), "builtin_needles.png"}}) {
            FILE* f = std::fopen((outDir / name).string().c_str(), "wb");
            if (f) { std::fwrite(img.bytes.data(), 1, img.bytes.size(), f); std::fclose(f); }
        }

        for (const treegen::Preset& pr : treegen::presets()) {
            if (!only.empty() && slug(only) != slug(pr.name)) continue;
            ++ran;
            std::printf("%s\n", pr.name);
            treegen::Params p = pr.params;
            if (seed >= 0) p.seed = static_cast<std::uint32_t>(seed);
            const bool needles = p.leaves.alongTwig;
            const treeglb::Image& leaf = needles ? needleImg : leafImg;
            if (!needles) { p.leaves.cols = cols; p.leaves.rows = rows; }
            p.leaves.aspect = (static_cast<float>(leaf.w) / static_cast<float>(p.leaves.cols)) /
                              (static_cast<float>(leaf.h) / static_cast<float>(p.leaves.rows));
            p.barkAspect = static_cast<float>(barkImg.h) / static_cast<float>(std::max(barkImg.w, 1));

            const auto t0 = std::chrono::steady_clock::now();
            const treegen::Mesh m = treegen::generate(p);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            char buf[256];
            std::snprintf(buf, sizeof buf, "%zu bark + %zu leaf tris, %d stems, %d cards, e=%.2f, %.1f ms",
                          m.barkTris(), m.leafTris(), m.stems, m.leafCards, m.exponent, ms);
            check(!m.barkIdx.empty() && !m.leafIdx.empty(), "grows bark and leaves", buf);
            std::string why;
            check(meshSane(m, why), "mesh is sane", why);
            const float h = m.hi.y - m.lo.y;
            std::snprintf(buf, sizeof buf, "%.2f m tall (asked %.2f), foot at y=%.3f", h, p.height, m.lo.y);
            check(m.lo.y > -0.01f && h > 0.7f * p.height && h < 1.35f * p.height, "height", buf);

            const treegen::Mesh again = treegen::generate(p);
            check(again.bark == m.bark && again.leaves == m.leaves && again.leafIdx == m.leafIdx,
                  "deterministic");
            treegen::Params q = p;
            q.leaves.size *= 1.3f; q.leaves.perStem += 2; q.leaves.hollow *= 0.5f;
            check(treegen::generate(q).bark == m.bark, "leaf edits leave the wood alone");

            if (glb) {
                const std::string file = (outDir / (slug(pr.name) + ".glb")).string();
                std::string err;
                p.barkNormal = normalPath;
                p.barkNormalStrength = relief;
                const bool wrote = treeglb::writeGlb(file, m, p, barkImg, leaf, normalImg, &err);
                check(wrote, "writes .glb", err);
                if (wrote) {
                    const fitzel::ModelData md = fitzel::loadGltf(file);
                    bool ok = md.primitives.size() == 2;
                    std::snprintf(buf, sizeof buf, "%zu parts", md.primitives.size());
                    if (ok) {
                        const fitzel::ModelPrimitive& b = md.primitives[0];
                        const fitzel::ModelPrimitive& l = md.primitives[1];
                        ok = !b.alphaCutout && l.alphaCutout && !b.texPixels.empty() && !l.texPixels.empty() &&
                             b.vertexCount() == static_cast<int>(m.barkIdx.size()) &&
                             l.vertexCount() == static_cast<int>(m.leafIdx.size()) &&
                             b.normalPixels.empty() == !normalImg.valid() && l.normalPixels.empty() &&
                             (!normalImg.valid() || std::abs(b.normalScale - relief) < 1e-4f);
                        std::snprintf(buf, sizeof buf, "bark %dx%d (normal %dx%d x%.1f), leaves %dx%d cut=%d, %.1f MB",
                                      b.texWidth, b.texHeight, b.normalWidth, b.normalHeight, b.normalScale,
                                      l.texWidth, l.texHeight, l.alphaCutout ? 1 : 0,
                                      static_cast<double>(fs::file_size(file)) / 1048576.0);
                    }
                    check(ok, "loads back as a two-part tree", buf);
                    treegen::Params back;
                    check(treeglb::readParams(file, back) && back.name == p.name && back.seed == p.seed &&
                              back.height == p.height && back.level[1].count == p.level[1].count,
                          "carries its parameters");
                }
            }

            // The picture, drawn at twice the size and boxed down (the leaves
            // are alpha-tested; the studio has no MSAA).
            studio.setMesh(m);
            studio.setTextures(toTexture(barkImg, barkPath), toTexture(leaf, needles ? needlePath : leafPath),
                               normalImg.valid() ? toTexture(normalImg, normalPath) : fitzel::Texture{});
            studio.render(W * 2, H * 2, view);
            const std::vector<std::uint8_t> big = studio.readRGBA();
            std::vector<std::uint8_t> px(static_cast<std::size_t>(W * H * 4));
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    for (int c = 0; c < 4; ++c) {
                        int s = 0;
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                                s += big[static_cast<std::size_t>(((y * 2 + dy) * W * 2 + (x * 2 + dx)) * 4 + c)];
                        px[static_cast<std::size_t>((y * W + x) * 4 + c)] = static_cast<std::uint8_t>(s / 4);
                    }
            const std::string png = (outDir / (slug(pr.name) + ".png")).string();
            check(stbi_write_png(png.c_str(), W, H, 4, px.data(), W * 4) != 0, "picture", png);
        }
    }
    glfwDestroyWindow(win);
    glfwTerminate();
    if (ran == 0) { std::printf("[FAIL] no preset matched '%s'\n", only.c_str()); return 1; }
    std::printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}