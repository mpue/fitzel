// camcheck -- does a camera bolted into a craft actually SIT there?
//
// The fault this exists to catch is invisible in the editor and unreadable in
// the place people look for it. A camera parented to a craft used to be placed
// from the craft's CACHED world transform, which the scene-graph resolve writes
// late in the frame -- so the eye rode where the craft was LAST frame while the
// craft was drawn where it is now. The gap is speed x the previous frame's
// length, so it breathes with frame-time jitter: the world is steady, the eye
// is steady, and the CRAFT appears to judder inside its own cockpit. Every
// instinct then sends you into the flight model, where there is nothing wrong.
//
// So it is measured here, on the one number that says it: the distance between
// where the camera was placed and where the seat actually ends up this frame.
// Zero is the only passing answer -- the seat is bolted on.
//
//   build/release/bin/camcheck.exe

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "../src/CameraSystem.hpp"
#include "../src/Component.hpp"
#include "../src/SandboxMath.hpp"
#include "../src/SceneGraph.hpp"
#include "../src/SceneTypes.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++failures;
}

Entity& add(std::vector<Entity>& es, int id, int parent, const glm::vec3& lpos,
            const glm::vec3& lrot) {
    Entity e;
    e.id = id; e.parent = parent;
    e.localCenter = lpos; e.localRotation = lrot;
    e.center = lpos; e.rotation = lrot;
    es.push_back(std::move(e));
    return es.back();
}

void addCam(Entity& e, int mode) {
    auto cc = std::make_unique<CameraComponent>();
    cc->mode = mode;
    e.components.items.push_back(std::move(cc));
}

// One frame of the real thing, in the real ORDER, which is the whole point: the
// flight model writes the craft's transform, the cameras are placed, and only
// THEN does the scene-graph resolve run (main.cpp does it just before the scene
// is drawn). A harness that resolved first would place the camera from a
// freshly-derived world transform and pass while the game juddered.
void frame(std::vector<Entity>& es, camerasys::CameraSystem& cams, float dt) {
    cams.update(es, dt);
    scenegraph::resolve(es);
}

float dist(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b); }

} // namespace

