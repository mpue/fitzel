// occlusioncheck -- does hiding what is behind something ever show?
//
// Occlusion culling is the kind of feature that looks finished while it is wrong:
// the frame rate goes up in the one view anyone tries, and the house that blinks
// out as you round a corner at speed is found by somebody else, months later.
// "Looks right" is no evidence, so this measures the one thing that matters: the
// same frame drawn with the culler and without it has to be the same picture.
//
// First the CPU half alone, on made-up depth, where each answer is known. Then a
// town of a few hundred boxes through the real Renderer: every frame drawn twice
// into two targets and compared pixel by pixel, along a path meant to hurt it --
// a walk, a whip pan, a sprint, a cut, a wall driving across the street and a
// wall removed outright. Flat unlit colours, different per object, so a missing
// box is a difference and nothing else is.
//
//   build/linux/bin/occlusioncheck [--frames n] [--no-gl] [--verbose]
// Works on either backend (FITZEL_RHI=vulkan). Exits non-zero when anything hidden
// showed, or when it stops hiding what a town plainly hides.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/core/Window.hpp>
#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/render/OcclusionCuller.hpp>
#include <fitzel/render/Renderer.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/scene/Camera.hpp>

namespace {

using fitzel::OcclusionCuller;

int  g_failed  = 0;
bool g_verbose = false;   // --verbose: where each differing frame differs

void expect(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failed;
}

// --- The CPU half -------------------------------------------------------------

glm::mat4 lookFrom(const glm::vec3& eye, const glm::vec3& at) {
    return glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f) *
           glm::lookAt(eye, at, glm::vec3(0.0f, 1.0f, 0.0f));
}

// A wall across the view at z = wallZ between these x, seen through `vp`, as
// depth: the far plane everywhere else. Ray by ray, the way a depth buffer has it.
std::vector<float> wallDepth(const glm::mat4& vp, const glm::vec3& eye, float wallZ, float x0,
                             float x1, int w, int h) {
    std::vector<float> d(static_cast<std::size_t>(w) * h, 1.0f);
    const glm::mat4 inv = glm::inverse(vp);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const glm::vec4 f = inv * glm::vec4((x + 0.5f) / w * 2.0f - 1.0f,
                                                (y + 0.5f) / h * 2.0f - 1.0f, 1.0f, 1.0f);
            const glm::vec3 dir = glm::vec3(f) / f.w - eye;
            if (std::abs(dir.z) < 1e-5f) continue;
            const float t = (wallZ - eye.z) / dir.z;
            if (t <= 0.0f || t >= 1.0f) continue;
            const glm::vec3 p = eye + dir * t;
            if (p.x < x0 || p.x > x1) continue;
            const glm::vec4 c = vp * glm::vec4(p, 1.0f);
            d[static_cast<std::size_t>(y) * w + x] = c.z / c.w * 0.5f + 0.5f;
        }
    return d;
}

