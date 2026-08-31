#include "ViewportTrace.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <fitzel/render/Renderer.hpp>
#include <fitzel/scene/Camera.hpp>

namespace viewtrace {
namespace {

// Point an already-harvested scene at where the camera is now. The same basis
// pathcapture::capture() takes, and the reason a camera move does not cost a
// harvest: nothing else about the scene has changed, and the triangles are the
// expensive part.
void aim(pathtrace::Scene& scene, const fitzel::Camera& camera) {
    scene.camera.position   = camera.position();
    scene.camera.forward    = glm::normalize(camera.front());
    scene.camera.up         = glm::normalize(camera.up());
    scene.camera.right      = glm::normalize(camera.right());
    scene.camera.fovDegrees = camera.fov();
}

// The GPU keeps SUMS in a float image; the screen wants eight bits through the
// same curve the Render panel's still goes through. Tonemapped on the CPU with
// pathtrace::tonemap rather than in a resolve shader, so the preview and the
// still cannot drift apart -- it is the same function, not a second one that
// looks like it.
bool gpuLdr(const State& st, std::vector<unsigned char>& out, int& w, int& h) {
    std::vector<float> hdr;
    if (!st.gpu.snapshotHdr(hdr)) return false;
    w = st.gpu.width();
    h = st.gpu.height();
    const float exposure = st.scene ? st.scene->exposure : 1.0f;
    const pathtrace::Grade grade = st.scene ? st.scene->grade : pathtrace::Grade{};
    out.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
        const glm::vec3 c = pathtrace::tonemap(
            glm::vec3(hdr[i * 3], hdr[i * 3 + 1], hdr[i * 3 + 2]), exposure, grade);
        for (int k = 0; k < 3; ++k)
            out[i * 4 + k] = static_cast<unsigned char>(
                std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return true;
}

void refreshImage(State& st, double now) {
    const bool onGpu   = st.gpuReady;
    const bool hasImg  = onGpu ? st.gpu.samplesDone() > 0 : st.job.hasImage();
    if (!hasImg) return;
    const int  done    = onGpu ? st.gpu.samplesDone() : st.job.samplesDone();
    const bool running = onGpu ? done < st.samples : st.job.running();
    if (done == st.shownSamples && !running) return;
    // A quarter of a second between uploads while it converges. The picture
    // gains a little less noise in that time than the upload costs to make.
    if (running && now - st.shownStamp < 0.25) return;

    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (onGpu) {
        if (!gpuLdr(st, px, w, h)) return;
    } else {
        if (!st.job.snapshotLdr(px)) return;
        w = st.job.settings().width;
        h = st.job.settings().height;
    }
    if (!st.image.isValid() || st.texW != w || st.texH != h) {
        st.image = fitzel::Texture::blank(w, h);
        st.texW  = w;
        st.texH  = h;
    }
    st.image.update(px.data(), w, h, 4);
    st.shownSamples = done;
    st.shownStamp   = now;
}

} // namespace

void service(State& st, bool enabled, fitzel::Renderer& renderer,
             const fitzel::Camera& camera, const pathpanel::SceneLook& look,
             int viewW, int viewH, double now) {
    if (!enabled) {
        if (st.wasEnabled) {
            st.job.cancel();
            st.scene.reset();       // the harvest is the memory; do not sit on it
            st.status.clear();
            st.shownSamples = -1;
            st.needCapture  = true;
            st.restartDue   = false;
            st.wasEnabled   = false;
        }
        return;
    }
    st.wasEnabled = true;

    // Once, on the first frame in this mode: is there a GPU tracer to be had?
    // Trying per frame would mean re-reading and re-compiling a kernel every
    // frame on a machine that cannot run it, which is the machine least able to
    // afford it.
    if (!st.gpuTried) {
        st.gpuTried = true;
        if (gputrace::available()) {
            st.gpuReady = st.gpu.init("assets/shaders/gputrace.comp");
            if (!st.gpuReady)
                std::printf("[Fitzel] the GPU tracer stayed off: %s\n",
                            st.gpu.error().c_str());
        }
    }

    const int w = std::max(64, static_cast<int>(viewW * st.scale));
    const int h = std::max(64, static_cast<int>(viewH * st.scale));
    const glm::mat4 view = camera.viewMatrix();

    // WHAT RESTARTS IT: the eye moved, the picture changed size, or somebody
    // asked (refresh()). Deliberately nothing else -- and in particular not
    // "the scene looks different from the one I traced", which was the first
    // version and was wrong. The day/night cycle walks the sun a fraction of a
    // degree every frame, so "different" was true on every frame of a scene
    // nobody was touching, and the preview threw away a finished picture every
    // couple of seconds to trace the same view again. A preview that never
    // finishes is worse than one that is a minute out of date.
    if (view != st.lastView || w != st.lastW || h != st.lastH || st.needCapture) {
        st.lastView   = view;
        st.lastW      = w;
        st.lastH      = h;
        // Pushed out again on every change, so a camera being dragged schedules
        // ONE restart when it comes to rest rather than one per frame.
        st.restartDue = true;
        st.restartAt  = now + st.settleTime;
        st.status     = "waiting for the view to settle";
    }

    if (st.restartDue && now >= st.restartAt) {
        st.restartDue = false;
        st.job.cancel();

        const bool uploaded = st.needCapture || !st.scene;
        if (uploaded) {
            pathcapture::Options opt = pathcapture::Options{};
            opt.hdriPath      = look.hdriPath;
            opt.hdriIntensity = look.hdriIntensity;
            opt.grade         = look.grade;
            st.scene       = pathcapture::capture(renderer, camera, opt, &st.report);
            st.needCapture = false;
        }
        // Cancel() has joined the workers, so nothing else is holding the scene
        // and re-aiming it here is safe.
        aim(*st.scene, camera);

        if (st.scene->triangles.empty()) {
            st.status = "nothing to trace: no geometry the tracer can read";
            return;
        }

        st.shownSamples = -1;
        if (st.gpuReady) {
            // The upload is the expensive half and only a real edit needs it;
            // a camera move re-aims what is already there. resize() throws the
            // accumulator away either way, because what is in it was seen from
            // somewhere else.
            if (uploaded && !st.gpu.upload(*st.scene)) {
                std::printf("[Fitzel] the GPU tracer stayed off: %s\n",
                            st.gpu.error().c_str());
                st.gpuReady = false;
            }
            if (st.gpuReady) {
                st.gpu.setCamera(st.scene->camera);
                st.gpu.setPath(st.maxBounces, pathtrace::Settings{}.clampIndirect);
                st.gpu.resize(w, h);
            }
        }
        if (!st.gpuReady) {
            pathtrace::Settings s;
            s.width      = w;
            s.height     = h;
            s.samples    = st.samples;
            s.maxBounces = st.maxBounces;
            s.batch      = 1;    // one sample a pass: the first picture soonest
            s.tonemap    = true; // straight to the screen, so it tonemaps itself
            st.job.start(st.scene, s);
        }
        st.status = "tracing";
    }

    // The GPU traces HERE, on the render thread and a couple of samples at a
    // time. Not on a thread of its own, because a GL context belongs to one
    // thread; and not the whole render in one dispatch, because Windows kills a
    // driver whose single command runs long. A few samples a frame is what
    // makes this a preview rather than a freeze.
    if (st.gpuReady && !st.restartDue && st.gpu.samplesDone() < st.samples) {
        const int left = st.samples - st.gpu.samplesDone();
        if (!st.gpu.accumulate(std::min(st.gpuPerFrame, left))) {
            std::printf("[Fitzel] the GPU tracer stopped: %s\n",
                        st.gpu.error().c_str());
            st.gpuReady = false;
        }
    }

    refreshImage(st, now);

    const bool onGpu = st.gpuReady;
    const int  done  = onGpu ? st.gpu.samplesDone() : st.job.samplesDone();
    const bool busy  = onGpu ? done < st.samples : st.job.running();
    if (!st.restartDue && (onGpu ? done > 0 : st.job.hasImage())) {
        st.status = std::to_string(done) + " / " + std::to_string(st.samples) +
                    (busy ? " samples" : " samples, done") +
                    (onGpu ? " (GPU)" : " (CPU)");
    }
}

void refresh(State& st) {
    // A fresh harvest, not just a re-aim: this is the way an EDIT gets into the
    // preview, and an edit is exactly what the kept scene no longer matches.
    st.needCapture = true;
}

unsigned int texture(const State& st) {
    return st.image.isValid() ? st.image.id() : 0u;
}

} // namespace viewtrace
