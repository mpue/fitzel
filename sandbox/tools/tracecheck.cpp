// tracecheck -- does the path-traced viewport actually get going, and stop?
//
// The preview is a small state machine with a clock in it: arm on a change,
// wait for the view to settle, harvest, trace, stop. Every one of those steps
// is invisible from a screenshot -- a preview that never starts and one that is
// still converging look identical for the first second, and a preview that
// restarts forever looks like one that is simply slow. Both of those have now
// been shipped, which is why this exists.
//
// It drives viewtrace::service() with a clock it controls, against a real
// renderer with real geometry in it, and asks the four questions the eye
// cannot:
//
//   * it starts at all (the stall: a standing flag read as "the view is still
//     moving" re-armed the timer on every frame, forever)
//   * it does not start BEFORE the view settles
//   * it finishes and then stays finished (the restart loop: a scene digest
//     that the day/night cycle changed every frame)
//   * moving the camera starts it over
//
//   build/release/bin/tracecheck.exe

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/render/Renderer.hpp>
#include <fitzel/scene/Camera.hpp>

#include "../src/ViewportTrace.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_failures;
}

// A shader that never runs: the preview never draws through the raster path,
// but a Material needs one and the capture reads its parameters off it.
constexpr const char* kVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
void main() { gl_Position = uModel * vec4(aPos, 1.0); }
)";
constexpr const char* kFrag = R"(#version 330 core
out vec4 FragColor;
void main() { FragColor = vec4(1.0); }
)";

} // namespace