void checkCpu() {
    std::printf("the CPU half, on known depth:\n");
    constexpr int w = 256, h = 144;
    const glm::vec3 eye(0.0f, 0.0f, 0.0f), at(0.0f, 0.0f, -1.0f);
    const glm::mat4 vp = lookFrom(eye, at);
    // Wall 20 m out, 40 m wide, and no end to it up or down.
    const std::vector<float> depth = wallDepth(vp, eye, -20.0f, -20.0f, 20.0f, w, h);
    const auto box = [](float x, float y, float z, float r) {
        return std::pair(glm::vec3(x - r, y - r, z - r), glm::vec3(x + r, y + r, z + r));
    };
    const auto ask = [](const OcclusionCuller& c, const std::pair<glm::vec3, glm::vec3>& b) {
        return c.occluded(b.first, b.second);
    };

    OcclusionCuller c;
    c.beginFrame(vp, eye);
    c.endCarve();
    expect(!ask(c, box(0, 0, -40, 1)), "no depth yet: nothing is hidden");

    c.feedDepth(depth.data(), w, h, vp, eye);
    c.beginFrame(vp, eye);
    c.endCarve();
    expect(ask(c, box(0, 0, -40, 1)),    "behind the wall: hidden");
    expect(!ask(c, box(0, 0, -10, 1)),   "in front of the wall: not");
    expect(!ask(c, box(0, 0, -19.5f, 1)), "through the wall: not");
    expect(ask(c, box(0, 0, -21.5f, 1)),  "a hand behind the wall: hidden");
    expect(!ask(c, box(0, 0, 1, 2)),     "round the eye: not");
    expect(!ask(c, box(0, 0, 5, 1)),     "behind the eye: not");

    // Step sideways: what was behind the wall from the old spot is in the open
    // from the new one. This is what the move is for.
    {
        OcclusionCuller m;
        m.feedDepth(depth.data(), w, h, vp, eye);
        const glm::vec3 eye2(30.0f, 0.0f, 0.0f);
        m.beginFrame(lookFrom(eye2, eye2 + at), eye2);
        m.endCarve();
        expect(!ask(m, box(45, 0, -40, 1)), "past the wall's end after a step: not");
        expect(ask(m, box(5, 0, -40, 1)),   "still behind it from the new spot: hidden");
    }
    // Turn: the side nobody has seen is a hole, and a hole hides nothing.
    {
        OcclusionCuller m;
        m.feedDepth(depth.data(), w, h, vp, eye);
        m.beginFrame(lookFrom(eye, glm::vec3(-1.0f, 0.0f, -1.0f)), eye);
        m.endCarve();
        expect(!ask(m, box(-60, 0, -10, 1)), "turned to where no depth was taken: not");
    }
    // A wall that is moving hides nothing, but the wall behind it still does.
    {
        OcclusionCuller m;
        m.feedDepth(depth.data(), w, h, vp, eye);
        m.beginFrame(vp, eye);
        m.carve(glm::vec3(-20.0f, -50.0f, -20.5f), glm::vec3(20.0f, 50.0f, -19.5f));
        m.endCarve();
        expect(!ask(m, box(0, 0, -40, 1)), "behind a wall that is moving: not");

        OcclusionCuller k;
        k.feedDepth(depth.data(), w, h, vp, eye);
        k.beginFrame(vp, eye);
        k.carve(glm::vec3(-5.0f, -5.0f, -6.0f), glm::vec3(5.0f, 5.0f, -4.0f));
        k.endCarve();
        expect(ask(k, box(0, 0, -40, 1)), "a mover in front of a still wall: still hidden");

        // A cockpit round the eye opens only its own depths.
        OcclusionCuller p;
        p.feedDepth(depth.data(), w, h, vp, eye);
        p.beginFrame(vp, eye);
        p.carve(glm::vec3(-2.0f, -2.0f, -3.0f), glm::vec3(2.0f, 2.0f, 2.0f));
        p.endCarve();
        expect(ask(p, box(0, 0, -40, 1)), "a mover round the eye: still hidden");
    }
    // A copy that has stopped coming is not bet on.
    {
        OcclusionCuller m;
        m.feedDepth(depth.data(), w, h, vp, eye);
        for (int i = 0; i < 20; ++i) {
            m.beginFrame(vp, eye);
            m.endCarve();
        }
        expect(!ask(m, box(0, 0, -40, 1)), "the depth is 20 frames old: not");
    }
    // Nor one from before a cut.
    {
        OcclusionCuller m;
        m.feedDepth(depth.data(), w, h, vp, eye);
        const glm::vec3 far(500.0f, 0.0f, 0.0f);
        m.beginFrame(lookFrom(far, far + at), far);
        m.endCarve();
        expect(!ask(m, box(500, 0, -40, 1)), "the camera has cut away: not");
    }
    // A box that spills past the wall's top keeps the part that shows.
    {
        std::vector<float> low = depth;
        for (int y = h / 2 + 8; y < h; ++y)
            for (int x = 0; x < w; ++x) low[static_cast<std::size_t>(y) * w + x] = 1.0f;
        OcclusionCuller m;
        m.feedDepth(low.data(), w, h, vp, eye);
        m.beginFrame(vp, eye);
        m.endCarve();
        expect(!ask(m, std::pair(glm::vec3(-1.0f, -1.0f, -41.0f), glm::vec3(1.0f, 40.0f, -39.0f))),
               "a tower over a low wall: not");
        expect(ask(m, box(0, -4, -40, 1)), "the shed beside that tower: hidden");
    }
}

