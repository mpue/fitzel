// The terrain-streaming check: after the ground has been regenerated many times
// in a row, how long until it is back?
//
// That is what dragging a terrain slider in the Inspector does -- every frame
// the component's settings differ, the editor adopts them and calls rebuild().
// Each rebuild queues the whole ring again (121 chunks at view distance 5), and
// the workers used to build every one of those queued jobs, stale or not, in
// the order they were queued: a three-second drag left ~20 000 chunks nobody
// would ever draw standing in front of the ground that was wanted. The terrain
// then took minutes to come back, or -- with a longer drag or finer chunks --
// effectively never did.
//
// This drives the real TerrainStreamer the way the editor's frame loop does
// (a rebuild and an update per frame while "dragging", then updates only) and
// times two things: the chunk under the camera, and the whole ring. It needs a
// GL context because finished chunks are uploaded; the window is never shown.
//   build/release/bin/streamcheck.exe
// Exits non-zero if any check fails.

#include <chrono>
#include <cstdio>
#include <thread>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <fitzel/world/Terrain.hpp>

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

using Clock = std::chrono::steady_clock;

bool hasChunk(const fitzel::TerrainStreamer& s, glm::ivec2 c) {
    for (const fitzel::TerrainChunk* ch : s.visibleChunks())
        if (ch->coord() == c) return true;
    return false;
}

struct Timing {
    double centre = -1.0;  // seconds until the chunk under the camera is drawn
    double ring   = -1.0;  // ...until every chunk of the ring is
};

// Frames at ~60 Hz until the ring is complete or `limit` seconds pass.
Timing waitForGround(fitzel::TerrainStreamer& s, const glm::vec3& cam, int radius,
                     double limit) {
    Timing t;
    const int want = (2 * radius + 1) * (2 * radius + 1);
    const auto t0  = Clock::now();
    for (;;) {
        s.update(cam);
        const double el = std::chrono::duration<double>(Clock::now() - t0).count();
        if (t.centre < 0.0 && hasChunk(s, {0, 0})) t.centre = el;
        if (s.loadedChunkCount() >= want) { t.ring = el; break; }
        if (el > limit) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return t;
}

} // namespace

int main() {
    if (!glfwInit()) { std::printf("[streamcheck] glfwInit failed\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "streamcheck", nullptr, nullptr);
    if (!win) { std::printf("[streamcheck] no GL 3.3 core context\n"); glfwTerminate(); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[streamcheck] glad failed\n"); glfwTerminate(); return 2;
    }

    const int       radius = 5;              // the editor's default view distance
    const glm::vec3 cam(32.0f, 20.0f, 32.0f); // the middle of chunk (0, 0)
    {
        fitzel::TerrainSettings ts;
        fitzel::TerrainStreamer s(ts, radius);

        std::printf("first load\n");
        const Timing first = waitForGround(s, cam, radius, 60.0);
        std::printf("       centre after %.2f s, whole ring after %.2f s\n", first.centre, first.ring);
        check(first.ring >= 0.0, "the ring loads");
        check(first.centre >= 0.0 && first.centre < first.ring * 0.5,
              "the chunk underfoot comes early, not half way through the ring");

        std::printf("after a 2 s slider drag (a rebuild every frame)\n");
        for (int f = 0; f < 120; ++f) {
            s.settings().heightScale += 0.05f;
            s.rebuild();
            s.update(cam);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        const Timing drag = waitForGround(s, cam, radius, 60.0);
        std::printf("       centre after %.2f s, whole ring after %.2f s\n", drag.centre, drag.ring);
        check(drag.ring >= 0.0, "the ground comes back at all (within a minute)");
        check(drag.ring >= 0.0 && drag.ring < first.ring * 2.0 + 0.5,
              "and about as fast as a first load -- no backlog of stale chunks");
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
