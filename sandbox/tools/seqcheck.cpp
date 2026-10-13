// seqcheck -- does a sequence cut where its shots say, pose the scene from the
// strips under the playhead, and come back from a save the same?
//
// A sequencer fails quietly in all the ways that matter: a cut a frame late is
// a flash of the wrong camera nobody can point at, a held pose that snaps back
// at the end of a strip reads as a glitch in the animation rather than in the
// edit, and a strip whose offset is lost on reload plays the right clip from the
// wrong place. So the arithmetic is measured, not looked at:
//
//   - two shots side by side cut at the boundary, a gap holds the last shot,
//     and a shot on a higher camera track overrides the one below it;
//   - an animation strip plays its clip from its own start, at its own speed,
//     from its own offset, holds its last pose after it ends (and does not
//     when the track says not to), and loops when asked;
//   - splitting a strip leaves the material under the playhead where it was;
//   - fades shape the gain the way the strip draws them;
//   - a sequence saved to JSON and loaded back is the same sequence;
//   - a recorded fly-camera path becomes a camera clip that looks the same way
//     the fly camera did.
//
// No GL, no window, no audio device.
//
//   build/release/bin/seqcheck
// Exits non-zero if any check fails.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "../src/AnimSystem.hpp"
#include "../src/CameraPath.hpp"
#include "../src/Component.hpp"
#include "../src/SceneTypes.hpp"
#include "../src/Sequencer.hpp"

namespace {

int g_fails = 0;

void check(bool ok, const char* what, const std::string& detail) {
    if (ok) std::printf("  ok   %s -- %s\n", what, detail.c_str());
    else  { std::printf("[FAIL] %s: %s\n", what, detail.c_str()); ++g_fails; }
}
void near(float got, float want, const char* what, float eps = 1e-3f) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "got %.4f, want %.4f", got, want);
    check(std::fabs(got - want) <= eps, what, buf);
}
void same(int got, int want, const char* what) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "got %d, want %d", got, want);
    check(got == want, what, buf);
}

// Two cameras and a box.
std::vector<Entity> makeScene() {
    std::vector<Entity> es;
    for (int i = 0; i < 2; ++i) {
        Entity cam;
        cam.type = EntityType::Empty;
        cam.id   = 10 + i;
        cam.name = i == 0 ? "Cam A" : "Cam B";
        cam.components.items.push_back(std::make_unique<CameraComponent>());
        es.push_back(std::move(cam));
    }
    Entity box;
    box.type = EntityType::Box;
    box.id   = 20;
    box.name = "Box";
    es.push_back(std::move(box));
    return es;
}

// A clip moving one entity's x from 0 to 10 over two seconds.
anim::Clip slide(const char* name, int entity) {
    anim::Clip c;
    c.name = name;
    anim::Track t;
    t.entityId = entity;
    t.key      = "center";
    t.index    = 0;
    t.keys     = {{0.0f, 0.0f}, {2.0f, 10.0f}};
    c.tracks.push_back(t);
    return c;
}

const Entity& byId(const std::vector<Entity>& es, int id) {
    for (const Entity& e : es) if (e.id == id) return e;
    return es.front();
}

} // namespace