// --- The whole thing ----------------------------------------------------------

constexpr const char* kVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uViewProj;
uniform vec4 uClipPlane;
void main() {
    vec4 w = uModel * vec4(aPos, 1.0);
    gl_Position = uViewProj * w;
    gl_ClipDistance[0] = dot(w, uClipPlane);
}
)";
constexpr const char* kFrag = R"(#version 330 core
uniform vec3 uColor;
out vec4 fragColor;
void main() { fragColor = vec4(uColor, 1.0); }
)";

struct Thing {
    glm::mat4 model;
    int       material;
};

glm::mat4 boxAt(const glm::vec3& centre, const glm::vec3& size) {
    return glm::scale(glm::translate(glm::mat4(1.0f), centre), size);
}

// A ball of the unit box's size, `n` x `n` quads: what an object weighs when it is
// not a box. Only for timing -- it does not fill its box, and nothing compares it.
fitzel::Mesh detailedBall(int n) {
    std::vector<fitzel::Vertex> v;
    std::vector<std::uint32_t>  idx;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            const float a = static_cast<float>(x) / n * 6.2831853f;
            const float b = static_cast<float>(y) / n * 3.1415926f;
            const glm::vec3 p(std::sin(b) * std::cos(a), std::cos(b), std::sin(b) * std::sin(a));
            fitzel::Vertex vert{};
            vert.position = p * 0.5f;
            vert.normal   = p;
            v.push_back(vert);
        }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const std::uint32_t i = static_cast<std::uint32_t>(y * (n + 1) + x);
            const std::uint32_t r = static_cast<std::uint32_t>(n + 1);
            idx.insert(idx.end(), {i, i + 1, i + r, i + 1, i + r + 1, i + r});
        }
    return fitzel::Mesh::create(v, idx);
}

struct Run {
    long long drawn = 0, frustum = 0, hidden = 0, shownWrongly = 0;
    long long wrongFrames = 0, wrongPixels = 0, worstFrame = 0;
    long long removalFrames = 0;   // after a wall is taken away outright
    float     moveMs = 0.0f;
    double    frameMs = 0.0;       // the frames timed, see runTown
    int       maxAge = 0;
};

// Where the eye is at `frame` of the path, and how that path means to hurt.
void pose(int frame, int frames, fitzel::Camera& cam) {
    const float t = static_cast<float>(frame) / static_cast<float>(frames);
    glm::vec3 p(-8.0f, 1.7f, 170.0f);
    float yaw = -90.0f;   // down -z, along the avenue
    float pitch = 0.0f;
    if (t < 0.25f) {            // a walk up the avenue, looking about
        p.z -= t / 0.25f * 40.0f;
        yaw += 25.0f * std::sin(t * 60.0f);
    } else if (t < 0.40f) {     // whip pans on the spot
        p.z -= 40.0f;
        yaw += 720.0f * (t - 0.25f) / 0.15f;
    } else if (t < 0.60f) {     // a sprint down the avenue, and the head turning
        p.z -= 40.0f + (t - 0.40f) / 0.20f * 220.0f;
        yaw += 30.0f * std::sin(t * 90.0f);
    } else if (t < 0.65f) {     // a cut to the other end, looking back
        p = glm::vec3(100.0f, 1.7f, -170.0f);
        yaw = 110.0f;
    } else if (t < 0.85f) {     // sidestepping along a block, past gaps
        p = glm::vec3(100.0f - (t - 0.65f) / 0.20f * 180.0f, 1.7f, -8.0f);
        yaw = 90.0f;
    } else {                    // up over the roofs
        const float u = (t - 0.85f) / 0.15f;
        p = glm::vec3(-80.0f, 1.7f + u * 90.0f, -8.0f);
        yaw = 40.0f;
        pitch = -35.0f * u;
    }
    cam.setPosition(p);
    cam.setYaw(yaw);
    cam.setPitch(pitch);
}

