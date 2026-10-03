// ikcheck -- inverse kinematics (LimbIK.hpp), measured on a made-up figure.
//
// A skeleton a metre tall at the hip -- legs with a twist bone between thigh and
// calf, arms with one between forearm and hand, toes and an index finger -- in
// the naming of five rigs (Mixamo, Character Creator, Unreal, Blender, Daz).
// Checked: the limbs are found on every one; a two-bone chain puts its end on
// the target and keeps its bones' lengths, bends the knee the way it bent, keeps
// the foot's turn and carries the toes along; an unreachable target gets a
// straight limb pointing at it; feet on a step, in a hole, over a wall and on a
// slope; the legs following the ground over frames, not in one jump; a hand
// sent to a point, part of the way, and given back after the frame. No GL.
//
//   build/release/bin/ikcheck.exe
//   build/release/bin/ikcheck.exe --model a.glb b.glb ...   (real figures: which
//       joints the limbs are found on, and one leg and one arm bent on each)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../src/LimbIK.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}
std::string str(const glm::vec3& v) {
    char b[96];
    std::snprintf(b, sizeof b, "%.4f %.4f %.4f", v.x, v.y, v.z);
    return b;
}
bool near(const glm::vec3& a, const glm::vec3& b, float tol = 1e-3f) { return glm::length(a - b) <= tol; }

// The joints, by role. Each side: thigh, thigh twist, calf, foot, toe, upper
// arm, forearm, forearm twist, hand, index finger.
enum Role { Thigh, ThighTwist, Calf, Foot, Toe, Upper, Fore, ForeTwist, Hand, Index, kRoles };
constexpr int kHips = 0, kSpine = 1;
int at(int side, Role r) { return 2 + side * kRoles + r; }

// Where each joint stands in the bind pose (left side; the right mirrors x).
// The knee is a little forward, the elbow a little back, as in any rig.
glm::vec3 bindPos(Role r) {
    switch (r) {
        case Thigh:      return {0.10f, 0.95f, 0.00f};
        case ThighTwist: return {0.10f, 0.735f, 0.015f};
        case Calf:       return {0.10f, 0.52f, 0.03f};
        case Foot:       return {0.10f, 0.08f, 0.00f};
        case Toe:        return {0.10f, 0.02f, 0.12f};
        case Upper:      return {0.20f, 1.45f, 0.00f};
        case Fore:       return {0.45f, 1.45f, -0.03f};
        case ForeTwist:  return {0.575f, 1.45f, -0.015f};
        case Hand:       return {0.70f, 1.45f, 0.00f};
        case Index:      return {0.78f, 1.45f, 0.02f};
        default:         return {};
    }
}

using Namer = std::function<std::string(int side, Role r)>;
std::vector<fitzel::SkeletonJoint> skeleton(const std::string& hips, const std::string& spine, const Namer& name) {
    std::vector<fitzel::SkeletonJoint> s(2 + 2 * kRoles);
    auto put = [&](int i, const std::string& n, int parent, glm::vec3 p) {
        s[static_cast<std::size_t>(i)].name        = n;
        s[static_cast<std::size_t>(i)].parent      = parent;
        s[static_cast<std::size_t>(i)].inverseBind = glm::inverse(glm::translate(glm::mat4(1.0f), p));
    };
    put(kHips, hips, -1, {0.0f, 1.0f, 0.0f});
    put(kSpine, spine, kHips, {0.0f, 1.3f, 0.0f});
    for (int side = 0; side < 2; ++side) {
        const float mx = side == 0 ? 1.0f : -1.0f;
        for (int r = 0; r < kRoles; ++r) {
            glm::vec3 p = bindPos(static_cast<Role>(r));
            p.x *= mx;
            int parent = at(side, static_cast<Role>(r - 1));
            if (r == Thigh) parent = kHips;
            if (r == Upper) parent = kSpine;
            put(at(side, static_cast<Role>(r)), name(side, static_cast<Role>(r)), parent, p);
        }
    }
    return s;
}

