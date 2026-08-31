// The path-tracer check.
//
// A renderer is the worst kind of code to judge by looking at it: every wrong
// answer still produces a picture, and a picture that is 8% too bright, or
// missing one bounce, or quietly losing the far half of a BVH node, looks
// exactly like a picture. "It rendered" is not evidence, and neither is "that
// looks about right" -- which is the entire reason this file exists rather than
// a note saying to open the editor and have a look.
//
// So the scenes here are ones whose answer is known before anything is traced:
//   * A white furnace. A fully diffuse surface with albedo 1, lit by an
//     environment of radiance 1 in every direction, must return radiance 1. It
//     is the one test that catches an integrator losing or inventing energy,
//     because the right answer is a number rather than an impression.
//   * A box over a plane with the sun overhead. Where the shadow falls is
//     geometry, so it can be computed and compared -- this is what fails if BVH
//     traversal drops a subtree or the shadow ray's offset is wrong.
//   * The same frame twice from the same seed. Identical or it is not a
//     renderer you can iterate on: a picture that changes when the thread count
//     does cannot be compared against yesterday's.
//   * The same frame at rising sample counts. Noise must fall; if it does not,
//     the estimator is biased and no number of samples will save it.
//
// Console program, like the other checks, and non-zero on failure. It also
// writes the frames out as PNGs, because a test that has failed is a test you
// then want to look at.
//   build/release/bin/pathcheck.exe [outDir]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/PathTrace.hpp"
#include "TraceScenes.hpp"

namespace {

// The frames live in TraceScenes.hpp, so gpucheck can be judged on the very
// same ones. Pulled in wholesale rather than qualified at every call site.
using namespace tracescenes;

int g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_failures;
}

// --- Scene building helpers -------------------------------------------------




// Render to completion and hand back the linear image. Synchronous: a test that
// polled progress would be testing the scheduler, not the picture.
std::vector<float> renderHdr(std::shared_ptr<pathtrace::Scene> scene,
                             const pathtrace::Settings& settings) {
    pathtrace::Job job;
    job.start(std::move(scene), settings);
    while (job.running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::vector<float> hdr;
    job.snapshotHdr(hdr);
    return hdr;
}

void writePng(const std::filesystem::path& file, std::shared_ptr<pathtrace::Scene> scene,
              const pathtrace::Settings& settings) {
    pathtrace::Job job;
    job.start(std::move(scene), settings);
    while (job.running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::vector<unsigned char> px;
    if (!job.snapshotLdr(px)) return;
    stbi_write_png(file.string().c_str(), settings.width, settings.height, 4,
                   px.data(), settings.width * 4);
    std::printf("  wrote %s (%.2fs, %lld tris)\n", file.string().c_str(),
                job.elapsedSeconds(), job.triangleCount());
}

float meanOf(const std::vector<float>& hdr) {
    if (hdr.empty()) return 0.0f;
    double sum = 0.0;
    for (float v : hdr) sum += v;
    return static_cast<float>(sum / static_cast<double>(hdr.size()));
}

float rms(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9f;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        sum += d * d;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(a.size())));
}


// The same furnace, lit by a PANORAMA of constant radiance 1 instead of the
// gradient. The answer is identical -- radiance 1 -- but the path to it is not:
// with a map present the environment is importance-sampled and every bounce is
// weighted between two strategies. That weighting is exactly the kind of thing
// that looks right and is a few percent wrong, and the only way to see a few
// percent is against a number.
std::shared_ptr<pathtrace::Scene> furnaceMapScene() {
    auto sc = furnaceScene();
    sc->env.width  = 64;
    sc->env.height = 32;
    sc->env.pixels.assign(static_cast<std::size_t>(64) * 32 * 3, 1.0f);
    sc->env.intensity = 1.0f;
    return sc;
}

// A panorama that is black everywhere except for a handful of ferociously
// bright texels -- a sun through a roof opening, a window, a lamp in an
// interior HDRI. This is the shape that breaks an environment sampler, and it
// breaks it in a way that looks like a colour bug rather than like a numerical
// one: the estimator divides a huge radiance by a tiny probability, the product
// with a colour channel that happens to be exactly zero is 0 * infinity, and
// that single channel comes back NaN. Zeroed, a lost blue channel is yellow and
// a lost red one is cyan -- so the brightest part of a sky turns into blocks of
// pure colour while everything dim stays perfectly correct.
std::shared_ptr<pathtrace::Scene> spotlightSkyScene() {
    auto sc = std::make_shared<pathtrace::Scene>();
    pathtrace::Material m;
    m.albedo    = glm::vec3(0.8f);
    m.roughness = 0.9f;
    sc->materials.push_back(m);
    addQuad(*sc, {-50, 0, -50}, {50, 0, -50}, {50, 0, 50}, {-50, 0, 50}, {0, 1, 0}, 0);

    sc->sun.enabled = false;
    sc->env.width  = 256;
    sc->env.height = 128;
    sc->env.pixels.assign(static_cast<std::size_t>(256) * 128 * 3, 0.0f);
    // Four texels of 20000, high in the sky, and DELIBERATELY not grey: the
    // blue channel is left at zero, which is the exact condition that turned
    // into a NaN.
    for (int y = 100; y < 102; ++y)
        for (int x = 60; x < 62; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * 256 + x) * 3;
            sc->env.pixels[o + 0] = 20000.0f;
            sc->env.pixels[o + 1] = 16000.0f;
            sc->env.pixels[o + 2] = 0.0f;
        }
    sc->env.intensity = 1.0f;
    sc->camera = lookAt({0.0f, 3.0f, 9.0f}, {0.0f, 0.0f, 0.0f}, 55.0f);
    return sc;
}