// compareEvery 0: nothing is compared, and the frames are timed instead, each
// waited for so the GPU's half counts. `occlude` false draws everything.
Run runTown(int frames, int compareEvery, int w, int h, bool occlude = true,
            const fitzel::Mesh* shape = nullptr) {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    fitzel::Shader flat = fitzel::Shader::fromSource(kVert, kFrag);
    fitzel::Mesh   cube = fitzel::Mesh::cube();
    const fitzel::Mesh& drawn = shape ? *shape : cube;

    std::mt19937 rng(7);
    const auto u01 = [&] { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); };
    std::vector<fitzel::Material> materials;
    std::vector<Thing> things;
    const auto add = [&](const glm::mat4& m) {
        // Colours nothing else has, so a difference is always a missing object.
        const int id = static_cast<int>(materials.size()) + 1;
        fitzel::Material mat(flat);
        mat.set("uColor", glm::vec3((id & 0x3F) / 63.0f, ((id >> 6) & 0x3F) / 63.0f,
                                    (((id >> 12) & 0x0F) + 2) / 18.0f));
        materials.push_back(std::move(mat));
        things.push_back({m, static_cast<int>(materials.size()) - 1});
    };
    add(boxAt(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(1000.0f, 1.0f, 1000.0f)));   // the ground
    for (int gz = -9; gz <= 9; ++gz)
        for (int gx = -9; gx <= 9; ++gx) {
            const glm::vec3 c(static_cast<float>(gx) * 20.0f + 4.0f, 0.0f,
                              static_cast<float>(gz) * 20.0f + 4.0f);
            if (gx == 0) continue;                         // the avenue
            const float high = 6.0f + 24.0f * u01() * u01();
            add(boxAt(c + glm::vec3(0.0f, high * 0.5f, 0.0f), glm::vec3(14.0f, high, 14.0f)));
            // Things on the pavement: the small stuff a town is mostly made of.
            for (int k = 0; k < 3; ++k)
                add(boxAt(c + glm::vec3((u01() - 0.5f) * 19.0f, 0.5f, (u01() - 0.5f) * 19.0f),
                          glm::vec3(0.6f + u01(), 1.0f, 0.6f + u01())));
        }

    // Moving wall: across the avenue and back. Removed wall: stands in a side
    // street until the frame it is gone without having moved a step.
    const int movingWall = static_cast<int>(materials.size());
    add(glm::mat4(1.0f));
    const int removedWall = static_cast<int>(materials.size());
    add(boxAt(glm::vec3(-80.0f, 10.0f, -30.0f), glm::vec3(1.0f, 20.0f, 60.0f)));
    const int removedAt = frames * 3 / 4;

    fitzel::RenderTarget a(w, h, fitzel::RenderTarget::Format::RGBA8, /*depthTex=*/true);
    fitzel::RenderTarget b(w, h, fitzel::RenderTarget::Format::RGBA8, /*depthTex=*/true);
    fitzel::Renderer renderer(16, 1);
    renderer.setViewport(w, h);
    renderer.setShadowsEnabled(false);
    renderer.setOcclusionAudit(true);
    OcclusionCuller occlusion;

    fitzel::Camera cam;
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    fitzel::DirectionalLight light;
    std::vector<unsigned char> pa(static_cast<std::size_t>(w) * h * 4), pb(pa.size());

    gpu.setDepthTest(true);
    gpu.setDepthWrite(true);
    gpu.setDepthFunc(fitzel::rhi::CompareOp::Less);
    gpu.setCullEnabled(true);
    gpu.setCullFace(fitzel::rhi::CullMode::Back);

    Run run;
    for (int f = 0; f < frames; ++f) {
        const auto t0 = std::chrono::steady_clock::now();
        pose(f, frames, cam);
        const glm::mat4 view = cam.viewMatrix();
        const glm::mat4 proj = cam.projectionMatrix(aspect);
        const glm::mat4 vp   = proj * view;
        const float slide = 30.0f * std::sin(static_cast<float>(f) * 0.05f);
        things[movingWall].model =
            boxAt(glm::vec3(-8.0f + slide, 10.0f, 80.0f), glm::vec3(30.0f, 20.0f, 1.0f));

        renderer.begin(cam, aspect, light);
        for (const Thing& t : things) {
            if (&t == &things[removedWall] && f >= removedAt) continue;
            renderer.submit(drawn, materials[t.material], t.model);
        }

        a.bind();
        gpu.setClearColor(glm::vec4(0.0f));
        gpu.clear(fitzel::rhi::ClearColor | fitzel::rhi::ClearDepth);
        if (occlude) {
            renderer.prepareOcclusion(occlusion, vp, cam.position());
            renderer.setOcclusion(&occlusion);
        }
        renderer.renderScene(view, proj, cam.position(), fitzel::Renderer::kNoClip);
        renderer.setOcclusion(nullptr);
        const int hidden = renderer.lastOccluded();
        run.drawn   += renderer.lastDrawn();
        run.frustum += renderer.lastCulled();
        run.hidden  += hidden;
        run.moveMs  += occlusion.stats().updateMs;
        run.maxAge   = std::max(run.maxAge, occlusion.stats().age);
        const bool compare = compareEvery > 0 &&
                             (f % compareEvery == 0 || (f >= removedAt && f < removedAt + 16));
        // The audit waits for the GPU, which would make the depth always one
        // frame old; between comparisons the frames run as they would in a game.
        if (compare) run.shownWrongly += renderer.auditOcclusion(vp);
        if (occlude) occlusion.captureDepth(a.depthTexture(), w, h);

        if (compare) {
            b.bind();
            gpu.setClearColor(glm::vec4(0.0f));
            gpu.clear(fitzel::rhi::ClearColor | fitzel::rhi::ClearDepth);
            renderer.renderScene(view, proj, cam.position(), fitzel::Renderer::kNoClip);
            gpu.readPixels(a.framebuffer(), 0, fitzel::rhi::Rect{0, 0, w, h},
                           fitzel::rhi::Format::RGBA8, pa.data());
            gpu.readPixels(b.framebuffer(), 0, fitzel::rhi::Rect{0, 0, w, h},
                           fitzel::rhi::Format::RGBA8, pb.data());
            long long wrong = 0;
            for (std::size_t i = 0; i < pa.size(); i += 4)
                if (std::memcmp(&pa[i], &pb[i], 3) != 0) ++wrong;
            if (wrong > 0 && g_verbose) {
                int x0 = w, y0 = h, x1 = -1, y1 = -1;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
                        if (std::memcmp(&pa[i], &pb[i], 3) == 0) continue;
                        x0 = std::min(x0, x); x1 = std::max(x1, x);
                        y0 = std::min(y0, y); y1 = std::max(y1, y);
                    }
                std::printf("    frame %d: %lld pixels in x %d-%d y %d-%d, eye %.1f %.1f %.1f yaw %.0f, "
                            "depth %d frames old\n",
                            f, wrong, x0, x1, y0, y1, cam.position().x, cam.position().y,
                            cam.position().z, cam.yaw(), occlusion.stats().age);
            }
            if (wrong > 0) {
                // A wall gone without moving is the one thing allowed: the depth
                // saying so has to be drawn first. It is counted on its own.
                if (f >= removedAt && f < removedAt + 16) {
                    ++run.removalFrames;
                } else {
                    ++run.wrongFrames;
                    run.wrongPixels += wrong;
                    run.worstFrame = std::max(run.worstFrame, wrong);
                }
            }
        }
        if (compareEvery > 0) {
            gpu.flush();
        } else {
            gpu.finish();
            run.frameMs += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
        }
    }
    return run;
}