int main() {
    std::vector<Entity> scene = makeScene();
    std::vector<anim::Clip> clips{slide("Fly A", 10), slide("Fly B", 11), slide("Push", 20)};

    // --- Cuts -----------------------------------------------------------------
    std::printf("cuts\n");
    seq::Sequence s;
    seq::Track& cams = s.addTrack(seq::Kind::Camera);
    {
        seq::Strip a; a.uid = s.newUid(); a.start = 0.0f; a.length = 2.0f; a.camera = 10; a.clip = "Fly A";
        seq::Strip b; b.uid = s.newUid(); b.start = 2.0f; b.length = 2.0f; b.camera = 11; b.clip = "Fly B";
        cams.strips = {a, b};
    }
    same(seq::cameraOfClip(clips[0], scene), 10, "a camera clip names the camera it moves");
    same(seq::cameraOfClip(clips[2], scene), -1, "a clip moving no camera names none");
    same(seq::cutAt(s, 0.5f).camera, 10, "first shot is on Cam A");
    same(seq::cutAt(s, 1.999f).camera, 10, "still Cam A a moment before the cut");
    same(seq::cutAt(s, 2.0f).camera, 11, "cut to Cam B exactly at the boundary");
    same(seq::cutAt(s, 9.0f).camera, 11, "after the last shot it holds");
    {
        seq::Track& over = s.addTrack(seq::Kind::Camera, "Insert");
        seq::Strip ins; ins.uid = s.newUid(); ins.start = 0.5f; ins.length = 0.5f; ins.camera = 11;
        over.strips.push_back(ins);
        // The track added second sits BELOW the first (same kind, appended),
        // so the master still wins there; move it on top to make it the insert.
        std::swap(s.tracks[0], s.tracks[1]);
        same(seq::cutAt(s, 0.75f).camera, 11, "a shot on the upper camera track overrides");
        same(seq::cutAt(s, 1.25f).camera, 10, "...and gives the cut back when it ends");
        s.tracks.erase(s.tracks.begin());
    }
    {
        seq::Sequence empty;
        same(seq::cutAt(empty, 1.0f).camera, -1, "no shot, no cut");
    }

    // --- Animation strips ------------------------------------------------------
    std::printf("animation strips\n");
    seq::Track& an = s.addTrack(seq::Kind::Anim);
    {
        seq::Strip p; p.uid = s.newUid(); p.start = 4.0f; p.length = 2.0f; p.clip = "Push";
        an.strips.push_back(p);
    }
    same(static_cast<int>(s.tracks.back().kind), static_cast<int>(seq::Kind::Anim),
         "an animation track goes below the camera track");
    seq::applyAnim(s, clips, scene, 5.0f);
    near(byId(scene, 20).localCenter.x, 5.0f, "strip plays its clip from its own start");
    near(byId(scene, 11).localCenter.x, 10.0f, "the last shot's camera holds its last pose");
    seq::applyAnim(s, clips, scene, 7.0f);
    near(byId(scene, 20).localCenter.x, 10.0f, "past its end the strip holds its last pose");
    seq::Track* anp = nullptr;
    for (seq::Track& t : s.tracks) if (t.kind == seq::Kind::Anim) anp = &t;
    anp->strips[0].speed  = 2.0f;
    anp->strips[0].offset = 0.5f;
    seq::applyAnim(s, clips, scene, 4.25f);
    near(byId(scene, 20).localCenter.x, 5.0f, "offset and speed: 0.5 + 0.25*2 = 1 s into the clip");
    anp->strips[0].speed  = 1.0f;
    anp->strips[0].offset = 0.0f;
    anp->strips[0].length = 5.0f;
    anp->strips[0].loop   = true;
    seq::applyAnim(s, clips, scene, 7.0f);
    near(byId(scene, 20).localCenter.x, 5.0f, "a looping strip comes round again (3 s = 1 s)");
    anp->strips[0].loop = false;
    anp->strips[0].length = 2.0f;
    anp->hold = false;
    for (Entity& e : scene) if (e.id == 20) e.localCenter.x = -1.0f;
    seq::applyAnim(s, clips, scene, 7.0f);
    near(byId(scene, 20).localCenter.x, -1.0f, "with hold off, an ended strip leaves the object alone");
    anp->hold = true;

    // --- Split -----------------------------------------------------------------
    std::printf("split\n");
    {
        const int uid = anp->strips[0].uid;
        const int right = seq::split(s, uid, 5.0f);
        check(right > 0, "split inside a strip makes a right half", std::to_string(right));
        seq::Track* owner = nullptr;
        seq::Strip* r = s.stripByUid(right, &owner);
        seq::Strip* l = s.stripByUid(uid);
        check(r && l && owner == anp, "both halves on the same track", "");
        if (r && l) {
            near(l->end(), 5.0f, "left half ends at the cut");
            near(r->start, 5.0f, "right half starts at the cut");
            near(r->offset, 1.0f, "right half plays on from where the left stopped");
            seq::applyAnim(s, clips, scene, 5.5f);
            near(byId(scene, 20).localCenter.x, 7.5f, "the pose across a split is unchanged");
        }
        same(seq::split(s, uid, 9.0f), -1, "splitting outside a strip does nothing");
    }

    // --- Fades -----------------------------------------------------------------
    std::printf("fades\n");
    {
        seq::Strip a; a.start = 1.0f; a.length = 4.0f; a.fadeIn = 1.0f; a.fadeOut = 2.0f;
        near(a.fadeAt(1.0f), 0.0f, "silent at the start of a fade in");
        near(a.fadeAt(1.5f), 0.5f, "half way through the S curve");
        near(a.fadeAt(3.0f), 1.0f, "full in the middle");
        near(a.fadeAt(4.0f), 0.5f, "half way out");
        near(a.fadeAt(5.0f), 0.0f, "silent at the end");
        seq::Strip b; b.start = 0.0f; b.length = 4.0f;
        near(b.fadeAt(2.0f), 1.0f, "no fades, no envelope");
    }

    // --- Save / load -----------------------------------------------------------
    std::printf("save / load\n");
    {
        seq::Track& au = s.addTrack(seq::Kind::Audio, "Score");
        au.gain = 0.5f; au.channel = "Music"; au.solo = true;
        seq::Strip m; m.uid = s.newUid(); m.start = 1.0f; m.length = 8.0f; m.offset = 2.0f;
        m.sound = "theme.ogg"; m.gain = 0.8f; m.fadeIn = 0.5f; m.fadeOut = 1.5f;
        au.strips.push_back(m);
        s.name = "Intro"; s.duration = 12.0f; s.fps = 25.0f; s.playOnStart = true; s.releaseCamera = false;

        nlohmann::json j;
        seq::save(j, {s});
        std::vector<seq::Sequence> back;
        seq::load(nlohmann::json::parse(j.dump()), back);
        same(static_cast<int>(back.size()), 1, "one sequence comes back");
        if (!back.empty()) {
            const seq::Sequence& b = back[0];
            check(b.name == "Intro" && b.playOnStart && !b.releaseCamera, "settings survive", b.name);
            near(b.duration, 12.0f, "duration survives");
            near(b.fps, 25.0f, "fps survives");
            same(static_cast<int>(b.tracks.size()), static_cast<int>(s.tracks.size()), "every track");
            check(b.tracks.back().kind == seq::Kind::Audio && b.tracks.back().channel == "Music" &&
                  b.tracks.back().solo, "audio track and its channel", b.tracks.back().name);
            const seq::Strip& bm = b.tracks.back().strips.at(0);
            check(bm.sound == "theme.ogg", "the strip's sound", bm.sound);
            near(bm.offset, 2.0f, "the strip's offset");
            near(bm.fadeOut, 1.5f, "the strip's fade out");
            same(seq::cutAt(b, 2.5f).camera, seq::cutAt(s, 2.5f).camera, "it cuts the same");
            check(b.nextUid > bm.uid, "new uids never collide with saved ones",
                  std::to_string(b.nextUid));
        }
    }

    // --- Recorded camera path ---------------------------------------------------
    std::printf("recorded camera path\n");
    {
        const float yaws[]    = {0.0f, 90.0f, 200.0f, -45.0f};
        const float pitches[] = {0.0f, 20.0f, -35.0f, 60.0f};
        for (float yaw : yaws)
            for (float pitch : pitches) {
                const glm::vec3 r = seq::cameraRotationFor(yaw, pitch);
                const glm::vec3 got = glm::quat(glm::radians(r)) * glm::vec3(0.0f, 0.0f, -1.0f);
                const float y = glm::radians(yaw), p = glm::radians(pitch);
                const glm::vec3 want(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p));
                char buf[96];
                std::snprintf(buf, sizeof(buf), "yaw %.0f pitch %.0f: |d| = %.5f", yaw, pitch,
                              glm::length(got - want));
                check(glm::length(got - want) < 1e-4f, "a static camera looks where the fly camera did", buf);
            }
        std::vector<CamKey> path{{3.0f, {1, 2, 3}, 10.0f, 5.0f, 60.0f},
                                 {5.0f, {4, 5, 6}, 30.0f, -5.0f, 50.0f}};
        const anim::Clip c = seq::cameraClipFromPath(path, 10, "Path");
        same(static_cast<int>(c.tracks.size()), 7, "position, rotation and fov channels");
        near(c.lastKeyTime(), 2.0f, "the path starts at clip time 0");
        anim::apply(c, scene, 2.0f);
        near(byId(scene, 10).localCenter.y, 5.0f, "the camera ends where the path did");
        const CameraComponent* cc = byId(scene, 10).components.get<CameraComponent>();
        near(cc ? cc->fov : 0.0f, 50.0f, "...with the path's field of view");
    }

    std::printf(g_fails ? "\n%d check(s) FAILED\n" : "\nall sequencer checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