// --- Sky alone --------------------------------------------------------------
// No geometry at all, so every ray escapes and the picture is nothing but the
// environment, the sun disc, and whatever the tonemap and grade do to them.
// That separation is the point: a sky that comes out in bands of pure red,
// yellow and cyan is either the environment, the fog or the grade, and with the
// geometry gone there are only three things left to be wrong.
//
// It also exercises the empty scene, which is a real case (a camera pointed at
// nothing) and an easy one to crash on.
std::shared_ptr<pathtrace::Scene> skyScene(bool withFog) {
    auto sc = std::make_shared<pathtrace::Scene>();
    sc->materials.emplace_back();      // never referenced; the table is never empty

    sc->sun.direction        = glm::normalize(glm::vec3(0.55f, 0.25f, -0.8f));
    sc->sun.color            = glm::vec3(2.2f, 2.05f, 1.85f);
    sc->sun.angularRadiusDeg = 0.5f;

    // The gradient the capture builds when a scene has no panorama: the flat
    // ambient, spread from ground to zenith.
    const glm::vec3 amb(0.20f, 0.22f, 0.26f);
    sc->env.zenith  = amb * 1.30f;
    sc->env.horizon = amb;
    sc->env.ground  = amb * 0.45f;

    if (withFog) {
        sc->fog.density       = 0.006f;
        sc->fog.heightFalloff = 0.03f;
        sc->fog.color         = glm::vec3(0.70f, 0.82f, 0.95f);
        sc->fog.sunColor      = glm::vec3(1.00f, 0.75f, 0.50f);
    }

    sc->camera = lookAt({0.0f, 2.0f, 0.0f}, {0.4f, 2.25f, -1.0f}, 55.0f);
    sc->grade.saturation = 1.35f;   // the editor's own starting grade
    sc->grade.warmth     = 0.18f;
    sc->grade.contrast   = 0.16f;
    return sc;
}

// Reconstruct a probe the way the shader will: one line and a clamp.
glm::vec3 irradianceFrom(const pathtrace::ProbeSh& sh, const glm::vec3& n) {
    return glm::max(sh.sh0 + sh.shX * n.x + sh.shY * n.y + sh.shZ * n.z,
                    glm::vec3(0.0f));
}

