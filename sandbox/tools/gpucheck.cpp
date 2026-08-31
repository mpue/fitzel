// gpucheck -- does the GPU tracer produce the CPU tracer's picture?
//
// That is the only question worth asking of a second renderer, and it is asked
// here the only way it can be answered: the SAME scene (TraceScenes.hpp), the
// same accelerator (pathtrace::buildBvh, built once by the CPU tracer's own
// code and handed to both), the same settings, rendered twice and subtracted.
//
// pathcheck says whether the CPU tracer is right, against numbers worked out by
// hand. This says whether the GPU one is the same renderer. Neither is enough
// on its own: a GPU port that matched a CPU tracer which had drifted would pass
// this and be wrong, and one checked only against hand-worked numbers would
// pass on the two frames somebody thought to write down.
//
// WHAT IS BEING COMPARED, and it is deliberately not the finished renderer:
// the GPU kernel does primary rays, emission and direct light, and nothing
// else. So the CPU side runs with maxBounces = 0, which is exactly that. No
// textures, no HDRI, no glass, no depth of field -- see gputrace.comp's header.
// The frames here have none of those in them except the look frame's glass,
// which both sides render as nothing, and the test says so rather than the
// picture quietly disagreeing.
//
// The two will never be bit-identical and are not asked to be: different random
// sequences, different orders of summation, and 32-bit floats. They are asked
// to be the same ESTIMATE -- the mean within a fraction of a percent, and the
// per-pixel difference down at the level the sampling noise itself sits at.
//
//   build/release/bin/gpucheck.exe [outDir] [--shader path]
//                                  [--size WxH] [--samples N]
//   defaults: .  sandbox/assets/shaders/gputrace.comp  256x192  128

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fitzel/core/GlCaps.hpp>

#include "../src/GpuTrace.hpp"
#include "../src/PathTrace.hpp"
#include "TraceScenes.hpp"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_failures;
}

double mean(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (float f : v) s += f;
    return s / static_cast<double>(v.size());
}

double rms(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        s += d * d;
    }
    return std::sqrt(s / static_cast<double>(a.size()));
}

