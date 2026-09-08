// The grass check.
//
// The path tracer's grass is a second implementation of assets/shaders/grass.vert
// -- there is no way around that, because the raster blade exists only as vertex
// -shader arithmetic and the tracer needs triangles. A second implementation is
// exactly the kind of thing that agrees on the day it is written and drifts
// afterwards, silently, because both versions keep producing grass.
//
// So this file asserts on the parts that HAVE answers, and writes a picture for
// the part that does not:
//   * The placement is the streamer's own. Two calls to generateTile for the
//     same tile must produce identical instances, and neighbouring tiles must
//     not overlap -- that is what makes streaming re-entry invisible, and the
//     tracer relies on the same property to render a field the viewport agrees
//     with.
//   * A blade is five triangles, standing on its instance's base position, no
//     taller than the height it was given. Not a matter of taste: a blade that
//     floats or reaches into the sky is a transform bug and shows up as a number
//     long before it shows up as a wrong-looking meadow.
//   * The colour palette is bounded. Quantisation exists so a million blades do
//     not become a million materials, and the day it stops working the GPU
//     tracer runs out of buffer rather than looking wrong.
//   * The wind is a pose, not noise: the same windTime twice is the same
//     geometry, and a different one is not.
// ...and then a rendered PNG, because whether it looks like grass is a question
// only an eye answers.
//
//   build/release/bin/grasscheck.exe [outDir]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/GrassTrace.hpp"
#include "../src/PathTrace.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_failures;
}

// A field on moist ground with nothing in the way -- the conditions the
// placement filters ask for, so a test is about the blades rather than about an
// empty meadow.
//
// `flat` zeroes the relief. That is not cosmetic: on real ground a blade's base
// is the terrain height at its CELL, while the blade itself is jittered up to
// 0.8 m away, so on a slope it stands slightly above or below the surface under
// it. That is the streamer's own behaviour and the raster field has it too --
// but it means "is the blade standing on the ground" only has an exact answer
// where the ground is level. The geometry checks use the flat world; the
// picture uses the real one.
grassfield::Field testField(bool flat) {
    grassfield::Field f;
    f.terrain = fitzel::TerrainSettings{};
    if (flat) {
        f.terrain.heightScale  = 0.0f;
        f.terrain.ridgeScale   = 0.0f;
        f.terrain.continentAmp = 0.0f;
        f.terrain.warpStrength = 0.0f;
        f.terrain.valleyDepth  = 0.0f;
    }
    f.waterLevel = -1000.0f;
    f.snowLevel  = 1000.0f;
    f.height     = 0.35f;
    f.density    = 1.0f;
    f.chaos      = 0.5f;
    f.roadClear  = 0.0f;
    return f;
}