int main() {
    if (!glfwInit()) { std::printf("[tracecheck] glfwInit failed\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "tracecheck", nullptr, nullptr);
    if (!win) {   // a 3.3 machine still runs the preview, on the CPU
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        win = glfwCreateWindow(64, 64, "tracecheck", nullptr, nullptr);
    }
    if (!win) { std::printf("[tracecheck] no GL context\n"); glfwTerminate(); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[tracecheck] glad failed\n"); glfwTerminate(); return 2;
    }

    // A box on a ground plane, submitted the way the editor submits: this has
    // to be a real queue, because the harvest reads the meshes back off the GPU.
    fitzel::Shader   shader = fitzel::Shader::fromSource(kVert, kFrag);
    fitzel::Mesh     box    = fitzel::Mesh::cube();
    fitzel::Material mat(shader);
    mat.set("uAlbedo", glm::vec3(0.7f)).set("uRoughness", 0.5f);

    fitzel::Camera camera(glm::vec3(0.0f, 3.0f, 8.0f), -90.0f, -15.0f);
    fitzel::DirectionalLight light;
    fitzel::Renderer renderer;
    renderer.setViewport(320, 240);

    viewtrace::State st;
    st.samples     = 8;      // a target it can actually reach in a test
    st.scale       = 0.25f;  // 80x60: this is a state machine test, not a render
    st.settleTime  = 0.25f;
    st.gpuPerFrame = 4;
    // Run from the repo root, where the kernel is a source file rather than a
    // copy beside an executable. Both spellings, so it works either way.
    st.kernelPath  = "sandbox/assets/shaders/gputrace.comp";

    pathpanel::SceneLook look;

    // One frame of the editor's loop, with a clock this test owns. Real time
    // would make the test a race against the machine it runs on.
    double now = 0.0;
    auto frame = [&](double dt) {
        now += dt;
        // The GPU traces inside service(), so a frame of fake time is a frame of
        // real work. The CPU tracer is threads, and they need real milliseconds
        // -- without this the test spins through its whole clock in microseconds
        // and concludes that a renderer which had not been given any time had
        // failed to produce anything.
        if (!st.gpuReady) std::this_thread::sleep_for(std::chrono::milliseconds(8));
        renderer.begin(camera, 4.0f / 3.0f, light);
        renderer.submit(box, mat, glm::scale(glm::mat4(1.0f), glm::vec3(1.0f)));
        renderer.submit(box, mat,
                        glm::scale(glm::translate(glm::mat4(1.0f),
                                                  glm::vec3(0.0f, -1.5f, 0.0f)),
                                   glm::vec3(12.0f, 0.2f, 12.0f)));
        viewtrace::service(st, /*enabled=*/true, renderer, camera, look,
                           320, 240, now);
    };

    std::printf("tracecheck -- %s\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // --- 1. It waits, and then it goes ---------------------------------------
    std::printf("\nstarting up\n");
    frame(0.016);
    check(st.status.find("settle") != std::string::npos,
          "it arms and says it is waiting", st.status);

    // Two frames inside the settle window: nothing may have started yet.
    frame(0.05);
    frame(0.05);
    check(viewtrace::samplesDone(st) == 0, "nothing starts before the view settles",
          std::to_string(viewtrace::samplesDone(st)) + " samples");

    // Past it: the harvest happens and samples start landing. Twenty frames is
    // a ceiling, not an expectation -- the point is that it is FINITE, which is
    // exactly what the stall was not.
    int frames = 0;
    while (viewtrace::samplesDone(st) == 0 && frames < 20) { frame(0.05); ++frames; }
    check(viewtrace::samplesDone(st) > 0, "and then it traces",
          "after " + std::to_string(frames) + " frames, status: " + st.status);

    // --- 2. It finishes, and stays finished ----------------------------------
    std::printf("\nreaching the end\n");
    frames = 0;
    while (viewtrace::samplesDone(st) < st.samples && frames < 200) {
        frame(0.05);
        ++frames;
    }
    check(viewtrace::samplesDone(st) >= st.samples, "it reaches its sample target",
          std::to_string(viewtrace::samplesDone(st)) + " / " +
              std::to_string(st.samples));

    // Sixty more frames -- three seconds of scene time, well past every timer in
    // there. A preview that restarts on its own drops back to a low count here,
    // and that is precisely the bug this row exists for.
    const int settled = viewtrace::samplesDone(st);
    for (int i = 0; i < 60; ++i) frame(0.05);
    check(viewtrace::samplesDone(st) >= settled,
          "and does not start over on its own",
          "was " + std::to_string(settled) + ", now " +
              std::to_string(viewtrace::samplesDone(st)));

    // --- 3. Moving the camera starts it over ---------------------------------
    std::printf("\nmoving the camera\n");
    camera.setPosition(glm::vec3(2.0f, 3.5f, 7.0f));
    frame(0.016);
    check(viewtrace::samplesDone(st) == 0 || st.status.find("settle") != std::string::npos,
          "a moved camera arms a restart", st.status);
    // The restart is what CLEARS the accumulator, so the evidence that one
    // happened is the count DROPPING -- not the count being above zero, which
    // it still is from the view before. Asking the weak question here passed
    // while doing nothing, which is its own small lesson.
    frames = 0;
    while (viewtrace::samplesDone(st) >= settled && frames < 20) {
        frame(0.05);
        ++frames;
    }
    check(viewtrace::samplesDone(st) < settled, "and starts the new view over",
          "count fell to " + std::to_string(viewtrace::samplesDone(st)) + " after " +
              std::to_string(frames) + " frames");

    frames = 0;
    while (viewtrace::samplesDone(st) < st.samples && frames < 200) {
        frame(0.05);
        ++frames;
    }
    check(viewtrace::samplesDone(st) >= st.samples, "and traces it to the end",
          std::to_string(viewtrace::samplesDone(st)) + " / " +
              std::to_string(st.samples));

    // --- 4. Switching off lets go of it --------------------------------------
    std::printf("\nswitching off\n");
    renderer.begin(camera, 4.0f / 3.0f, light);
    viewtrace::service(st, /*enabled=*/false, renderer, camera, look, 320, 240, now);
    check(!st.scene, "the harvest is dropped when the mode is left");

    glfwTerminate();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "passed" : "FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
