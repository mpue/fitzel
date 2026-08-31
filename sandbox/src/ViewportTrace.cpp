#include "ViewportTrace.hpp"

#include <algorithm>
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

void refreshImage(State& st, double now) {
    if (!st.job.hasImage()) return;
    const bool running = st.job.running();
    const int  done    = st.job.samplesDone();
    if (done == st.shownSamples && !running) return;
    // A quarter of a second between uploads while it converges. The picture
    // gains a little less noise in that time than the upload costs to make.
    if (running && now - st.shownStamp < 0.25) return;

    std::vector<unsigned char> px;
    if (!st.job.snapshotLdr(px)) return;
    const int w = st.job.settings().width, h = st.job.settings().height;
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

        if (st.needCapture || !st.scene) {
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

        pathtrace::Settings s;
        s.width      = w;
        s.height     = h;
        s.samples    = st.samples;
        s.maxBounces = st.maxBounces;
        s.batch      = 1;      // one sample a pass: the first picture arrives soonest
        s.tonemap    = true;   // straight to the screen, so it tonemaps itself
        st.shownSamples = -1;
        st.job.start(st.scene, s);
        st.status = "tracing";
    }

    refreshImage(st, now);
    if (!st.restartDue && st.job.hasImage()) {
        const int done = st.job.samplesDone();
        st.status = st.job.running()
                      ? std::to_string(done) + " / " + std::to_string(st.samples) +
                            " samples"
                      : std::to_string(done) + " samples, done";
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