std::shared_ptr<pathtrace::Scene> grassScene(float radius, float windTime,
                                             bool flat, int colorSteps,
                                             float upBias, float translucency,
                                             grassfield::TraceReport* rep) {
    auto scene = std::make_shared<pathtrace::Scene>();

    // The ground the blades stand on, so the picture has a floor and the grass
    // has something to bounce light off. One big quad, mid grey-brown.
    pathtrace::Material soil;
    soil.albedo    = glm::vec3(0.20f, 0.17f, 0.13f);
    soil.roughness = 0.9f;
    scene->materials.push_back(soil);

    // The soil the blades stand on. Flat, and therefore only honest in the flat
    // world -- which is why the pictures use that one: this harness traces the
    // GRASS, not the terrain, so a relief field would leave every blade standing
    // on a plane that is nowhere near it. The height comes from the generator
    // rather than from 0, because "flat" does not mean "at zero".
    const float g = 400.0f;
    const glm::vec3 n(0.0f, 1.0f, 0.0f);
    const float ground = fitzel::terrainHeight(testField(flat).terrain, 0.0f, 0.0f);
    const float y = ground - 0.02f;
    pathtrace::Triangle a, b;
    a.p0 = {-g, y, -g}; a.p1 = {g, y, -g}; a.p2 = {g, y, g};
    b.p0 = {-g, y, -g}; b.p1 = {g, y,  g}; b.p2 = {-g, y, g};
    a.n0 = a.n1 = a.n2 = n;
    b.n0 = b.n1 = b.n2 = n;
    a.material = b.material = 0;
    scene->triangles.push_back(a);
    scene->triangles.push_back(b);

    grassfield::TraceOptions opt;
    opt.centerXZ = glm::vec2(0.0f);
    opt.radius   = radius;
    opt.windTime   = windTime;
    opt.colorSteps   = colorSteps;
    opt.normalUpBias = upBias;
    opt.translucency = translucency;
    grassfield::appendToScene(*scene, testField(flat), opt, rep);

    // Daylight at the strength the tracer's own reference frames use (see
    // TraceScenes.hpp): a sun around 2.4 against a sky around 0.2. The numbers
    // matter more than they look. A sun of 1.0 over a sky of 0.6 is not "the
    // same picture, dimmer" -- it is a blown-out sky over a meadow in the dark,
    // which reads exactly like grass whose lighting is broken. Neither the
    // albedo nor the normals were ever wrong; this was.
    scene->sun.direction = glm::normalize(glm::vec3(0.45f, 0.72f, 0.45f));
    scene->sun.color     = glm::vec3(2.4f, 2.3f, 2.1f);
    scene->sun.angularRadiusDeg = 0.6f;
    scene->env.zenith  = glm::vec3(0.18f, 0.26f, 0.44f);
    scene->env.horizon = glm::vec3(0.34f, 0.40f, 0.50f);
    scene->env.ground  = glm::vec3(0.12f, 0.11f, 0.10f);
    scene->env.intensity = 1.0f;
    // The editor's own starting grade, for the reason lookScene() gives: a
    // render never comes out ungraded, so judging one that has not been is
    // judging a picture nobody will ever see.
    scene->grade.saturation = 1.35f;
    scene->grade.warmth     = 0.18f;
    scene->grade.contrast   = 0.16f;

    // Just above the canopy, looking down the field: high enough to see the
    // meadow as a surface, low enough that a blade is still a blade. Under it
    // the picture is only the inside of the canopy, which is dark and says
    // nothing about whether the blades are right.
    scene->camera.position = glm::vec3(0.0f, ground + 0.75f, 7.0f);
    const glm::vec3 fwd = glm::normalize(glm::vec3(0.0f, -0.22f, -1.0f));
    scene->camera.forward = fwd;
    scene->camera.right   = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
    scene->camera.up      = glm::cross(scene->camera.right, fwd);
    scene->camera.fovDegrees = 42.0f;
    scene->exposure = 1.0f;
    return scene;
}