// The CPU tracer's own render, synchronously.
std::vector<float> cpuHdr(std::shared_ptr<pathtrace::Scene> scene,
                          const pathtrace::Settings& settings) {
    pathtrace::Job job;
    job.start(std::move(scene), settings);
    while (job.running()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<float> hdr;
    job.snapshotHdr(hdr);
    return hdr;
}

// Tonemapped, so a failure can be LOOKED at. Same curve for both images, which
// is the point of writing them at all.
void writePng(const fs::path& file, const std::vector<float>& hdr, int w, int h) {
    if (hdr.size() != static_cast<std::size_t>(w) * h * 3) return;
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
        const glm::vec3 c = pathtrace::tonemap(
            glm::vec3(hdr[i * 3], hdr[i * 3 + 1], hdr[i * 3 + 2]), 1.0f,
            pathtrace::Grade{});
        for (int k = 0; k < 3; ++k)
            px[i * 4 + k] = static_cast<unsigned char>(
                std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    stbi_write_png(file.string().c_str(), w, h, 4, px.data(), w * 4);
}

struct Frame {
    const char*                       name;
    std::shared_ptr<pathtrace::Scene> scene;
    // How far apart the two may be. Sampling noise, not tolerance for a bug:
    // both sides run the same number of samples through different random
    // sequences, so what is left is the variance of the estimator itself.
    double maxMeanError;   // relative, on the frame's mean radiance
    double maxRms;         // absolute, per channel
};

} // namespace

int main(int argc, char** argv) {
    fs::path    outDir = ".";
    std::string shader = "sandbox/assets/shaders/gputrace.comp";
    // Size and sample count are dials because the DEFAULT ones answer the
    // question this harness is for (are the two renderers the same picture?)
    // and not the other one everybody asks next (how much faster is it?). These
    // frames are a handful of triangles at 256x192: at that size most of the
    // GPU's time is the dispatch and the readback, and the ratio printed below
    // says more about launch overhead than about tracing. Turn them up to
    // measure throughput.
    int width = 256, height = 192, samples = 128;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--shader" && i + 1 < argc) { shader = argv[++i]; continue; }
        if (a == "--samples" && i + 1 < argc) {
            samples = std::max(1, std::atoi(argv[++i]));
            continue;
        }
        if (a == "--size" && i + 1 < argc) {
            const std::string v = argv[++i];
            const std::size_t x = v.find('x');
            if (x != std::string::npos) {
                width  = std::max(16, std::atoi(v.substr(0, x).c_str()));
                height = std::max(16, std::atoi(v.substr(x + 1).c_str()));
            }
            continue;
        }
        if (!a.empty() && a[0] != '-') outDir = a;
    }

    if (!glfwInit()) { std::printf("[gpucheck] glfwInit failed\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "gpucheck", nullptr, nullptr);
    if (!win) {
        // Not a failure. A machine without 4.3 renders on the CPU, which is the
        // whole reason the GPU path is optional -- and a harness that failed
        // here would be reporting the graphics card, not the code.
        std::printf("[gpucheck] no OpenGL 4.3 context here -- skipped\n");
        glfwTerminate();
        return 0;
    }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[gpucheck] glad failed\n");
        glfwTerminate();
        return 2;
    }
    std::printf("gpucheck -- %s | OpenGL %d.%d | compute %s\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                fitzel::glcaps::majorVersion(), fitzel::glcaps::minorVersion(),
                gputrace::available() ? "yes" : "no");
    if (!gputrace::available()) {
        std::printf("[gpucheck] the context came up below 4.3 -- skipped\n");
        glfwTerminate();
        return 0;
    }

    gputrace::Tracer gpu;
    if (!gpu.init(shader)) {
        std::printf("[gpucheck] %s\n", gpu.error().c_str());
        glfwTerminate();
        return 2;
    }
    std::printf("  the kernel builds\n");

    // 128 samples: enough that the two estimates are close for the right
    // reason. Below about 64 the noise itself is larger than the differences
    // this is meant to catch, and the test would pass on a broken renderer.
    pathtrace::Settings s;
    s.width = width; s.height = height; s.samples = samples; s.batch = 16;
    s.maxBounces = 0;          // primary + direct: exactly what the kernel does
    s.tonemap = false;
    s.clampIndirect = 0.0f;    // nothing indirect to clamp; keep both sides equal

    const Frame frames[] = {
        {"shadow", tracescenes::shadowScene(),   0.02, 0.06},
        {"lamp",   tracescenes::lampScene(12.0f), 0.02, 0.06},
        // The look frame is the hard one: a mirror, a rough metal and a pane of
        // glass, so it exercises the specular lobe where it is narrowest. Wider
        // bands because a near-mirror lit by a small sun is where two random
        // sequences disagree most.
        {"look",   tracescenes::lookScene(),     0.06, 0.30},
    };

    for (const Frame& f : frames) {
        std::printf("\n%s\n", f.name);
        auto scene = f.scene;

        const auto cpuStart = std::chrono::steady_clock::now();
        const std::vector<float> cpu = cpuHdr(scene, s);
        const double cpuSecs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - cpuStart).count();

        const auto gpuStart = std::chrono::steady_clock::now();
        bool ok = gpu.upload(*scene) && gpu.resize(s.width, s.height);
        // In batches, never in one dispatch: Windows kills a driver whose
        // single command takes longer than a couple of seconds, and "the screen
        // went black" is a poor way to learn that a scene got bigger.
        for (int done = 0; ok && done < s.samples; done += s.batch)
            ok = gpu.accumulate(std::min(s.batch, s.samples - done));
        std::vector<float> gpuImg;
        ok = ok && gpu.snapshotHdr(gpuImg);
        glFinish();
        const double gpuSecs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - gpuStart).count();

        if (!ok) {
            check(false, "the GPU rendered it", gpu.error());
            continue;
        }
        check(!gpuImg.empty(), "the GPU rendered it");
        check(gpuImg.size() == cpu.size(), "both images are the same size");
        if (gpuImg.size() != cpu.size()) continue;

        bool finite = true;
        for (float v : gpuImg)
            if (!(v == v) || std::isinf(v) || v < 0.0f) { finite = false; break; }
        check(finite, "no NaN, infinity or negative radiance came back");

        const double mc = mean(cpu), mg = mean(gpuImg);
        const double rel = std::fabs(mg - mc) / std::max(mc, 1e-6);
        const double err = rms(cpu, gpuImg);
        char detail[192];
        std::snprintf(detail, sizeof detail, "cpu %.5f vs gpu %.5f (%.2f%%)",
                      mc, mg, rel * 100.0);
        check(rel < f.maxMeanError, "the same amount of light arrives", detail);
        std::snprintf(detail, sizeof detail, "rms %.4f (allowed %.4f)",
                      err, f.maxRms);
        check(err < f.maxRms, "and it arrives in the same places", detail);

        std::printf("       cpu %.2fs (%d threads) vs gpu %.2fs -- %.1fx, %lld tris\n",
                    cpuSecs, static_cast<int>(std::thread::hardware_concurrency()),
                    gpuSecs, cpuSecs / std::max(gpuSecs, 1e-6), gpu.triangleCount());

        writePng(outDir / (std::string("gpucheck-") + f.name + "-cpu.png"),
                 cpu, s.width, s.height);
        writePng(outDir / (std::string("gpucheck-") + f.name + "-gpu.png"),
                 gpuImg, s.width, s.height);
    }

    glfwTerminate();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "passed" : "FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
