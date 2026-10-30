// springcheck -- spring bones (SpringBones.hpp), measured on a made-up figure.
//
// A hip, a chest, one bone with no child whose vertices hang forward of it (as a
// chest or a belly does) and a two-bone chain hanging behind (as hair does). The
// figure stands, turns and stops as a whole, the way a clip or a script turns
// it. Checked: standing still nothing moves; turning, the hanging part lags
// behind the turn; stopped, it swings back past its pose and settles; with full
// damping it comes back without swinging past; never further than the largest
// angle allowed; a jump across the world starts it over instead of flinging it;
// amount 0 leaves the pose alone; a chain swings parent first, the child with
// it, whatever order the names are given in. No GL.
//
//   build/release/bin/springcheck.exe
//   build/release/bin/springcheck.exe --model figure.glb <clip> "<bones>" [Hz damping amount]
//       (a real figure through one of its clips: how far each bone swings;
//       QUIET=1 prints the summary alone)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../src/SpringBones.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}
std::string num(float v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

enum { Hip, Chest, Front, Hair1, Hair2, Bare, Count };

fitzel::SkeletonJoint joint(const char* name, int parent, glm::vec3 at) {
    fitzel::SkeletonJoint j;
    j.name        = name;
    j.parent      = parent;
    j.inverseBind = glm::translate(glm::mat4(1.0f), -at);
    return j;
}

// The figure. Its hanging-forward part: a cluster of vertices 15 cm in front of
// its bone, bound to it alone.
fitzel::ModelData figure() {
    fitzel::ModelData m;
    m.skeleton = {joint("hip", -1, {0, 1.0f, 0}),     joint("chest", Hip, {0, 1.3f, 0}),
                  joint("front", Chest, {0.1f, 1.35f, 0.05f}), joint("hair1", Chest, {0, 1.65f, -0.1f}),
                  joint("hair2", Hair1, {0, 1.4f, -0.15f}), joint("bare", Chest, {0, 1.5f, 0.1f})};
    // Vertices bound to one joint alone, around a point.
    fitzel::ModelPrimitive p;
    auto cluster = [&](int j, glm::vec3 c) {
        for (int i = 0; i < 9; ++i) {
            const float dx = 0.02f * static_cast<float>(i % 3 - 1), dy = 0.02f * static_cast<float>(i / 3 - 1);
            const float v[8] = {c.x + dx, c.y + dy, c.z, 0, 0, 1, 0, 0};
            p.vertices.insert(p.vertices.end(), v, v + 8);
            fitzel::VertexSkin s;
            s.joints[0]  = j;
            s.weights[0] = 1.0f;
            p.skin.push_back(s);
        }
    };
    cluster(Front, {0.1f, 1.33f, 0.2f});
    cluster(Hair2, {0.0f, 1.2f, -0.18f});   // the hair's last strand; "bare" has nothing
    m.primitives.push_back(p);
    return m;
}

// Where the palette puts a bind-space point.
glm::vec3 moved(const std::vector<glm::mat4>& pal, int j, glm::vec3 p) {
    return glm::vec3(pal[static_cast<std::size_t>(j)] * glm::vec4(p, 1.0f));
}

const glm::vec3 kFrontTip(0.1f, 1.33f, 0.2f);   // the cluster's centre

struct Run {
    springbones::System sys;
    springbones::Settings s;
    fitzel::ModelData m = figure();
    std::vector<glm::mat4> pal;
    float yaw = 0.0f;   // degrees the figure is turned
    glm::vec3 at{0.0f};

    // One frame of `dt` with the figure turned to `yaw`.
    void frame(float dt) {
        pal.assign(Count, glm::mat4(1.0f));   // the rest pose
        const glm::mat4 toWorld = glm::translate(glm::mat4(1.0f), at) *
                                  glm::rotate(glm::mat4(1.0f), glm::radians(yaw), glm::vec3(0, 1, 0));
        sys.apply(1, s, m, toWorld, pal, dt);
    }
    // The cluster's sideways offset from its pose, model space (+x = the way a
    // turn to +yaw carries it).
    float lagX() const { return moved(pal, Front, kFrontTip).x - kFrontTip.x; }
    float angle() const { return sys.angleOf(1, Front); }
};

} // namespace