void writePng(const std::filesystem::path& file,
              std::shared_ptr<pathtrace::Scene> scene,
              const pathtrace::Settings& settings) {
    pathtrace::Job job;
    job.start(std::move(scene), settings);
    while (job.running()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::vector<unsigned char> px;
    if (!job.snapshotLdr(px)) { check(false, "render produced no image"); return; }
    stbi_write_png(file.string().c_str(), settings.width, settings.height, 4,
                   px.data(), settings.width * 4);
    std::printf("  wrote %s (%.2fs, %lld tris, build %.2fs)\n",
                file.string().c_str(), job.elapsedSeconds(),
                job.triangleCount(), job.buildSeconds());
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path outDir = (argc > 1) ? argv[1] : ".";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    const grassfield::Field f = testField(/*flat=*/true);

    std::printf("Placement\n");
    {
        std::vector<float> a, b;
        grassfield::generateTile(3, -7, glm::vec2(3 * 12.0f, -7 * 12.0f), 12.0f, f, a);
        grassfield::generateTile(3, -7, glm::vec2(3 * 12.0f, -7 * 12.0f), 12.0f, f, b);
        check(!a.empty(), "a tile of flat, moist ground grows blades",
              std::to_string(a.size() / 7) + " blades");
        check(a == b, "the same tile twice is the same blades");

        // Every blade a tile produces has to be INSIDE it (plus the position
        // jitter), or two neighbouring tiles would each place blades in the
        // other's ground and the overlap would double the field where they meet.
        const float ox = 3 * 12.0f, oz = -7 * 12.0f;
        const float slack = 0.6f * 2.6f; // spacing * (2.2 + 0.4 * chaos) at chaos 1
        bool inside = true;
        for (std::size_t i = 0; i + 6 < a.size(); i += 7) {
            if (a[i] < ox - slack || a[i] > ox + 12.0f + slack ||
                a[i + 2] < oz - slack || a[i + 2] > oz + 12.0f + slack)
                inside = false;
        }
        check(inside, "a tile's blades stay in its own ground");
    }

    std::printf("Blades\n");
    {
        grassfield::TraceReport rep;
        auto scene = grassScene(8.0f, 0.0f, /*flat=*/true, 16, 0.0f, 0.4f, &rep);
        check(rep.blades > 0, "the field reaches the tracer",
              std::to_string(rep.blades) + " blades");
        check(rep.triangles == rep.blades * 5,
              "five triangles a blade",
              std::to_string(rep.triangles) + " for " + std::to_string(rep.blades));
        check(rep.materials > 1 && rep.materials < 2000,
              "the colours quantise to a bounded palette",
              std::to_string(rep.materials) + " materials");
        check(!rep.truncated, "the ceiling is not in the way at 8 m");

        // A blade stands on the ground and does not reach the sky. Measured
        // against the terrain, not against zero: even the flat world's height is
        // whatever the generator's constant term happens to be, and a test that
        // assumed it was 0 would pass for the wrong reason. The field is 0.35 m
        // before jitter and the tallest outlier is a 2x seed stalk on a 1.2x
        // tuft, so anything past a metre is a transform gone wrong.
        const float ground = fitzel::terrainHeight(f.terrain, 0.0f, 0.0f);
        float lo = 1e9f, hi = -1e9f;
        for (std::size_t t = 2; t < scene->triangles.size(); ++t) {
            const pathtrace::Triangle& tri = scene->triangles[t];
            lo = std::min({lo, tri.p0.y, tri.p1.y, tri.p2.y});
            hi = std::max({hi, tri.p0.y, tri.p1.y, tri.p2.y});
        }
        check(std::fabs(lo - ground) < 0.01f, "blades stand on the ground",
              "lowest vertex " + std::to_string(lo - ground) + " m above it");
        check(hi - ground > 0.05f && hi - ground < 1.0f, "blades are blade-sized",
              "tallest vertex " + std::to_string(hi - ground) + " m above it");

        // The disc is round. A tile grid is not, and a blade outside the radius
        // is one the caller did not ask to pay for.
        float far = 0.0f;
        for (std::size_t t = 2; t < scene->triangles.size(); ++t) {
            const glm::vec3& p = scene->triangles[t].p0;
            far = std::max(far, glm::length(glm::vec2(p.x, p.z)));
        }
        check(far <= 8.05f, "nothing outside the radius",
              "furthest blade " + std::to_string(far) + " m");

        // A blade is a blade of grass, not a leaf. grass.vert makes it
        // 2 * 0.016 * widthVar wide at the base, so ~5 cm at the broadest --
        // and a blade an order of magnitude wider would still render as a
        // perfectly plausible meadow, only of the wrong plant.
        float widest = 0.0f;
        for (std::size_t t = 2; t + 4 < scene->triangles.size(); t += 5) {
            const pathtrace::Triangle& base = scene->triangles[t];
            widest = std::max(widest, glm::length(base.p1 - base.p0));
        }
        check(widest > 0.015f && widest < 0.06f, "blades are grass-width",
              "widest " + std::to_string(widest * 100.0f) + " cm");
    }

    std::printf("Wind\n");
    {
        grassfield::TraceReport r0, r1, r2;
        auto s0 = grassScene(4.0f, 0.0f, /*flat=*/true, 16, 0.0f, 0.4f, &r0);
        auto s1 = grassScene(4.0f, 0.0f, /*flat=*/true, 16, 0.0f, 0.4f, &r1);
        auto s2 = grassScene(4.0f, 3.5f, /*flat=*/true, 16, 0.0f, 0.4f, &r2);
        bool same = s0->triangles.size() == s1->triangles.size();
        for (std::size_t i = 0; same && i < s0->triangles.size(); ++i)
            same = s0->triangles[i].p0 == s1->triangles[i].p0;
        check(same, "the same instant of wind is the same geometry");

        bool moved = s0->triangles.size() == s2->triangles.size();
        bool any = false;
        for (std::size_t i = 0; moved && i < s0->triangles.size(); ++i)
            if (s0->triangles[i].p2 != s2->triangles[i].p2) { any = true; break; }
        check(any, "a different instant bends them somewhere else");
    }

    std::printf("Pictures\n");
    {
        pathtrace::Settings s;
        s.width = 640; s.height = 360;
        s.samples = 48; s.maxBounces = 3; s.seed = 7u;
        // The same meadow at two quantisations. Whether the palette is coarse
        // enough to SEE is not a thing to have an opinion about: render both and
        // look. The colour count beside each is what it costs.
        for (int steps : {16, 64}) {
            grassfield::TraceReport rep;
            auto scene = grassScene(14.0f, 0.0f, /*flat=*/true, steps, 0.0f, 0.4f, &rep);
            std::printf("  %d steps: %lld blades, %lld tris, %d colours\n",
                        steps, rep.blades, rep.triangles, rep.materials);
            writePng(outDir / ("grasscheck-field-" + std::to_string(steps) + ".png"),
                     scene, s);
        }

        // What the translucency is worth, as a picture rather than as a claim.
        // 0 is the opaque sheet the first traced meadow was.
        for (float t : {0.0f, 0.4f, 0.7f}) {
            char name[64];
            std::snprintf(name, sizeof(name), "grasscheck-translucency-%02d.png",
                          static_cast<int>(t * 10.0f));
            writePng(outDir / name,
                     grassScene(14.0f, 0.0f, /*flat=*/true, 64, 0.0f, t, nullptr), s);
        }

        // The ribbon's own normal against grass.vert's up-biased one. Which of
        // the two a render should use is a judgement about the picture, so it is
        // made by looking at two pictures rather than by arguing from the shader.
        writePng(outDir / "grasscheck-upbias.png",
                 grassScene(14.0f, 0.0f, /*flat=*/true, 64, 1.2f, 0.4f, nullptr), s);

        grassfield::TraceReport wrep;
        auto windy = grassScene(14.0f, 2.4f, /*flat=*/true, 64, 0.0f, 0.4f, &wrep);
        writePng(outDir / "grasscheck-wind.png", windy, s);

        // The tracer's own diagnostics, for the same reason it has them: a dark
        // meadow could be its albedo, its normals or the light reaching it, and
        // the finished picture cannot tell those apart. Each of these removes
        // everything after one stage, so the first one that looks wrong is the
        // stage that is wrong.
        pathtrace::Settings d = s;
        d.samples = 8;
        d.show = pathtrace::Show::BaseColor;
        writePng(outDir / "grasscheck-albedo.png",
                 grassScene(14.0f, 0.0f, /*flat=*/true, 64, 0.0f, 0.4f, nullptr), d);
        d.show = pathtrace::Show::Normal;
        writePng(outDir / "grasscheck-normal.png",
                 grassScene(14.0f, 0.0f, /*flat=*/true, 64, 0.0f, 0.4f, nullptr), d);
    }

    std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
                g_failures);
    return g_failures ? 1 : 0;
}