int main() {
    // --- 1. The cockpit stays put through jittering frames --------------------
    // A craft doing 80 m/s down +Z with a seat 1.2 m up and 0.8 m forward, run
    // over frame times that wander the way a vsync'd frame really does.
    {
        std::vector<Entity> es;
        add(es, 1, -1, glm::vec3(0.0f), glm::vec3(0.0f));           // craft
        addCam(add(es, 2, 1, glm::vec3(0.0f, 1.2f, 0.8f), glm::vec3(0.0f)),
               CameraComponent::Cockpit);

        camerasys::CameraSystem cams;
        const float frames[] = {0.0083f, 0.0331f, 0.0092f, 0.0167f, 0.0350f,
                                0.0081f, 0.0210f, 0.0166f, 0.0089f, 0.0300f};
        float t = 0.0f, worst = 0.0f, worstStale = 0.0f;
        for (float dt : frames) {
            t += dt;
            // What the sim does: write the craft's transform for this frame.
            es[0].localCenter = glm::vec3(0.0f, 0.0f, 80.0f * t);
            es[0].center      = es[0].localCenter;
            // What the eye WOULD have been handed by the cached world transform
            // (still last frame's, because the resolve has not run yet). Kept
            // only to report the size of the error live composition removes.
            const glm::vec3 stale = es[1].center;

            frame(es, cams, dt);

            camerasys::Pose p;
            if (!cams.pose(2, p)) { check(false, "cockpit camera has a pose"); break; }
            worst      = glm::max(worst,      dist(p.position, es[1].center));
            worstStale = glm::max(worstStale, dist(stale,      es[1].center));
        }
        char d[160];
        std::snprintf(d, sizeof d,
                      "worst %.4f m; the cached-transform answer was off by %.2f m",
                      worst, worstStale);
        check(worst < 1.0e-3f, "cockpit eye sits exactly on its seat", d);
        // The comparison is only worth anything if the stale answer really was
        // wrong here -- otherwise the case is too gentle to catch a regression.
        check(worstStale > 0.5f, "the frame-time jitter is severe enough to matter");
    }

    // --- 2. A static camera hung on a craft rides it live too -----------------
    // Same fault, and the mode people reach for first (see CameraComponent).
    {
        std::vector<Entity> es;
        add(es, 1, -1, glm::vec3(0.0f), glm::vec3(0.0f));
        addCam(add(es, 2, 1, glm::vec3(0.0f, 2.0f, -6.0f), glm::vec3(0.0f)),
               CameraComponent::Static);

        camerasys::CameraSystem cams;
        const float frames[] = {0.0083f, 0.0331f, 0.0092f, 0.0350f, 0.0081f};
        float t = 0.0f, worst = 0.0f;
        for (float dt : frames) {
            t += dt;
            es[0].localCenter = glm::vec3(0.0f, 0.0f, 80.0f * t);
            es[0].center      = es[0].localCenter;
            frame(es, cams, dt);
            camerasys::Pose p;
            if (cams.pose(2, p)) worst = glm::max(worst, dist(p.position, es[1].center));
        }
        char d[80];
        std::snprintf(d, sizeof d, "worst %.4f m", worst);
        check(worst < 1.0e-3f, "parented static camera rides its parent live", d);
    }

    // --- 3. A camera standing on its own is untouched -------------------------
    {
        std::vector<Entity> es;
        addCam(add(es, 1, -1, glm::vec3(3.0f, 5.0f, -2.0f),
                   glm::vec3(-10.0f, 45.0f, 0.0f)),
               CameraComponent::Static);
        camerasys::CameraSystem cams;
        frame(es, cams, 0.016f);
        camerasys::Pose p;
        const bool got = cams.pose(1, p);
        check(got && dist(p.position, glm::vec3(3.0f, 5.0f, -2.0f)) < 1.0e-4f,
              "a root static camera stands where it is put");
        // -Z forward through yaw 45 / pitch -10.
        const float cp = std::cos(glm::radians(10.0f));
        const glm::vec3 want = glm::normalize(
            glm::vec3(-std::sin(glm::radians(45.0f)) * cp,
                      -std::sin(glm::radians(10.0f)),
                      -std::cos(glm::radians(45.0f)) * cp));
        check(got && glm::dot(glm::normalize(p.front), want) > 0.9995f,
              "a root static camera aims where its frustum is drawn");
    }

    // --- 4. The cockpit rolls with the craft ----------------------------------
    // And rolls about the CRAFT'S NOSE, not the world Z axis, which is the trap
    // a scene Euler triple sets for anything that banks (see attitudeEuler).
    // Craft heading due east, banked 30 degrees: the eye's up must lean 30
    // degrees out of vertical, and lean about the nose rather than about world Z.
    {
        std::vector<Entity> es;
        add(es, 1, -1, glm::vec3(0.0f), attitudeEuler(90.0f, 0.0f, 30.0f));
        addCam(add(es, 2, 1, glm::vec3(0.0f, 1.2f, 0.8f), glm::vec3(0.0f)),
               CameraComponent::Cockpit);
        camerasys::CameraSystem cams;
        frame(es, cams, 0.016f);

        camerasys::Pose p;
        const bool got = cams.pose(2, p);
        const float lean = glm::degrees(std::acos(glm::clamp(
            glm::dot(glm::normalize(p.up), glm::vec3(0.0f, 1.0f, 0.0f)), -1.0f, 1.0f)));
        char d[80];
        std::snprintf(d, sizeof d, "%.1f deg out of vertical", lean);
        check(got && std::abs(lean - 30.0f) < 0.5f, "cockpit banks with the craft", d);
        // Which WAY it looks is the camera's own convention, and it is worth
        // pinning down because it surprises people: a camera looks along its own
        // -Z, a craft's nose is its +Z, so a cockpit camera left at zero rotation
        // looks out of the BACK of the craft. Nose east means the shot points
        // west -- dead level, and exactly opposite the nose. A bank misread as a
        // heading (the rotation.y trap) would swing it off that axis instead.
        check(got && glm::dot(glm::normalize(p.front), glm::vec3(-1.0f, 0.0f, 0.0f)) > 0.999f,
              "an unturned cockpit camera looks straight back down the craft");
    }

    // --- 4b. Turned to face the nose ------------------------------------------
    // Which is what a cockpit view is, and it is the camera's own local rotation
    // that says so -- 180 degrees of yaw in the craft's frame. Deliberately NOT
    // done for the author behind the scenes: the frustum the gizmo draws is then
    // the shot you get, and a hidden half turn would make the editor lie. It also
    // could not be right in general -- a glider model whose nose is the other way
    // carries `forward` for exactly that reason.
    {
        std::vector<Entity> es;
        add(es, 1, -1, glm::vec3(0.0f), attitudeEuler(90.0f, 0.0f, 30.0f));
        addCam(add(es, 2, 1, glm::vec3(0.0f, 1.2f, 0.8f), glm::vec3(0.0f, 180.0f, 0.0f)),
               CameraComponent::Cockpit);
        camerasys::CameraSystem cams;
        frame(es, cams, 0.016f);
        camerasys::Pose p;
        const bool got = cams.pose(2, p);
        check(got && glm::dot(glm::normalize(p.front), glm::vec3(1.0f, 0.0f, 0.0f)) > 0.999f,
              "turned about, it looks where the banked craft's nose points");
        // ...and the bank comes with it, still about the nose rather than about
        // world Z: the eye's up leans the same 30 degrees after the half turn.
        const float lean = glm::degrees(std::acos(glm::clamp(
            glm::dot(glm::normalize(p.up), glm::vec3(0.0f, 1.0f, 0.0f)), -1.0f, 1.0f)));
        char d[80];
        std::snprintf(d, sizeof d, "%.1f deg out of vertical", lean);
        check(got && std::abs(lean - 30.0f) < 0.5f,
              "and keeps the craft's bank once turned", d);
    }

    // --- 5. A follow camera is NOT dragged into the craft's frame -------------
    // The same craft, the same bank, a follow camera at rollWith 0: its horizon
    // stays level. Proof that the cockpit mode is a mode, and not a change to
    // every camera in the scene.
    {
        std::vector<Entity> es;
        add(es, 1, -1, glm::vec3(0.0f), attitudeEuler(90.0f, 0.0f, 30.0f));
        addCam(add(es, 2, 1, glm::vec3(0.0f, 2.0f, -6.0f), glm::vec3(0.0f)),
               CameraComponent::Follow);
        camerasys::CameraSystem cams;
        frame(es, cams, 0.016f);
        camerasys::Pose p;
        const bool got = cams.pose(2, p);
        check(got && glm::dot(glm::normalize(p.up), glm::vec3(0.0f, 1.0f, 0.0f)) > 0.999f,
              "a follow camera keeps its horizon level through the same bank");
    }

    // --- 6. No parent, no cockpit ---------------------------------------------
    // Same rule as a parentless follow camera: stop being a view rather than
    // stare from the world origin, because the second is a mistake nobody sees.
    {
        std::vector<Entity> es;
        addCam(add(es, 1, -1, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f)),
               CameraComponent::Cockpit);
        camerasys::CameraSystem cams;
        frame(es, cams, 0.016f);
        camerasys::Pose p;
        check(!cams.pose(1, p), "a cockpit camera with no parent is not a view");
    }

    std::printf("\n%s\n", failures == 0 ? "camcheck: all good"
                                        : "camcheck: FAILURES above");
    return failures == 0 ? 0 : 1;
}