void checkGpu(int frames) {
    constexpr int w = 640, h = 360;
    for (const int every : {1, 5}) {
        std::printf("\nthe town, %d frames, compared every %d:\n", frames, every);
        const Run r = runTown(frames, every, w, h);
        const double total = static_cast<double>(r.drawn + r.hidden);
        std::printf("  drawn %lld, out of frustum %lld, hidden %lld (%.0f%% of what the frustum "
                    "let through)\n",
                    r.drawn, r.frustum, r.hidden, total > 0 ? 100.0 * r.hidden / total : 0.0);
        std::printf("  CPU %.3f ms a frame; depth at most %d frames old\n", r.moveMs / frames,
                    r.maxAge);
        std::printf("  hidden draws that would have shown: %lld\n", r.shownWrongly);
        std::printf("  frames that differ: %lld, %lld pixels, worst %lld of %d\n", r.wrongFrames,
                    r.wrongPixels, r.worstFrame, w * h);
        std::printf("  frames different after a wall was removed outright: %lld\n", r.removalFrames);
        expect(total > 0 && static_cast<double>(r.hidden) / total > 0.3,
               "a town hides at least 30% of what is in the frustum");
        // Half a percent of the picture in a single frame is a box a street away
        // for one frame, which nobody could pick out of a whip pan; more would be.
        expect(r.worstFrame * 200 <= w * h, "no frame is off by more than 0.5% of its pixels");
        expect(r.wrongFrames * 100 <= frames, "at most 1% of the frames differ at all");
        expect(r.removalFrames <= 6, "a removed wall is caught up with within 6 frames");
    }
    // What it is worth. Reported, not judged: a clock is no pass or fail, and the
    // answer is the point -- a box is about the cheapest draw there is, so there
    // it barely pays for itself, while a mesh with some triangles in it does.
    const fitzel::Mesh ball = detailedBall(100);
    for (const fitzel::Mesh* m : {static_cast<const fitzel::Mesh*>(nullptr), &ball}) {
        const Run with    = runTown(frames, 0, w, h, true, m);
        const Run without = runTown(frames, 0, w, h, false, m);
        std::printf("\ntimed, %s: %.2f ms a frame with, %.2f ms without\n",
                    m ? "20 000 triangles an object" : "boxes", with.frameMs / frames,
                    without.frameMs / frames);
    }
}

} // namespace

int main(int argc, char** argv) {
    int  frames = 900;
    bool gl     = true;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-gl")) gl = false;
        else if (!std::strcmp(argv[i], "--verbose")) g_verbose = true;
    }
    try {
        checkCpu();
        if (gl) {
            fitzel::Window window(fitzel::WindowConfig{.width = 320, .height = 200,
                                                       .title = "occlusioncheck", .vsync = false,
                                                       .maximized = false, .visible = false});
            std::printf("\nrunning on %s\n", fitzel::rhi::device().caps().renderer);
            checkGpu(frames);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "occlusioncheck: %s\n", e.what());
        return 2;
    }
    std::printf("\n%s\n", g_failed ? "FAILED" : "all good");
    // Past the window's end the statics would release into a device that is gone.
    std::fflush(stdout);
    return g_failed ? 1 : 0;
}