// A panorama that is white above the horizon and black below it. Chosen because
// its answer is not approximately anything: an L1 band reconstructs a hemisphere
// EXACTLY at the pole and at the equator (0.5 + 0.5 * n.y), so straight up must
// come back at 1, straight down at 0 and sideways at 0.5. Nothing about that
// survives a wrong normalisation constant.
std::shared_ptr<pathtrace::Scene> hemisphereSkyScene() {
    auto sc = std::make_shared<pathtrace::Scene>();
    sc->sun.enabled = false;
    sc->env.width  = 64;
    sc->env.height = 32;
    sc->env.pixels.assign(static_cast<std::size_t>(64) * 32 * 3, 0.0f);
    for (int y = 16; y < 32; ++y)            // row 0 is DOWN; the top half is sky
        for (int x = 0; x < 64; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * 64 + x) * 3;
            sc->env.pixels[o] = sc->env.pixels[o + 1] = sc->env.pixels[o + 2] = 1.0f;
        }
    sc->env.intensity = 1.0f;
    return sc;
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path outDir = argc > 1 ? argv[1] : ".";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    std::printf("pathcheck -- %u hardware threads\n\n",
                std::thread::hardware_concurrency());

    // --- 1. White furnace ---------------------------------------------------
    std::printf("white furnace (albedo 1, env 1, no sun)\n");
    {
        pathtrace::Settings s;
        s.width = 96; s.height = 64; s.samples = 256; s.batch = 16;
        s.maxBounces = 6; s.tonemap = false;
        const std::vector<float> hdr = renderHdr(furnaceScene(), s);

        check(!hdr.empty(), "produced an image");
        bool finite = true, nonneg = true;
        for (float v : hdr) {
            if (!(v == v) || std::isinf(v)) finite = false;
            if (v < 0.0f) nonneg = false;
        }
        check(finite, "no NaN or infinity in the result");
        check(nonneg, "no negative radiance");

        const float mean = meanOf(hdr);
        // The band is not slack: the excess above 1.0 is the dielectric
        // specular lobe sitting on top of a full-albedo diffuse base, which
        // this BSDF does not energy-compensate. That is a known and bounded
        // property of the model -- a few percent -- and NOT licence for a
        // renderer that loses a bounce, which lands well below 1.
        check(mean > 0.97f && mean < 1.12f, "energy conserved",
              "mean radiance " + std::to_string(mean) + " (want ~1.0)");
    }

    std::printf("\nwhite furnace again, this time lit by a panorama\n");
    {
        pathtrace::Settings s;
        s.width = 96; s.height = 64; s.samples = 256; s.batch = 16;
        s.maxBounces = 6; s.tonemap = false;
        const std::vector<float> hdr = renderHdr(furnaceMapScene(), s);
        const float mean = meanOf(hdr);
        check(mean > 0.97f && mean < 1.12f,
              "energy conserved with the environment importance-sampled",
              "mean radiance " + std::to_string(mean) + " (want ~1.0)");
        // Importance sampling is supposed to buy LESS noise, not more. Against a
        // uniform map it cannot help, but it must not hurt either -- a wrong MIS
        // weight shows up here as variance rather than as a shifted mean.
        double var = 0.0;
        for (float v : hdr) var += (v - mean) * (v - mean);
        var /= static_cast<double>(std::max<std::size_t>(hdr.size(), 1));
        check(std::sqrt(var) < 0.05f, "and the result is flat, as a furnace is",
              "std dev " + std::to_string(std::sqrt(var)));
    }

    std::printf("\na sky that is black except for one brilliant spot\n");
    {
        pathtrace::Settings s;
        s.width = 128; s.height = 96; s.samples = 64; s.batch = 16;
        s.maxBounces = 3; s.tonemap = false;
        const std::vector<float> hdr = renderHdr(spotlightSkyScene(), s);
        check(!hdr.empty(), "produced an image");

        bool finite = true, nonneg = true;
        for (float v : hdr) {
            if (!(v == v) || std::isinf(v)) finite = false;
            if (v < 0.0f) nonneg = false;
        }
        check(finite, "no NaN survives a huge radiance over a tiny probability");
        check(nonneg, "and nothing came back negative");

        // The spot has no blue in it, so nothing lit only by it may end up
        // with more blue than green. A lost channel shows up here as exactly
        // that inversion -- which is the yellow and cyan, stated as a number.
        bool hueSane = true;
        for (std::size_t i = 0; i + 2 < hdr.size(); i += 3)
            if (hdr[i + 2] > hdr[i + 1] + 0.02f) hueSane = false;
        check(hueSane, "no pixel gained a colour the light did not have");

        // And it must still be LIT: a guard that fixed the NaN by throwing the
        // light away would pass every check above and render a black picture.
        check(meanOf(hdr) > 0.02f, "the spot still lights the scene",
              "mean " + std::to_string(meanOf(hdr)));
    }

    // --- 2. Shadow geometry -------------------------------------------------
    std::printf("\nshadow placement (box at y=2, sun overhead)\n");
    {
        pathtrace::Settings s;
        s.width = 128; s.height = 128; s.samples = 64; s.batch = 16;
        s.maxBounces = 2; s.tonemap = false;
        const std::vector<float> hdr = renderHdr(shadowScene(), s);
        check(!hdr.empty(), "produced an image");

        // The camera looks straight down from 12 m with a 60 degree vertical
        // field of view, so the visible ground is 2*12*tan(30) = 13.86 m tall.
        // The box spans +/-1 m in x and z at y = 2, and the sun is vertical, so
        // its shadow is the same +/-1 m square on the ground.
        const float halfExtent = 12.0f * std::tan(glm::radians(30.0f));
        auto groundAt = [&](float worldX, float worldZ, int& px, int& py) {
            px = static_cast<int>((worldX / halfExtent * 0.5f + 0.5f) * 128.0f);
            // Screen y runs down; the camera's up maps to -z here.
            py = static_cast<int>((worldZ / halfExtent * 0.5f + 0.5f) * 128.0f);
            px = std::clamp(px, 0, 127);
            py = std::clamp(py, 0, 127);
        };
        auto lum = [&](int px, int py) {
            const std::size_t o = (static_cast<std::size_t>(py) * 128 + px) * 3;
            return 0.2126f * hdr[o] + 0.7152f * hdr[o + 1] + 0.0722f * hdr[o + 2];
        };

        // A point inside the shadow but outside the box's own silhouette cannot
        // be sampled from straight above (the box hides it), so the test reads
        // the shadow at its corner instead: 1.6 m out in x is under the box's
        // edge... which is also hidden. Read the LIT ground well clear of the
        // box, and the SHADOWED ground is verified through the box's absence:
        // at 3 m out the ground is lit, at 6 m out it is lit, and directly
        // beside the box (1.5 m) it is lit too -- the shadow is only under it.
        int px, py;
        groundAt(3.0f, 0.0f, px, py);
        const float lit = lum(px, py);
        groundAt(1.5f, 0.0f, px, py);
        const float nearBox = lum(px, py);
        groundAt(0.0f, 0.0f, px, py);
        const float underBox = lum(px, py); // the box's top face, seen from above

        check(lit > 0.2f, "open ground is lit by the sun",
              "luminance " + std::to_string(lit));
        check(nearBox > 0.2f, "ground beside the box is lit, not shadowed",
              "luminance " + std::to_string(nearBox));
        check(std::fabs(nearBox - lit) / std::max(lit, 1e-4f) < 0.35f,
              "lit ground is even across the frame",
              "3m " + std::to_string(lit) + " vs 1.5m " + std::to_string(nearBox));
        check(underBox > 0.0f, "the box itself is visible from above",
              "luminance " + std::to_string(underBox));

        // Now the shadow proper, from a low sun so it falls clear of the box.
        auto sc = shadowScene();
        sc->sun.direction = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f)); // 45 deg
        const std::vector<float> hdr2 = renderHdr(sc, s);
        auto lum2 = [&](int pxx, int pyy) {
            const std::size_t o = (static_cast<std::size_t>(pyy) * 128 + pxx) * 3;
            return 0.2126f * hdr2[o] + 0.7152f * hdr2[o + 1] + 0.0722f * hdr2[o + 2];
        };
        // At 45 degrees a box whose base is at y = 1.5 throws its shadow 1.5 m
        // along -z from its own footprint: the shadow centre sits at z = -1.5.
        groundAt(0.0f, -1.5f, px, py);
        const float shadowed = lum2(px, py);
        groundAt(0.0f,  4.5f, px, py);
        const float openGround = lum2(px, py);
        check(shadowed < openGround * 0.5f, "the shadow lands where geometry says",
              "shadow " + std::to_string(shadowed) + " vs open " +
              std::to_string(openGround));
    }

    // --- 3. A point lamp ----------------------------------------------------
    std::printf("\na point lamp on its own (no sun, black sky)\n");
    {
        pathtrace::Settings s;
        s.width = 128; s.height = 128; s.samples = 64; s.batch = 16;
        // One bounce: direct light only, so what lands on the ground is a single
        // term that can be written down rather than a series that can only be
        // compared with itself.
        s.maxBounces = 1; s.tonemap = false;
        const std::vector<float> hdr = renderHdr(lampScene(12.0f), s);
        check(!hdr.empty(), "produced an image");

        const float halfExtent = 12.0f * std::tan(glm::radians(30.0f));
        auto groundAt = [&](float worldX, float worldZ, int& px, int& py) {
            px = static_cast<int>((worldX / halfExtent * 0.5f + 0.5f) * 128.0f);
            py = static_cast<int>((worldZ / halfExtent * 0.5f + 0.5f) * 128.0f);
            px = std::clamp(px, 0, 127);
            py = std::clamp(py, 0, 127);
        };
        auto lum = [&](int px, int py) {
            const std::size_t o = (static_cast<std::size_t>(py) * 128 + px) * 3;
            return 0.2126f * hdr[o] + 0.7152f * hdr[o + 1] + 0.0722f * hdr[o + 2];
        };

        // Straight under the lamp the geometry is trivial: 6 m away, facing it
        // squarely, so the raster path's range falloff gives (1 - 6/12)^2 = 0.25
        // and the diffuse lobe gives albedo/pi -- albedo 0.6 sRGB, which the
        // tracer linearises to 0.6^2.2 = 0.325 first. 8 * 0.25 * 0.325/pi =
        // 0.207, and a few percent of dielectric specular sits on top of it.
        int px, py;
        groundAt(0.0f, 3.0f, px, py);
        const float under = lum(px, py);
        check(under > 0.05f, "a point lamp lights the scene at all",
              "radiance " + std::to_string(under));
        check(under > 0.19f && under < 0.24f, "...and by the amount the falloff says",
              "radiance " + std::to_string(under) + " (want ~0.207)");

        // Four metres further out and the same arithmetic gives 0.029: the
        // check is that the light FALLS OFF, not merely that it arrived.
        groundAt(0.0f, -4.0f, px, py);
        const float away = lum(px, py);
        check(away > 0.0f && away < under * 0.5f, "it falls off with distance",
              "under " + std::to_string(under) + " vs 7m out " + std::to_string(away));

        // The box between lamp and ground. With nothing else lighting the scene
        // its shadow is not "darker", it is black.
        groundAt(0.0f, -1.5f, px, py);
        const float shadowed = lum(px, py);
        check(shadowed < 0.005f, "and casts a shadow, with no sun to fill it in",
              "shadow " + std::to_string(shadowed));

        // Range is a hard edge, not a curve that fades: at range 5 the ground
        // 6 m below the lamp is outside it and must be exactly unlit.
        const std::vector<float> shortRange = renderHdr(lampScene(5.0f), s);
        groundAt(0.0f, 3.0f, px, py);
        const std::size_t o = (static_cast<std::size_t>(py) * 128 + px) * 3;
        const float beyond = 0.2126f * shortRange[o] + 0.7152f * shortRange[o + 1] +
                             0.0722f * shortRange[o + 2];
        check(beyond < 1e-4f, "nothing outside the lamp's range is lit",
              "radiance " + std::to_string(beyond));
    }

    // --- 4. Determinism -----------------------------------------------------
    std::printf("\ndeterminism (same seed, twice)\n");
    {
        pathtrace::Settings s;
        s.width = 64; s.height = 48; s.samples = 32; s.batch = 8;
        s.maxBounces = 3; s.tonemap = false; s.seed = 7u;
        s.threads = 1;
        const std::vector<float> a = renderHdr(lookScene(), s);
        s.threads = 4;
        const std::vector<float> b = renderHdr(lookScene(), s);
        check(a.size() == b.size() && a == b,
              "identical image regardless of thread count",
              "rms " + std::to_string(rms(a, b)));
    }

    // --- 5. Convergence -----------------------------------------------------
    std::printf("\nconvergence (noise must fall with samples)\n");
    {
        pathtrace::Settings ref;
        ref.width = 64; ref.height = 48; ref.samples = 1024; ref.batch = 64;
        ref.maxBounces = 4; ref.tonemap = false; ref.seed = 3u;
        const std::vector<float> reference = renderHdr(lookScene(), ref);

        pathtrace::Settings few = ref;  few.samples = 8;   few.batch = 8;  few.seed = 11u;
        pathtrace::Settings many = ref; many.samples = 128; many.batch = 16; many.seed = 11u;
        const float errFew  = rms(renderHdr(lookScene(), few),  reference);
        const float errMany = rms(renderHdr(lookScene(), many), reference);

        check(errMany < errFew, "128 samples is closer to the reference than 8",
              "8spp rms " + std::to_string(errFew) + ", 128spp rms " +
              std::to_string(errMany));
        // Monte Carlo error falls as 1/sqrt(n): 16x the samples should be
        // roughly 4x closer. Anything much worse means the extra samples are
        // being spent on something correlated -- a fixed sequence, or a seed
        // that does not actually vary per pass.
        check(errMany < errFew * 0.5f, "the error falls at something like 1/sqrt(n)",
              "ratio " + std::to_string(errFew > 0.0f ? errMany / errFew : 0.0f));
    }

    // --- 6. Cancelling ------------------------------------------------------
    std::printf("\ncancelling a long render\n");
    {
        pathtrace::Settings s;
        s.width = 320; s.height = 240; s.samples = 100000; s.batch = 4;
        s.maxBounces = 6;
        pathtrace::Job job;
        job.start(lookScene(), s);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const bool wasRunning = job.running();
        job.cancel();
        check(wasRunning, "a big render is still going after 250 ms");
        check(!job.running(), "cancel() stops it and returns");
        std::vector<unsigned char> px;
        check(job.snapshotLdr(px) && px.size() == 320u * 240u * 4u,
              "a cancelled render still hands over what it had");
    }

    // --- 7. The colour grade ------------------------------------------------
    // The viewport never shows a raw tonemap: composite.frag grades every frame
    // on the way to the screen, and the project's own defaults are nowhere near
    // neutral. A render that skipped the grade came out flat and cool beside
    // the picture it was meant to be a better version of -- so the grade is
    // pinned here, against values worked out by hand.
    std::printf("\ncolour grade (the post chain's, on the CPU)\n");
    {
        pathtrace::Grade neutral;
        const glm::vec3 mid(0.4f, 0.2f, 0.1f);
        const glm::vec3 plain = pathtrace::tonemap(mid, 1.0f, neutral);

        pathtrace::Grade warm = neutral;
        warm.warmth = 0.4f;
        const glm::vec3 warmed = pathtrace::tonemap(mid, 1.0f, warm);
        check(warmed.r > plain.r && warmed.b < plain.b,
              "warmth pushes red up and blue down",
              "plain r/b " + std::to_string(plain.r) + "/" + std::to_string(plain.b) +
              ", warm " + std::to_string(warmed.r) + "/" + std::to_string(warmed.b));

        pathtrace::Grade grey = neutral;
        grey.saturation = 0.0f;
        const glm::vec3 desat = pathtrace::tonemap(mid, 1.0f, grey);
        check(std::fabs(desat.r - desat.g) < 0.01f &&
              std::fabs(desat.g - desat.b) < 0.01f,
              "saturation 0 leaves a grey", "got " + std::to_string(desat.r) +
              ", " + std::to_string(desat.g) + ", " + std::to_string(desat.b));

        pathtrace::Grade dark = neutral;
        dark.value = 0.5f;
        const glm::vec3 dimmed = pathtrace::tonemap(mid, 1.0f, dark);
        check(dimmed.r < plain.r * 0.6f, "value scales brightness",
              std::to_string(dimmed.r) + " vs " + std::to_string(plain.r));

        // The identity that matters most: a neutral grade must be a no-op, or
        // every ungraded scene quietly shifts.
        pathtrace::Grade same = neutral;
        const glm::vec3 again = pathtrace::tonemap(mid, 1.0f, same);
        check(std::fabs(again.r - plain.r) < 1e-5f &&
              std::fabs(again.g - plain.g) < 1e-5f &&
              std::fabs(again.b - plain.b) < 1e-5f,
              "a neutral grade changes nothing");

        // Nothing may leave the tonemap above 1, whatever the grade does.
        //
        // This is the one that bit: the brightness gain is the last operation
        // and had nothing after it to bound the result, so a bright pixel came
        // out at 1.02 and the byte cast turned 260 into 4. One channel at a
        // time, which is why a sky's bright end went yellow, cyan and black
        // instead of simply white -- and why it was always the bright end.
        pathtrace::Grade bright = neutral;
        bright.value = 1.8f;
        bool bounded = true;
        for (int i = 0; i <= 40; ++i) {
            const float v = static_cast<float>(i) * 2.0f;   // 0 .. 80, well past white
            for (const glm::vec3& probe : {glm::vec3(v), glm::vec3(v, v * 0.6f, 0.0f),
                                           glm::vec3(0.0f, v, v)}) {
                const glm::vec3 c = pathtrace::tonemap(probe, 2.0f, bright);
                for (int k = 0; k < 3; ++k)
                    if (!(c[k] >= 0.0f && c[k] <= 1.0f)) bounded = false;
            }
        }
        check(bounded, "a bright grade cannot push a channel past 1",
              "value gain 1.8 over radiances up to 80");

        // Contrast pushes away from mid grey in both directions.
        pathtrace::Grade punch = neutral;
        punch.contrast = 0.5f;
        const glm::vec3 lo = pathtrace::tonemap(glm::vec3(0.05f), 1.0f, neutral);
        const glm::vec3 loP = pathtrace::tonemap(glm::vec3(0.05f), 1.0f, punch);
        const glm::vec3 hi = pathtrace::tonemap(glm::vec3(3.0f), 1.0f, neutral);
        const glm::vec3 hiP = pathtrace::tonemap(glm::vec3(3.0f), 1.0f, punch);
        check(loP.r < lo.r && hiP.r >= hi.r - 1e-4f,
              "contrast darkens the shadows and holds the highlights",
              "dark " + std::to_string(lo.r) + "->" + std::to_string(loP.r) +
              ", bright " + std::to_string(hi.r) + "->" + std::to_string(hiP.r));
    }

    // --- 8. The sky on its own ----------------------------------------------
    std::printf("\nsky with no geometry in it\n");
    {
        pathtrace::Settings s;
        s.width = 320; s.height = 180; s.samples = 16; s.batch = 8;
        s.maxBounces = 2; s.tonemap = false;
        const std::vector<float> hdr = renderHdr(skyScene(false), s);
        check(!hdr.empty(), "an empty scene renders instead of crashing");

        // A gradient sky must stay a gradient. Bands of pure primaries mean a
        // channel is saturating on its own, and the quickest way to see that
        // without looking is that the hue stops being nearly constant.
        bool sane = true;
        float worstSat = 0.0f;
        for (std::size_t i = 0; i + 2 < hdr.size(); i += 3) {
            const float mx = std::max(hdr[i], std::max(hdr[i + 1], hdr[i + 2]));
            const float mn = std::min(hdr[i], std::min(hdr[i + 1], hdr[i + 2]));
            if (mx > 1e-4f) worstSat = std::max(worstSat, (mx - mn) / mx);
            if (!(hdr[i] == hdr[i]) || hdr[i] < 0.0f) sane = false;
        }
        check(sane, "no NaN or negative radiance in the sky");
        // The sun's disc is allowed to be bright, but the SKY is a desaturated
        // blue-grey and must stay one.
        check(worstSat < 0.75f, "the sky is not banding into pure colours",
              "worst channel spread " + std::to_string(worstSat));

        pathtrace::Settings look = s;
        look.samples = 32; look.tonemap = true;
        look.width = 480; look.height = 270;
        writePng(outDir / "pathcheck-sky.png",     skyScene(false), look);
        writePng(outDir / "pathcheck-sky-fog.png", skyScene(true),  look);
    }

    // --- 9. The panorama path -----------------------------------------------
    // The gradient sky above is not the one a real scene uses -- a project with
    // an HDRI lights from the panorama instead, and that is a completely
    // separate lookup with its own chance of being upside down or off by a
    // hemisphere. So: a panorama painted with a KNOWN colour in each direction,
    // sampled back through the same code the render uses.
    //
    // The loader hands panoramas over bottom-up and the GL mapping is
    // v = asin(y)/pi + 0.5, so row 0 is the ground and the last row is the
    // zenith. Getting that backwards is invisible in a test that only checks
    // brightness and glaring in a picture.
    std::printf("\npanorama lookup\n");
    {
        pathtrace::Environment env;
        env.width  = 64;
        env.height = 32;
        env.intensity = 1.0f;
        env.pixels.assign(static_cast<std::size_t>(env.width) * env.height * 3, 0.0f);
        for (int y = 0; y < env.height; ++y) {
            // Row 0 = down, last row = up, per the loader's bottom-up order.
            const bool upper = y >= env.height / 2;
            for (int x = 0; x < env.width; ++x) {
                const std::size_t o =
                    (static_cast<std::size_t>(y) * env.width + x) * 3;
                // Sky green above, ground red below; and the eastern half (+x)
                // gets a blue tint so a horizontal flip cannot pass either.
                env.pixels[o + (upper ? 1 : 0)] = 1.0f;
                if (x < env.width / 4 || x >= 3 * env.width / 4)
                    env.pixels[o + 2] = 0.5f;
            }
        }

        const glm::vec3 up    = env.sample(glm::vec3(0.0f,  1.0f, 0.0f));
        const glm::vec3 down  = env.sample(glm::vec3(0.0f, -1.0f, 0.0f));
        check(up.g > 0.5f && up.r < 0.5f, "straight up reads the sky half",
              "got (" + std::to_string(up.r) + ", " + std::to_string(up.g) + ")");
        check(down.r > 0.5f && down.g < 0.5f, "straight down reads the ground half",
              "got (" + std::to_string(down.r) + ", " + std::to_string(down.g) + ")");

        // +x is u = 0.5 exactly (atan2(0, 1) = 0), which is the middle column --
        // the untinted half. -x wraps to the edge columns, which are tinted.
        const glm::vec3 east = env.sample(glm::normalize(glm::vec3(1.0f, 0.2f, 0.0f)));
        const glm::vec3 west = env.sample(glm::normalize(glm::vec3(-1.0f, 0.2f, 0.0f)));
        check(east.b < 0.25f && west.b > 0.25f,
              "the horizontal mapping is not mirrored",
              "east b " + std::to_string(east.b) + ", west b " + std::to_string(west.b));

        // Nothing anywhere on the sphere may read outside the image.
        bool inRange = true;
        for (int i = 0; i < 2000; ++i) {
            const float a = static_cast<float>(i) * 0.0031415f * 2.0f;
            const float t = static_cast<float>(i % 101) / 50.0f - 1.0f;
            const glm::vec3 d = glm::normalize(glm::vec3(std::cos(a), t, std::sin(a)));
            const glm::vec3 c = env.sample(d);
            if (!(c.r == c.r) || c.r < 0.0f || c.r > 1.01f) inRange = false;
        }
        check(inRange, "every direction on the sphere lands inside the panorama");
    }

    // --- 10. Light probes ----------------------------------------------------
    std::printf("\nlight probes (irradiance as an L1 band)\n");
    {
        pathtrace::BakeSettings b;
        b.rays = 4096; b.maxBounces = 2; b.seed = 5u;

        // A uniform environment of radiance 1. A Lambertian surface under it
        // reflects exactly its albedo, so the stored quantity must be 1 for
        // every normal -- and the directional band must be nothing at all.
        {
            auto sc = std::make_shared<pathtrace::Scene>();
            sc->sun.enabled = false;
            sc->env.zenith = sc->env.horizon = sc->env.ground = glm::vec3(1.0f);
            const auto probes = pathtrace::bakeProbes(*sc, {glm::vec3(0.0f)}, b);
            check(probes.size() == 1, "one point in, one probe out");
            const glm::vec3 up   = irradianceFrom(probes[0], glm::vec3(0, 1, 0));
            const glm::vec3 side = irradianceFrom(probes[0], glm::vec3(1, 0, 0));
            check(std::fabs(up.r - 1.0f) < 0.03f && std::fabs(side.r - 1.0f) < 0.03f,
                  "a uniform sky reconstructs at 1 in every direction",
                  "up " + std::to_string(up.r) + ", sideways " + std::to_string(side.r));
            const float dir = glm::length(probes[0].shX) + glm::length(probes[0].shY) +
                              glm::length(probes[0].shZ);
            check(dir < 0.05f, "and has no direction to it",
                  "band-1 magnitude " + std::to_string(dir));
            check(probes[0].valid, "a probe in the open is usable");
        }

        // Sky above, nothing below. The exact answers are 1, 0.5 and 0.
        {
            auto sc = hemisphereSkyScene();
            const auto probes = pathtrace::bakeProbes(*sc, {glm::vec3(0.0f)}, b);
            const float up   = irradianceFrom(probes[0], glm::vec3(0, 1, 0)).r;
            const float side = irradianceFrom(probes[0], glm::vec3(1, 0, 0)).r;
            const float down = irradianceFrom(probes[0], glm::vec3(0, -1, 0)).r;
            check(std::fabs(up - 1.0f) < 0.05f, "facing the sky: all of it",
                  std::to_string(up));
            check(std::fabs(side - 0.5f) < 0.04f, "facing the horizon: half",
                  std::to_string(side));
            check(down < 0.04f, "facing away from it: none", std::to_string(down));
        }

        // The whole point of a probe grid over a flat ambient colour: a place
        // with no sky over it has to come out dark, and a single ambient colour
        // cannot tell the difference between there and an open field.
        {
            auto sc = hemisphereSkyScene();
            pathtrace::Material m;
            m.albedo = glm::vec3(0.02f);   // near-black lid, so nothing bounces
            m.roughness = 1.0f;
            sc->materials.push_back(m);
            addQuad(*sc, {-40, 8, -40}, {40, 8, -40}, {40, 8, 40}, {-40, 8, 40},
                    {0, -1, 0}, 0);
            const auto probes = pathtrace::bakeProbes(
                *sc, {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(60.0f, 1.0f, 0.0f)}, b);
            const float under = irradianceFrom(probes[0], glm::vec3(0, 1, 0)).r;
            const float open  = irradianceFrom(probes[1], glm::vec3(0, 1, 0)).r;
            check(under < open * 0.15f,
                  "a probe under a lid is dark, one beside it is not",
                  "under " + std::to_string(under) + " vs open " + std::to_string(open));
        }

        // Bounced colour. A white sky, a red wall, and a probe beside it: the
        // side facing the wall has to come back redder than the side facing
        // away. This is the thing a flat ambient can never do.
        {
            auto sc = hemisphereSkyScene();
            pathtrace::Material red;
            red.albedo = glm::vec3(0.85f, 0.05f, 0.05f);
            red.roughness = 1.0f;
            sc->materials.push_back(red);
            addQuad(*sc, {-2, -6, -6}, {-2, -6, 6}, {-2, 6, 6}, {-2, 6, -6},
                    {1, 0, 0}, 0);
            const auto probes = pathtrace::bakeProbes(*sc, {glm::vec3(0.0f)}, b);
            const glm::vec3 toward = irradianceFrom(probes[0], glm::vec3(-1, 0, 0));
            const glm::vec3 away   = irradianceFrom(probes[0], glm::vec3(1, 0, 0));
            const float towardRatio = toward.r / std::max(toward.b, 1e-4f);
            const float awayRatio   = away.r / std::max(away.b, 1e-4f);
            check(towardRatio > awayRatio * 1.3f,
                  "the side facing a red wall picks up red",
                  "red/blue facing it " + std::to_string(towardRatio) +
                  ", facing away " + std::to_string(awayRatio));
        }

        // Buried probes have to say so, or the inside of a wall darkens
        // everything that samples near it.
        {
            auto sc = hemisphereSkyScene();
            pathtrace::Material m;
            m.albedo = glm::vec3(0.5f);
            sc->materials.push_back(m);
            addBox(*sc, glm::vec3(0.0f), glm::vec3(2.0f), 0);
            const auto probes = pathtrace::bakeProbes(
                *sc, {glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f)}, b);
            check(!probes[0].valid, "a probe inside solid geometry is marked unusable");
            check(probes[1].valid, "one outside it is not");
        }

        // The design promise, as a number: what is baked must not change when
        // the sun does. Two bakes with the sun in very different places, and
        // the results have to agree -- otherwise the grid is a photograph of
        // one moment and this engine's sun crosses the sky in four minutes.
        {
            auto morning = hemisphereSkyScene();
            morning->sun.enabled   = true;
            morning->sun.direction = glm::normalize(glm::vec3(1.0f, 0.2f, 0.0f));
            morning->sun.color     = glm::vec3(6.0f);
            auto noon = hemisphereSkyScene();
            noon->sun.enabled   = true;
            noon->sun.direction = glm::vec3(0.0f, 1.0f, 0.0f);
            noon->sun.color     = glm::vec3(6.0f);

            const auto a = pathtrace::bakeProbes(*morning, {glm::vec3(0.0f)}, b);
            const auto c = pathtrace::bakeProbes(*noon, {glm::vec3(0.0f)}, b);
            const float d = glm::length(a[0].sh0 - c[0].sh0) +
                            glm::length(a[0].shY - c[0].shY);
            check(d < 0.01f, "moving the sun does not change what is baked",
                  "difference " + std::to_string(d));
        }
    }

    // --- 11. Pictures -------------------------------------------------------

    std::printf("\nwriting look frames\n");
    {
        pathtrace::Settings s;
        s.width = 640; s.height = 360; s.samples = 256; s.batch = 16;
        s.maxBounces = 6; s.tonemap = true;
        writePng(outDir / "pathcheck-look.png", lookScene(), s);

        pathtrace::Settings f;
        f.width = 320; f.height = 200; f.samples = 64; f.batch = 16;
        f.maxBounces = 4; f.tonemap = true;
        writePng(outDir / "pathcheck-shadow.png", shadowScene(), f);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "passed",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