std::vector<fitzel::SkeletonJoint> mixamo() {
    return skeleton("mixamorig:Hips", "mixamorig:Spine", [](int side, Role r) {
        const std::string s = side == 0 ? "Left" : "Right";
        const char* n[kRoles] = {"UpLeg", "UpLegTwist", "Leg", "Foot", "ToeBase",
                                 "Arm", "ForeArm", "ForeArmTwist", "Hand", "HandIndex1"};
        return "mixamorig:" + s + n[r];
    });
}

glm::vec3 posOf(const glm::mat4& m) { return glm::vec3(m[3]); }

} // namespace

// --model: the limbs of real figures, and each one bent once.
int realFigures(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
        const fitzel::ModelData m = fitzel::loadGltf(argv[i]);
        std::printf("%s: %zu joints\n", argv[i], m.skeleton.size());
        if (m.skeleton.empty()) { std::printf("  (no skeleton: not a figure)\n"); continue; }
        const ik::Rig rig = ik::findRig(m.skeleton);
        auto name = [&](int j) { return j >= 0 ? m.skeleton[static_cast<std::size_t>(j)].name : std::string("-"); };
        const std::vector<glm::mat4> rest(m.skeleton.size(), glm::mat4(1.0f));
        const char* limbs[4] = {"left leg", "right leg", "left arm", "right arm"};
        const ik::Chain chains[4] = {rig.leg[0], rig.leg[1], rig.arm[0], rig.arm[1]};
        for (int k = 0; k < 4; ++k) {
            const ik::Chain& c = chains[k];
            std::printf("  %-9s %s > %s > %s\n", limbs[k], name(c.root).c_str(), name(c.mid).c_str(), name(c.end).c_str());
            if (!c.ok()) { check(false, std::string(limbs[k]) + " found"); continue; }
            // Bent: the end a fifth of the way in towards the root, and a little
            // forward -- within reach of a limb held straight (a T-pose arm).
            std::vector<glm::mat4> G = ik::joints(rest, m.skeleton);
            const glm::vec3 H = posOf(G[static_cast<std::size_t>(c.root)]);
            const glm::vec3 K = posOf(G[static_cast<std::size_t>(c.mid)]);
            const glm::vec3 A = posOf(G[static_cast<std::size_t>(c.end)]);
            const float a = glm::length(K - H), b = glm::length(A - K);
            const glm::vec3 target = A + (H - A) * 0.2f + glm::vec3(0.0f, 0.0f, 0.05f) * (a + b);
            ik::solveTwoBone(G, m.skeleton, c, target, glm::vec3(0, 0, 1), true);
            const glm::vec3 K2 = posOf(G[static_cast<std::size_t>(c.mid)]);
            const glm::vec3 A2 = posOf(G[static_cast<std::size_t>(c.end)]);
            check(glm::length(A2 - target) < 1e-3f * (a + b) && std::abs(glm::length(K2 - H) - a) < 1e-3f * a &&
                      std::abs(glm::length(A2 - K2) - b) < 1e-3f * b,
                  std::string(limbs[k]) + ": bent onto its target, bones the same length");
        }
    }
    std::printf("\nikcheck --model: %s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 2 && std::string(argv[1]) == "--model") return realFigures(argc, argv);

    // --- The limbs, found on five rigs' naming --------------------------------------------
    std::printf("Finding the limbs\n");
    struct Scheme { const char* rig; std::vector<fitzel::SkeletonJoint> skel; };
    const std::vector<Scheme> schemes = {
        {"Mixamo", mixamo()},
        {"Character Creator", skeleton("CC_Base_Hip", "CC_Base_Spine01", [](int side, Role r) {
             const std::string s = side == 0 ? "L" : "R";
             const char* n[kRoles] = {"Thigh", "ThighTwist01", "Calf", "Foot", "ToeBase",
                                      "Upperarm", "Forearm", "ForearmTwist01", "Hand", "Index1"};
             return "CC_Base_" + s + "_" + n[r];
         })},
        {"Unreal", skeleton("pelvis", "spine_01", [](int side, Role r) {
             const std::string s = side == 0 ? "_l" : "_r";
             const char* n[kRoles] = {"thigh", "thigh_twist_01", "calf", "foot", "ball",
                                      "upperarm", "lowerarm", "lowerarm_twist_01", "hand", "index_01"};
             return std::string(n[r]) + s;
         })},
        {"Blender", skeleton("hips", "spine", [](int side, Role r) {
             const std::string s = side == 0 ? ".L" : ".R";
             const char* n[kRoles] = {"thigh", "thigh_twist", "shin", "foot", "toe",
                                      "upper_arm", "forearm", "forearm_twist", "hand", "f_index.01"};
             return std::string(n[r]) + s;
         })},
        {"Daz", skeleton("hip", "abdomenLower", [](int side, Role r) {
             const std::string s = side == 0 ? "l" : "r";
             const char* n[kRoles] = {"ThighBend", "ThighTwist", "Shin", "Foot", "Toe",
                                      "ShldrBend", "ForearmBend", "ForearmTwist", "Hand", "Index1"};
             return s + n[r];
         })},
    };
    for (const Scheme& sc : schemes) {
        const ik::Rig rig = ik::findRig(sc.skel);
        bool ok = true;
        for (int side = 0; side < 2; ++side) {
            const ik::Chain& l = rig.leg[static_cast<std::size_t>(side)];
            const ik::Chain& a = rig.arm[static_cast<std::size_t>(side)];
            ok = ok && l.root == at(side, Thigh) && l.mid == at(side, Calf) && l.end == at(side, Foot) &&
                 a.root == at(side, Upper) && a.mid == at(side, Fore) && a.end == at(side, Hand);
        }
        check(ok, std::string(sc.rig) + ": both legs and both arms found, twist bones stepped over");
    }
    {
        const auto skel = mixamo();
        const ik::Chain c = ik::chainNamed(skel, " mixamorig:LeftUpLeg, mixamorig:LeftLeg ,mixamorig:LeftFoot");
        const ik::Chain bad = ik::chainNamed(skel, "mixamorig:LeftUpLeg, nobody, mixamorig:LeftFoot");
        check(c.root == at(0, Thigh) && c.mid == at(0, Calf) && c.end == at(0, Foot) && !bad.ok(),
              "a chain by its names (spaces allowed); a missing name gives none");
    }

    const auto skel = mixamo();
    const ik::Rig rig = ik::findRig(skel);
    const std::vector<glm::mat4> rest(skel.size(), glm::mat4(1.0f));   // the bind pose's palette

    // --- One chain ----------------------------------------------------------------------------
    std::printf("A two-bone chain\n");
    {
        std::vector<glm::mat4> G = ik::joints(rest, skel);
        const ik::Chain& leg = rig.leg[0];
        const glm::vec3 H = posOf(G[static_cast<std::size_t>(leg.root)]);
        const glm::vec3 K0 = posOf(G[static_cast<std::size_t>(leg.mid)]);
        const glm::vec3 A0 = posOf(G[static_cast<std::size_t>(leg.end)]);
        const glm::vec3 toe0 = posOf(G[static_cast<std::size_t>(at(0, Toe))]) - A0;
        const float a = glm::length(K0 - H), b = glm::length(A0 - K0);
        const glm::vec3 target = A0 + glm::vec3(0.0f, 0.25f, 0.10f);
        check(ik::solveTwoBone(G, skel, leg, target, glm::vec3(0, 0, 1), true), "the chain solves");
        const glm::vec3 K = posOf(G[static_cast<std::size_t>(leg.mid)]);
        const glm::vec3 A = posOf(G[static_cast<std::size_t>(leg.end)]);
        check(near(A, target, 1e-4f), "the end lands on the target", str(A));
        check(std::abs(glm::length(K - H) - a) < 1e-4f && std::abs(glm::length(A - K) - b) < 1e-4f &&
                  near(posOf(G[static_cast<std::size_t>(leg.root)]), H, 1e-5f),
              "the bones keep their lengths, the hip stays put");
        // Forward of the line from hip to foot: the knee bends the way it did.
        const glm::vec3 d = glm::normalize(A - H);
        const glm::vec3 off = (K - H) - d * glm::dot(K - H, d);
        check(off.z > 0.02f, "the knee bends forward, as it did", str(off));
        const glm::vec3 toe = posOf(G[static_cast<std::size_t>(at(0, Toe))]) - A;
        check(near(toe, toe0, 1e-4f), "the foot keeps its turn, and the toes come with it", str(toe));
        check(near(posOf(G[static_cast<std::size_t>(at(1, Foot))]), posOf(ik::joints(rest, skel)[static_cast<std::size_t>(at(1, Foot))]), 1e-6f),
              "the other leg is not touched");

        std::vector<glm::mat4> F = ik::joints(rest, skel);
        const glm::vec3 far = H + glm::vec3(0.0f, -3.0f, 0.5f);
        ik::solveTwoBone(F, skel, leg, far, glm::vec3(0, 0, 1), true);
        const glm::vec3 Af = posOf(F[static_cast<std::size_t>(leg.end)]);
        check(std::abs(glm::length(Af - H) - (a + b)) < 1e-3f &&
                  glm::dot(glm::normalize(Af - H), glm::normalize(far - H)) > 0.9999f,
              "out of reach: the limb straight, pointing at the target", str(Af));
    }

    // --- Feet ---------------------------------------------------------------------------------
    std::printf("Feet on the ground\n");
    ik::System sys;
    ik::FeetOptions opt;   // feet on, aligned, half a metre, 12/s
    std::function<float(float, float)> height = [](float, float) { return 0.0f; };
    const ik::GroundFn ground = [&](const glm::vec3& from, float maxDist, float& y, glm::vec3& n) {
        y = height(from.x, from.z);
        if (y > from.y || from.y - y > maxDist) return false;
        const float e = 0.01f;
        n = glm::normalize(glm::vec3(y - height(from.x + e, from.z), e, y - height(from.x, from.z + e)));
        return true;
    };
    const glm::mat4 I(1.0f);
    fitzel::ModelData model;
    model.skeleton = skel;
    // The figure's pose after the feet: the joints, for figure `id`.
    auto pose = [&](int id, float dt) {
        std::vector<glm::mat4> p = rest;
        sys.apply(id, &opt, model, I, 0.0f, p, ground, dt);
        return ik::joints(p, skel);
    };
    const std::vector<glm::mat4> R = ik::joints(rest, skel);
    auto P = [](const std::vector<glm::mat4>& G, int j) { return posOf(G[static_cast<std::size_t>(j)]); };
    const int lf = at(0, Foot), rf = at(1, Foot);

    {
        const auto G = pose(1, 1.0f / 60.0f);
        check(near(P(G, lf), P(R, lf), 1e-4f) && near(P(G, rf), P(R, rf), 1e-4f) && near(P(G, kHips), P(R, kHips), 1e-4f),
              "on flat ground nothing moves");
    }
    {
        height = [](float x, float) { return x > 0.0f ? 0.2f : 0.0f; };   // a step under the left foot
        const auto G = pose(2, 1.0f / 60.0f);
        check(std::abs(P(G, lf).y - 0.28f) < 1e-3f && near(P(G, rf), P(R, rf), 1e-3f) &&
                  near(P(G, kHips), P(R, kHips), 1e-4f),
              "a step: that foot goes up onto it, the body stays", str(P(G, lf)));
    }
    {
        height = [](float x, float) { return x > 0.0f ? -0.3f : 0.0f; };  // a hole under the left foot
        const auto G = pose(3, 1.0f / 60.0f);
        check(std::abs(P(G, kHips).y - 0.7f) < 1e-3f && std::abs(P(G, lf).y + 0.22f) < 1e-3f &&
                  std::abs(P(G, rf).y - 0.08f) < 1e-3f,
              "a hole: the body comes down by it, that foot reaches in, the other bends",
              "hips " + str(P(G, kHips)) + ", left " + str(P(G, lf)) + ", right " + str(P(G, rf)));
        const float thigh = glm::length(P(G, at(1, Calf)) - P(G, at(1, Thigh)));
        check(std::abs(thigh - glm::length(P(R, at(1, Calf)) - P(R, at(1, Thigh)))) < 1e-4f,
              "...without a bone stretched");
    }
    {
        height = [](float x, float) { return x > 0.0f ? 0.9f : 0.0f; };   // a ledge, waist high
        const auto G = pose(4, 1.0f / 60.0f);
        check(near(P(G, lf), P(R, lf), 1e-3f) && near(P(G, kHips), P(R, kHips), 1e-4f),
              "a ledge higher than a step is not stood on: the foot stays as animated", str(P(G, lf)));
        height = [](float x, float) { return x > 0.0f ? -0.8f : 0.0f; };  // a drop, deeper than a step
        const auto D = pose(11, 1.0f / 60.0f);
        check(std::abs(P(D, kHips).y - 0.5f) < 1e-3f,
              "a drop: the body comes down no further than the max step", str(P(D, kHips)));
    }
    {
        const float k = std::tan(glm::radians(20.0f));
        height = [k](float, float z) { return k * z; };                    // rising 20 degrees ahead
        const auto G = pose(5, 1.0f / 60.0f);
        const glm::vec3 toe0 = P(R, at(0, Toe)) - P(R, lf), toe = P(G, at(0, Toe)) - P(G, lf);
        const float turn = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(toe0), glm::normalize(toe)), -1.0f, 1.0f)));
        check(std::abs(turn - 20.0f) < 0.5f && toe.y > toe0.y, "a slope: a standing foot tilts with it, toes up",
              std::to_string(turn) + " degrees");
        opt.align = false;
        const auto H = pose(6, 1.0f / 60.0f);
        check(near(P(H, at(0, Toe)) - P(H, lf), toe0, 1e-4f), "...and stays level with the tilt turned off");
        opt.align = true;
    }
    {
        // Over frames: figure 7 stands on the flat, then the step comes.
        height = [](float, float) { return 0.0f; };
        pose(7, 1.0f / 60.0f);
        height = [](float x, float) { return x > 0.0f ? 0.2f : 0.0f; };
        const float one = pose(7, 1.0f / 60.0f)[static_cast<std::size_t>(lf)][3].y;
        float last = one;
        for (int f = 0; f < 120; ++f) last = pose(7, 1.0f / 60.0f)[static_cast<std::size_t>(lf)][3].y;
        check(one > 0.085f && one < 0.2f && std::abs(last - 0.28f) < 1e-3f,
              "the legs follow the ground over frames, not in a jump",
              "after one frame " + std::to_string(one) + ", after two seconds " + std::to_string(last));
    }

    // --- Hands --------------------------------------------------------------------------------
    std::printf("Hands\n");
    {
        height = [](float, float) { return 0.0f; };
        const glm::vec3 handRest = P(R, at(0, Hand));
        const glm::vec3 index0 = P(R, at(0, Index)) - handRest;
        const glm::vec3 target(0.40f, 1.20f, 0.30f);   // within the arm's reach
        sys.reach(10, 0, target, 1.0f);
        check(sys.reaching(10), "a hand sent somewhere is waiting for the figure");
        std::vector<glm::mat4> p = rest;
        sys.apply(10, nullptr, model, I, 0.0f, p, ground, 1.0f / 60.0f);
        auto G = ik::joints(p, skel);
        check(near(P(G, at(0, Hand)), target, 1e-4f), "game.reach: the hand is on the point", str(P(G, at(0, Hand))));
        check(near(P(G, at(0, Index)) - P(G, at(0, Hand)), index0, 1e-4f) && near(P(G, at(1, Hand)), P(R, at(1, Hand)), 1e-6f),
              "...keeping its turn (the fingers with it); the other hand stays");
        sys.reach(10, 0, target, 0.5f);
        p = rest;
        sys.apply(10, nullptr, model, I, 0.0f, p, ground, 1.0f / 60.0f);
        G = ik::joints(p, skel);
        check(near(P(G, at(0, Hand)), glm::mix(handRest, target, 0.5f), 1e-4f), "at weight 0.5, half the way");
        sys.endFrame();
        p = rest;
        sys.apply(10, nullptr, model, I, 0.0f, p, ground, 1.0f / 60.0f);
        check(!sys.reaching(10) && near(posOf(p[static_cast<std::size_t>(at(0, Hand))] * glm::inverse(skel[static_cast<std::size_t>(at(0, Hand))].inverseBind)), handRest, 1e-6f),
              "the frame over, the hand is the animation's again");
    }

    std::printf("\nikcheck: %s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