// --model: a real figure's clip, standing where it is; each bone's swing over time.
int realFigure(const char* path, int clip, const std::string& bones, int argc, char** argv) {
    const fitzel::ModelData m = fitzel::loadGltf(path);
    if (m.skeleton.empty() || clip < 0 || clip >= static_cast<int>(m.animations.size())) {
        std::printf("no skeleton or no clip %d in %s\n", clip, path);
        return 1;
    }
    springbones::System sys;
    springbones::Settings s;
    s.bones = bones;
    if (argc > 5) s.frequency = static_cast<float>(std::atof(argv[5]));
    if (argc > 6) s.damping   = static_cast<float>(std::atof(argv[6]));
    if (argc > 7) s.amount    = static_cast<float>(std::atof(argv[7]));
    std::vector<int> joints;
    for (std::size_t j = 0; j < m.skeleton.size(); ++j)
        if (bones.find(m.skeleton[j].name) != std::string::npos) joints.push_back(static_cast<int>(j));
    const float dt = 1.0f / 60.0f, dur = m.animations[static_cast<std::size_t>(clip)].duration;
    std::printf("%s, clip %d (%.1f s), %zu joints named\n", path, clip, dur, joints.size());
    float most = 0.0f, sum = 0.0f;
    int   n = 0;
    for (int i = 0; i * dt < dur; ++i) {
        const float t = static_cast<float>(i) * dt;
        std::vector<glm::mat4> pal = fitzel::sampleSkeleton(m, clip, t);
        sys.apply(1, s, m, glm::mat4(1.0f), pal, dt);
        float a = 0.0f;
        for (int j : joints) a = std::max(a, sys.angleOf(1, j));
        most = std::max(most, a);
        sum += a;
        ++n;
        if (i % 15 == 0 && !std::getenv("QUIET")) std::printf("  t %5.2f  swing %5.2f deg\n", t, a);
    }
    std::printf("%.1f Hz, damping %.2f, amount %.2f: largest swing %.2f deg, mean %.2f\n", s.frequency,
                s.damping, s.amount, most, n ? sum / static_cast<float>(n) : 0.0f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 4 && std::string(argv[1]) == "--model") return realFigure(argv[2], std::atoi(argv[3]), argv[4], argc, argv);
    const float dt = 1.0f / 60.0f;

    {
        Run r;
        r.s.bones = "front";
        for (int i = 0; i < 120; ++i) r.frame(dt);
        check(r.angle() >= 0.0f && r.angle() < 0.01f, "standing still, nothing swings", num(r.angle()) + " deg");
        check(std::abs(r.lagX()) < 1e-5f, "...and the pose is the animation's", num(r.lagX()));
    }

    {
        Run r;
        r.s.bones   = "front";
        r.s.damping = 0.2f;
        float maxDuring = 0.0f, lagDuring = 0.0f;
        for (int i = 0; i < 30; ++i) {   // half a second, 180 deg/s
            r.yaw += 180.0f * dt;
            r.frame(dt);
            maxDuring = std::max(maxDuring, r.angle());
            lagDuring = r.lagX();
        }
        check(maxDuring > 2.0f, "turning, the hanging part swings", num(maxDuring) + " deg");
        // A turn to +yaw carries a point in front (+z) towards +x; lagging, it
        // stays behind: towards -x in the figure's own space.
        check(lagDuring < 0.0f, "...behind the turn, not ahead of it", num(lagDuring));
        bool passed = false;
        for (int i = 0; i < 60; ++i) {
            r.frame(dt);
            if (r.lagX() > 1e-4f) passed = true;
        }
        check(passed, "stopped, it swings back past its pose");
        for (int i = 0; i < 240; ++i) r.frame(dt);
        check(r.angle() < 0.5f, "...and settles", num(r.angle()) + " deg after 5 s");
    }

    {
        Run r;
        r.s.bones   = "front";
        r.s.damping = 1.0f;
        for (int i = 0; i < 30; ++i) { r.yaw += 180.0f * dt; r.frame(dt); }
        bool passed = false;
        for (int i = 0; i < 300; ++i) {
            r.frame(dt);
            if (r.lagX() > 1e-4f) passed = true;
        }
        check(!passed, "fully damped, it comes back without swinging past");
    }

    {
        Run r;
        r.s.bones    = "front";
        r.s.maxAngle = 10.0f;
        r.s.amount   = 2.0f;
        float most = 0.0f;
        for (int i = 0; i < 60; ++i) { r.yaw += 2000.0f * dt; r.frame(dt); most = std::max(most, r.angle()); }
        check(most <= 10.0f + 1e-3f && most > 9.0f, "a violent spin swings it no further than allowed",
              num(most) + " deg of 10");
    }

    {
        Run r;
        r.s.bones = "front";
        for (int i = 0; i < 30; ++i) r.frame(dt);
        r.at = glm::vec3(100.0f, 0.0f, 0.0f);   // a teleport
        r.frame(dt);
        check(r.angle() < 0.01f, "a jump across the world starts it over, no fling", num(r.angle()) + " deg");
    }

    {
        Run r;
        r.s.bones  = "front";
        r.s.amount = 0.0f;
        for (int i = 0; i < 30; ++i) { r.yaw += 180.0f * dt; r.frame(dt); }
        check(std::abs(r.lagX()) < 1e-6f && r.angle() < 0.0f, "amount 0 leaves the pose alone");
    }

    {
        // Named child first: they are taken parent first all the same.
        Run r;
        r.s.bones = "hair2, hair1";
        for (int i = 0; i < 30; ++i) { r.yaw += 180.0f * dt; r.frame(dt); }
        const float a1 = r.sys.angleOf(1, Hair1), a2 = r.sys.angleOf(1, Hair2);
        check(a1 > 1.0f && a2 > 0.1f, "a chain swings", num(a1) + " / " + num(a2) + " deg");
        // The child's origin goes where its parent's swing takes it.
        const glm::vec3 child = moved(r.pal, Hair2, {0, 1.4f, -0.15f});
        check(glm::length(child - glm::vec3(0, 1.4f, -0.15f)) > 1e-3f, "...the child with its parent");
        // A bone keeps its length, swung or not.
        const glm::vec3 head = moved(r.pal, Hair1, {0, 1.65f, -0.1f});
        check(std::abs(glm::length(child - head) - glm::length(glm::vec3(0, 0.25f, 0.05f))) < 1e-4f,
              "...and keeps its length");
        r.s.bones = "nosuchbone, bare";
        r.frame(dt);
        check(r.sys.angleOf(1, Hair1) < 0.0f && r.sys.angleOf(1, Bare) < 0.0f,
              "a name not on the rig, or a bone with nothing on it, swings nothing");
    }

    std::printf("\n%s (%d failed)\n", failures ? "FAILED" : "all ok", failures);
    return failures ? 1 : 0;
}
