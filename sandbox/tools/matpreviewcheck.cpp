// matpreviewcheck -- the Materials panel's preview, drawn for a row of typical
// materials and written to a PNG, so "does the ball look like the material" is
// answered by looking, not by starting the editor.
//
//   build/release/bin/matpreviewcheck.exe [out.png] [baseColour.png normalMap.jpg]
//
// Draws: red plastic, polished gold, rough iron, a textured material (with its
// normal map when given), glass, an emissive one, a see-through blend -- each as
// a sphere, plus the textured one as a cube and a tile. Exits non-zero only if
// the shader did not load or nothing could be written.

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/graphics/Texture.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/MaterialPreview.hpp"
#include "../src/SceneTypes.hpp"

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "matpreview.png";
    fitzel::Window window(fitzel::WindowConfig{
        .width = 640, .height = 480, .title = "matpreviewcheck", .vsync = false, .maximized = false});
    MaterialPreview prev;
    if (!prev.init()) { std::printf("[matpreviewcheck] the preview shader did not load\n"); return 1; }

    std::vector<MaterialDef> mats(10);
    mats[0].name = "plastic"; mats[0].albedo = {0.75f, 0.12f, 0.1f}; mats[0].roughness = 0.35f;
    mats[1].name = "gold";    mats[1].albedo = {1.0f, 0.78f, 0.34f}; mats[1].reflectivity = 1.0f; mats[1].roughness = 0.18f;
    mats[2].name = "iron";    mats[2].albedo = {0.56f, 0.57f, 0.58f}; mats[2].reflectivity = 1.0f; mats[2].roughness = 0.7f;
    if (argc > 3) {
        auto base = std::make_shared<fitzel::Texture>(fitzel::Texture::fromFile(argv[2]));
        auto nrm  = std::make_shared<fitzel::Texture>(fitzel::Texture::fromFile(argv[3]));
        for (int i : {3, 8, 9}) {
            mats[i].tex = base->isValid() ? base : nullptr;
            mats[i].normalTex = nrm->isValid() ? nrm : nullptr;
            mats[i].roughness = 0.6f;
        }
    } else {
        mats[3].albedo = {0.3f, 0.5f, 0.2f};
    }
    mats[3].name = "textured";
    mats[4].name = "glass";   mats[4].glass = true; mats[4].roughness = 0.05f; mats[4].albedo = {0.9f, 0.95f, 1.0f};
    mats[5].name = "emissive"; mats[5].albedo = {0.1f, 0.1f, 0.1f}; mats[5].emission = {1.0f, 0.45f, 0.1f};
    mats[5].emissionStrength = 3.0f;
    mats[6].name = "blend";   mats[6].albedo = {0.2f, 0.5f, 0.9f}; mats[6].alphaMode = AlphaMode::Blend;
    mats[6].opacity = 0.45f;
    mats[7].name = "rough white"; mats[7].albedo = {0.9f, 0.9f, 0.88f}; mats[7].roughness = 1.0f;

    const int S = 256, cols = 5, rows = 2;
    std::vector<unsigned char> sheet(static_cast<std::size_t>(S * cols) * S * rows * 4, 0);
    for (int i = 0; i < 10; ++i) {
        const auto shape = i == 8 ? MaterialPreview::Shape::Cube
                         : i == 9 ? MaterialPreview::Shape::Tile : MaterialPreview::Shape::Sphere;
        const std::uint32_t tex = prev.render(mats[i], S, shape, 30.0f);
        if (!tex) { std::printf("[matpreviewcheck] render %d failed\n", i); return 1; }
        std::vector<unsigned char> px(static_cast<std::size_t>(S) * S * 4);
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        const int cx = (i % cols) * S, cy = (i / cols) * S;
        for (int y = 0; y < S; ++y)     // GL rows are bottom-up
            std::memcpy(&sheet[(static_cast<std::size_t>(cy + y) * S * cols + cx) * 4],
                        &px[static_cast<std::size_t>(S - 1 - y) * S * 4], static_cast<std::size_t>(S) * 4);
    }
    const bool ok = stbi_write_png(out.c_str(), S * cols, S * rows, 4, sheet.data(), S * cols * 4) != 0;
    std::printf(ok ? "[matpreviewcheck] wrote %s\n" : "[matpreviewcheck] could not write %s\n", out.c_str());
    return ok ? 0 : 1;
}