// camtexcheck -- what a camera sees, on a material (CameraTexture.hpp).
//
// A real GL context and the real CameraTextures, with a stand-in for the scene
// pass: the "scene" is a clear to a colour the harness picks, so what lands in
// the material's texture can be read back and compared texel for texel. What is
// checked is everything the module itself decides -- which cameras are drawn,
// how often, at what shape, into which texture, what the materials are pointed
// at, that the framebuffer and the viewport come back as they were, and that a
// file keeps the link. The scene pass itself is the camera preview's, which the
// editor has drawn for months.
//
//   build/release/bin/camtexcheck.exe

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <fitzel/core/Window.hpp>
#include <fitzel/graphics/Texture.hpp>

#include "../src/CameraTexture.hpp"
#include "../src/Component.hpp"
#include "../src/SceneTypes.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}

// The texel in the middle of `tex`, as 0..255 RGB.
glm::ivec3 middle(const fitzel::Texture& tex) {
    const fitzel::ImagePixels px = tex.readback();
    if (!px.valid()) return glm::ivec3(-1);
    const std::size_t at = (static_cast<std::size_t>(px.height / 2) * px.width + px.width / 2) * px.channels;
    return glm::ivec3(px.pixels[at], px.pixels[at + 1], px.pixels[at + 2]);
}
bool near(glm::ivec3 a, glm::ivec3 b) {
    return std::abs(a.x - b.x) <= 2 && std::abs(a.y - b.y) <= 2 && std::abs(a.z - b.z) <= 2;
}
std::string str(glm::ivec3 c) {
    return std::to_string(c.x) + " " + std::to_string(c.y) + " " + std::to_string(c.z);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fitzel::Window window(fitzel::WindowConfig{
        .width = 320, .height = 200, .title = "camtexcheck", .vsync = false, .maximized = false});

    // The scene: a camera called "Monitor Cam", and something else.
    std::vector<Entity> entities(2);
    entities[0].id   = 7;
    entities[0].name = "Monitor Cam";
    entities[0].components.items.push_back(std::make_unique<CameraComponent>());
    entities[1].id   = 8;
    entities[1].name = "Crate";

    // Two materials on the camera at one size, one at another, one naming a
    // camera that is not there, and one with nothing to do with cameras.
    std::vector<MaterialDef> mats(5);
    mats[0].name = "screen A";  mats[0].cameraName = "Monitor Cam"; mats[0].cameraSize = {64, 36};
    mats[1].name = "screen B";  mats[1].cameraName = "Monitor Cam"; mats[1].cameraSize = {64, 36};
    mats[2].name = "mirror";    mats[2].cameraName = "Monitor Cam"; mats[2].cameraSize = {32, 32};
    mats[2].cameraGlow = false;
    mats[3].name = "nobody's";  mats[3].cameraName = "Nobody";
    mats[4].name = "paint";

    int poses = 0;
    auto poseOf = [&](int id, camerasys::Pose& out) {
        ++poses;
        if (id != 7) return false;
        out.position = glm::vec3(0.0f, 2.0f, 5.0f);
        out.front    = glm::vec3(0.0f, 0.0f, -1.0f);
        out.fov      = 50.0f;
        return true;
    };
    glm::vec3 colour(0.2f, 0.6f, 1.0f);
    int draws = 0;
    std::vector<float> aspects;   // of every picture drawn
    GLint drawnInto = 0;
    auto draw = [&](const glm::mat4&, const glm::mat4& proj, const camerasys::Pose&) {
        ++draws;
        aspects.push_back(proj[1][1] / proj[0][0]);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawnInto);
        glClearColor(colour.r, colour.g, colour.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    };

    camtex::CameraTextures feeds;
    glViewport(5, 6, 100, 50);
    feeds.update(mats, entities, poseOf, 0.1f, 1000.0f, 0.0, draw);
    GLint fbo = -1, vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, vp);

    check(feeds.drawnLastUpdate() == 2 && draws == 2,
          "each camera is drawn once per size, however many materials show it",
          std::to_string(draws) + " drawn");
    check(drawnInto != 0, "...into a target of its own, not the screen");
    check(fbo == 0 && vp[0] == 5 && vp[1] == 6 && vp[2] == 100 && vp[3] == 50,
          "the framebuffer and the viewport come back as they were");
    check(mats[0].tex && mats[0].tex == mats[1].tex && mats[2].tex && mats[2].tex != mats[0].tex,
          "the materials on one camera and size share one picture; another size has its own");
    check(mats[0].tex && mats[0].tex->width() == 64 && mats[0].tex->height() == 36,
          "the picture is the size the material asks for");
    const glm::ivec3 want(51, 153, 255);
    check(mats[0].tex && near(middle(*mats[0].tex), want), "what the camera saw lands in the material's texture",
          mats[0].tex ? str(middle(*mats[0].tex)) : "no texture");
    check(mats[0].emissionTex == mats[0].tex && !mats[2].emissionTex,
          "a glowing material has the picture as its emission map; one that doesn't, not");
    check(!mats[3].tex && !mats[4].tex, "a material naming no camera of the scene, or none, is left alone");

    // At most 30 pictures a second.
    feeds.update(mats, entities, poseOf, 0.1f, 1000.0f, 0.01, draw);
    check(feeds.drawnLastUpdate() == 0, "a hundredth of a second later, nothing is drawn again");
    colour = glm::vec3(1.0f, 0.0f, 0.0f);
    feeds.update(mats, entities, poseOf, 0.1f, 1000.0f, 0.05, draw);
    check(feeds.drawnLastUpdate() == 2 && near(middle(*mats[0].tex), glm::ivec3(255, 0, 0)),
          "a thirtieth later it is, and the picture is live",
          str(middle(*mats[0].tex)));
    bool wide = false, square = false;
    for (float a : aspects) {
        wide   |= std::abs(a - 64.0f / 36.0f) < 1e-3f;
        square |= std::abs(a - 1.0f) < 1e-3f;
    }
    check(wide && square, "the camera is drawn at the shape of each picture");

    // Glow off: the emission map it had.
    mats[0].cameraGlow = false;
    feeds.update(mats, entities, poseOf, 0.1f, 1000.0f, 0.10, draw);
    check(!mats[0].emissionTex && mats[1].emissionTex == mats[1].tex,
          "turning the glow off gives the emission map back");

    // The camera gone from the scene: nothing drawn, the surface keeps its last frame.
    entities[0].name = "Renamed";
    const auto held = mats[0].tex;
    feeds.update(mats, entities, poseOf, 0.1f, 1000.0f, 0.20, draw);
    check(feeds.drawnLastUpdate() == 0 && mats[0].tex == held,
          "with no camera of that name, nothing is drawn and the surface keeps its last picture");
    entities[0].name = "Monitor Cam";

    // In a file.
    nlohmann::json j, none;
    camtex::save(mats[2], j);
    camtex::save(mats[4], none);
    MaterialDef back;
    camtex::load(j, back);
    check(back.cameraName == "Monitor Cam" && back.cameraSize == glm::ivec2(32, 32) && !back.cameraGlow,
          "a file keeps the camera's name, the size and the glow", j.dump());
    check(none.is_null() || none.empty(), "a material without a camera writes nothing for it");

    std::printf("\ncamtexcheck: %s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
