// retargetcheck -- the retargeting pipeline end to end, without a window.
//
//   1. finds Blender and converts a motion file with it (cached),
//   2. reads the motion and a character, maps both onto the humanoid template
//      and prints the maps (plus synthetic Mixamo / Unreal / BVH skeletons,
//      whose expected maps are known),
//   3. retargets the motion and compares it, frame by frame and bone by bone,
//      with a reference clip already in a file (the Python prototype's walk_f
//      on Vicky) -- angles in degrees, hip position in centimetres,
//   4. bakes it into a copy of the character, repairs included, and loads that
//      back through the engine's own loader.
//
//   retargetcheck [--src motion.fbx] [--target char.glb] [--ref model.glb clip]
//                 [--noref] [--out dir]
// Exit code 0 when every check passed.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/world/Model.hpp>

#include "../src/Blender.hpp"
#include "../src/Retarget.hpp"

namespace fs = std::filesystem;
using namespace retarget;

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

glm::mat3 rotOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) r[c] = glm::normalize(r[c]);
    return r;
}
float angleDeg(const glm::mat3& a, const glm::mat3& b) {
    const glm::mat3 d = glm::transpose(a) * b;
    const float c = std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
    return std::acos(c) * 57.29578f;
}

void printMap(const Rig& r, const BoneMap& m) {
    int g = -1;
    for (int s = 0; s < SlotCount; ++s) {
        if (slotInfo(s).group != g) { g = slotInfo(s).group; std::printf("    %s:", groupName(g)); }
        const int n = m[static_cast<std::size_t>(s)];
        std::printf(" %s=%s", slotInfo(s).label, n >= 0 ? r.nodes[static_cast<std::size_t>(n)].name.c_str() : "-");
        if (s + 1 == SlotCount || slotInfo(s + 1).group != g) std::printf("\n");
    }
}

// A skeleton from (name, parent name) pairs, for the naming tests.
Rig synthetic(const std::vector<std::pair<std::string, std::string>>& bones) {
    Rig r;
    for (const auto& [name, parent] : bones) {
        Node n;
        n.name = name;
        n.parent = parent.empty() ? -1 : r.find(parent);
        n.joint = true;
        r.order.push_back(static_cast<int>(r.nodes.size()));
        r.nodes.push_back(n);
    }
    return r;
}

std::string slotBone(const Rig& r, const BoneMap& m, int s) {
    const int n = m[static_cast<std::size_t>(s)];
    return n >= 0 ? r.nodes[static_cast<std::size_t>(n)].name : std::string("-");
}

void namingTests() {
    std::printf("\n== naming conventions\n");
    {
        const std::string p = "mixamorig:";
        std::vector<std::pair<std::string, std::string>> b = {
            {p + "Hips", ""}, {p + "Spine", p + "Hips"}, {p + "Spine1", p + "Spine"}, {p + "Spine2", p + "Spine1"},
            {p + "Neck", p + "Spine2"}, {p + "Head", p + "Neck"}, {p + "HeadTop_End", p + "Head"}};
        for (const char* S : {"Left", "Right"}) {
            const std::string s = S;
            b.push_back({p + s + "Shoulder", p + "Spine2"});
            b.push_back({p + s + "Arm", p + s + "Shoulder"});
            b.push_back({p + s + "ForeArm", p + s + "Arm"});
            b.push_back({p + s + "Hand", p + s + "ForeArm"});
            for (const char* F : {"Thumb", "Index", "Middle", "Ring", "Pinky"})
                for (int k = 1; k <= 4; ++k)
                    b.push_back({p + s + "Hand" + F + std::to_string(k),
                                 k == 1 ? p + s + "Hand" : p + s + "Hand" + F + std::to_string(k - 1)});
            b.push_back({p + s + "UpLeg", p + "Hips"});
            b.push_back({p + s + "Leg", p + s + "UpLeg"});
            b.push_back({p + s + "Foot", p + s + "Leg"});
            b.push_back({p + s + "ToeBase", p + s + "Foot"});
            b.push_back({p + s + "Toe_End", p + s + "ToeBase"});
        }
        const Rig r = synthetic(b);
        const BoneMap m = autoMap(r);
        std::printf("  Mixamo: %d of %d slots\n", mappedCount(m), SlotCount);
        check(slotBone(r, m, Hips) == p + "Hips" && slotBone(r, m, Spine) == p + "Spine" &&
              slotBone(r, m, Chest) == p + "Spine1" && slotBone(r, m, UpperChest) == p + "Spine2", "Mixamo spine");
        check(slotBone(r, m, LUpperArm) == p + "LeftArm" && slotBone(r, m, LLowerArm) == p + "LeftForeArm" &&
              slotBone(r, m, LShoulder) == p + "LeftShoulder", "Mixamo left arm");
        check(slotBone(r, m, RUpperLeg) == p + "RightUpLeg" && slotBone(r, m, RLowerLeg) == p + "RightLeg" &&
              slotBone(r, m, RToes) == p + "RightToeBase", "Mixamo right leg");
        check(slotBone(r, m, LLittle3) == p + "LeftHandPinky3" && slotBone(r, m, RThumb1) == p + "RightHandThumb1",
              "Mixamo fingers");
        check(mappedCount(m) == SlotCount - 2, "Mixamo: everything but pelvis and upper neck");
    }
    {
        std::vector<std::pair<std::string, std::string>> b = {
            {"root", ""}, {"pelvis", "root"}, {"spine_01", "pelvis"}, {"spine_02", "spine_01"},
            {"spine_03", "spine_02"}, {"spine_04", "spine_03"}, {"spine_05", "spine_04"}, {"neck_01", "spine_05"},
            {"neck_02", "neck_01"}, {"head", "neck_02"}};
        for (const char* s : {"l", "r"}) {
            const std::string x = std::string("_") + s;
            b.push_back({"clavicle" + x, "spine_05"});
            b.push_back({"upperarm" + x, "clavicle" + x});
            b.push_back({"upperarm_twist_01" + x, "upperarm" + x});
            b.push_back({"lowerarm" + x, "upperarm" + x});
            b.push_back({"hand" + x, "lowerarm" + x});
            b.push_back({"index_metacarpal" + x, "hand" + x});
            b.push_back({"index_01" + x, "index_metacarpal" + x});
            b.push_back({"index_02" + x, "index_01" + x});
            b.push_back({"index_03" + x, "index_02" + x});
            b.push_back({"thigh" + x, "pelvis"});
            b.push_back({"thigh_twist_01" + x, "thigh" + x});
            b.push_back({"calf" + x, "thigh" + x});
            b.push_back({"foot" + x, "calf" + x});
            b.push_back({"ball" + x, "foot" + x});
        }
        const Rig r = synthetic(b);
        const BoneMap m = autoMap(r);
        std::printf("  Unreal 5: %d of %d slots\n", mappedCount(m), SlotCount);
        check(slotBone(r, m, Hips) == "pelvis" && slotBone(r, m, Pelvis) == "-", "UE hips are the pelvis");
        check(slotBone(r, m, Spine) == "spine_01" && slotBone(r, m, Chest) == "spine_03" &&
              slotBone(r, m, UpperChest) == "spine_05", "UE spine");
        check(slotBone(r, m, Neck) == "neck_01" && slotBone(r, m, UpperNeck) == "neck_02", "UE neck");
        check(slotBone(r, m, LLowerArm) == "lowerarm_l" && slotBone(r, m, RLowerLeg) == "calf_r" &&
              slotBone(r, m, LToes) == "ball_l" && slotBone(r, m, LIndex1) == "index_01_l", "UE limbs");
    }
    {
        std::vector<std::pair<std::string, std::string>> b = {
            {"Hips", ""}, {"LowerBack", "Hips"}, {"Spine", "LowerBack"}, {"Spine1", "Spine"},
            {"Neck", "Spine1"}, {"Neck1", "Neck"}, {"Head", "Neck1"}};
        for (const char* S : {"Left", "Right"}) {
            const std::string s = S, c = s.substr(0, 1);
            b.push_back({c + "HipJoint", "Hips"});
            b.push_back({s + "UpLeg", c + "HipJoint"});
            b.push_back({s + "Leg", s + "UpLeg"});
            b.push_back({s + "Foot", s + "Leg"});
            b.push_back({s + "ToeBase", s + "Foot"});
            b.push_back({s + "Shoulder", "Spine1"});
            b.push_back({s + "Arm", s + "Shoulder"});
            b.push_back({s + "ForeArm", s + "Arm"});
            b.push_back({s + "Hand", s + "ForeArm"});
            b.push_back({c + "Thumb", s + "Hand"});
        }
        const Rig r = synthetic(b);
        const BoneMap m = autoMap(r);
        std::printf("  CMU BVH: %d of %d slots\n", mappedCount(m), SlotCount);
        check(slotBone(r, m, Hips) == "Hips" && slotBone(r, m, Spine) == "LowerBack" &&
              slotBone(r, m, UpperChest) == "Spine1" && slotBone(r, m, UpperNeck) == "Neck1", "BVH spine");
        check(slotBone(r, m, LUpperLeg) == "LeftUpLeg" && slotBone(r, m, RLowerArm) == "RightForeArm" &&
              slotBone(r, m, LThumb1) == "LThumb", "BVH limbs");
    }
}

} // namespace
int main(int argc, char** argv) {
    std::string src = "D:/models/Motion/walk_normal_f.fbx";
    std::string target = "D:/fitzel_projects/treetest/models/vicky.glb.vor-claude-backup";
    std::string ref = "D:/fitzel_projects/treetest/models/vicky.glb", refClip = "walk_f";
    std::string outDir = (fs::temp_directory_path() / "retargetcheck").generic_string();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--src" && i + 1 < argc) src = argv[++i];
        else if (a == "--target" && i + 1 < argc) target = argv[++i];
        else if (a == "--ref" && i + 2 < argc) { ref = argv[++i]; refClip = argv[++i]; }
        else if (a == "--noref") ref.clear();
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
    }
    namingTests();

    std::printf("\n== Blender\n");
    const blender::Install& bl = blender::find();
    std::printf("  %s  (version %s%s)\n", bl.exe.empty() ? "(none)" : bl.exe.c_str(), bl.version.c_str(),
                bl.chosen ? ", chosen" : ", found");
    // The rest needs real files (and Blender for an FBX). A machine without
    // them skips it rather than failing: the naming tests above still ran.
    std::error_code fe;
    if (!fs::exists(src, fe) || !fs::exists(target, fe) || (blender::needsBlender(src) && bl.exe.empty())) {
        std::printf("\n  files part skipped: %s / %s / Blender not all here\n", src.c_str(), target.c_str());
        std::printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
        return g_fail ? 1 : 0;
    }
    if (!ref.empty() && !fs::exists(ref, fe)) ref.clear();
    std::string srcGlb = src;
    if (blender::needsBlender(src)) {
        const auto t0 = std::chrono::steady_clock::now();
        const blender::Result res = blender::toGlb(src);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  %s -> %s: %s (%.1f s)\n", fs::path(src).filename().string().c_str(), res.glb.c_str(),
                    res.message.c_str(), secs);
        check(res.ok, "conversion");
        if (!res.ok) { std::printf("%s\n", res.log.c_str()); return 1; }
        srcGlb = res.glb;
    }

    std::printf("\n== source\n");
    const Rig srcRig = loadRig(srcGlb);
    std::printf("  %zu nodes, %d joints, %zu motions\n", srcRig.nodes.size(), srcRig.jointCount(), srcRig.motions.size());
    for (const Motion& m : srcRig.motions)
        std::printf("    \"%s\": %.0f fps, %.3f s, %d frames, %zu channels\n", m.name.c_str(), m.fps, m.duration,
                    m.frames(), m.channels.size());
    check(srcRig.ok() && !srcRig.motions.empty(), "source has a skeleton and a motion");
    const BoneMap sm = autoMap(srcRig);
    const int srcMotion = std::max(0, motionIndex(srcRig, ""));
    if (!srcRig.motions.empty())
        std::printf("  takes \"%s\"\n", srcRig.motions[static_cast<std::size_t>(srcMotion)].name.c_str());
    printMap(srcRig, sm);

    std::printf("\n== target\n");
    const Rig tgtRig = loadRig(target, false);
    std::printf("  %zu nodes, %d joints\n", tgtRig.nodes.size(), tgtRig.jointCount());
    check(tgtRig.ok(), "target has a skeleton");
    const BoneMap tm = autoMap(tgtRig);
    printMap(tgtRig, tm);
    std::printf("  mapped: source %d, target %d of %d\n", mappedCount(sm), mappedCount(tm), SlotCount);

    const Transfer tx(srcRig, sm, tgtRig, tm);
    check(tx.valid(), tx.valid() ? "transfer" : tx.problem().c_str());
    if (!tx.valid()) return 1;
    std::printf("  %d bones, hip scale %.3f, facing turn %.1f deg, %d more bones copied by name\n", tx.bones(),
                tx.hipScale(), std::atan2(tx.facing()[2][0], tx.facing()[0][0]) * 57.29578f, tx.copiedBones());

    // Bones copied by name: each turns away from its bind pose as its
    // namesake in the motion does (same rig family, so the same axes).
    if (tx.copiedBones() > 0) {
        std::vector<glm::mat4> sw, W;
        float worst = 0.0f;
        std::string worstName;
        const Motion& m0 = srcRig.motions[static_cast<std::size_t>(std::max(0, motionIndex(srcRig, "")))];
        const int mi = std::max(0, motionIndex(srcRig, ""));
        for (int k = 0; k < m0.frames(); k += 4) {
            srcRig.sample(mi, static_cast<float>(k) / m0.fps, sw);
            tx.pose(sw, glm::vec3(0.0f), W);
            for (int node : tx.animatedNodes()) {
                const std::string& name = tgtRig.nodes[static_cast<std::size_t>(node)].name;
                bool slot = false;
                for (int s = 0; s < SlotCount; ++s) slot = slot || tm[static_cast<std::size_t>(s)] == node;
                if (slot) continue;
                const int sn = srcRig.find(name);
                if (sn < 0) continue;
                // Local turn away from rest, target vs source.
                const int tp = tgtRig.nodes[static_cast<std::size_t>(node)].parent;
                const int sp = srcRig.nodes[static_cast<std::size_t>(sn)].parent;
                const glm::mat3 tl = glm::transpose(rotOf(W[static_cast<std::size_t>(tp)])) * rotOf(W[static_cast<std::size_t>(node)]);
                const glm::mat3 sl = glm::transpose(rotOf(sw[static_cast<std::size_t>(sp)])) * rotOf(sw[static_cast<std::size_t>(sn)]);
                const glm::mat3 td = glm::transpose(glm::mat3_cast(tgtRig.nodes[static_cast<std::size_t>(node)].r)) * tl;
                const glm::mat3 sd = glm::transpose(glm::mat3_cast(srcRig.nodes[static_cast<std::size_t>(sn)].r)) * sl;
                const float a = angleDeg(td, sd);
                if (a > worst) { worst = a; worstName = name; }
            }
        }
        std::printf("  copied bones: worst %.3f deg off their namesakes' turn (%s)\n", worst, worstName.c_str());
        check(worst < 0.5f, "bones copied by name turn like their namesakes");
    }

    // Whatever the proportions, every mapped bone must point where the source
    // bone points (both turned to face the same way).
    {
        std::printf("\n== bone directions, character vs motion\n");
        std::vector<glm::mat4> sw, W;
        float worst = 0.0f;
        int worstSlot = -1;
        std::vector<float> per(SlotCount, 0.0f);
        const Motion& m0 = srcRig.motions[static_cast<std::size_t>(srcMotion)];
        for (int k = 0; k < m0.frames(); k += 3) {
            const float t = static_cast<float>(k) / m0.fps;
            srcRig.sample(srcMotion, t, sw);
            tx.pose(sw, glm::vec3(0.0f), W);
            for (int s = 0; s < SlotCount; ++s) {
                // A bone runs from its slot to the next one down the chain; the
                // links from the trunk to a limb (pelvis -> thigh, chest ->
                // shoulder) are the trunk's shape, not a bone that turns.
                const int c = slotInfo(s).parent;
                if (c < 0 || c == Hips || c == Pelvis || c == UpperChest || c == LHand || c == RHand) continue;
                const int sa = sm[static_cast<std::size_t>(c)], sb = sm[static_cast<std::size_t>(s)];
                const int ta = tm[static_cast<std::size_t>(c)], tb = tm[static_cast<std::size_t>(s)];
                if (sa < 0 || sb < 0 || ta < 0 || tb < 0) continue;
                const glm::vec3 ds = glm::mat3(tx.facing()) * (glm::vec3(sw[static_cast<std::size_t>(sb)][3]) - glm::vec3(sw[static_cast<std::size_t>(sa)][3]));
                const glm::vec3 dt = glm::vec3(W[static_cast<std::size_t>(tb)][3]) - glm::vec3(W[static_cast<std::size_t>(ta)][3]);
                if (glm::length(ds) < 1e-4f || glm::length(dt) < 1e-4f) continue;
                const float a = std::acos(std::clamp(glm::dot(glm::normalize(ds), glm::normalize(dt)), -1.0f, 1.0f)) * 57.29578f;
                per[static_cast<std::size_t>(c)] = std::max(per[static_cast<std::size_t>(c)], a);
                if (a > worst) { worst = a; worstSlot = c; }
            }
        }
        for (int s = 0; s < SlotCount; ++s)
            if (per[static_cast<std::size_t>(s)] > 2.0f)
                std::printf("    %-10s %-12s worst %5.2f deg\n", groupName(slotInfo(s).group), slotInfo(s).label,
                            per[static_cast<std::size_t>(s)]);
        std::printf("  worst %.2f deg (%s %s)\n", worst, worstSlot >= 0 ? groupName(slotInfo(worstSlot).group) : "-",
                    worstSlot >= 0 ? slotInfo(worstSlot).label : "-");
        check(worst < 8.0f, "limbs point the way the motion's limbs point");
    }

    if (!ref.empty()) {
        std::printf("\n== against %s \"%s\"\n", fs::path(ref).filename().string().c_str(), refClip.c_str());
        const Rig refRig = loadRig(ref);
        int refMotion = -1;
        for (std::size_t i = 0; i < refRig.motions.size(); ++i)
            if (refRig.motions[i].name == refClip) refMotion = static_cast<int>(i);
        const bool same = refMotion >= 0 && refRig.nodes.size() == tgtRig.nodes.size();
        check(same, "reference clip found, same skeleton");
        if (same) {
            const Motion& refM = refRig.motions[static_cast<std::size_t>(refMotion)];
            float worstLimb = 0.0f, worstHip = 0.0f;
            std::vector<double> sum(SlotCount, 0.0);
            std::vector<float> worst(SlotCount, 0.0f);
            int n = 0;
            std::vector<glm::mat4> sw, W, R;
            const int frames = std::min(refM.frames(), srcRig.motions[static_cast<std::size_t>(srcMotion)].frames());
            for (int k = 0; k < frames; k += 2) {
                const float t = static_cast<float>(k) / refM.fps;
                srcRig.sample(srcMotion, t, sw);
                tx.pose(sw, glm::vec3(0.0f), W);
                refRig.sample(refMotion, t, R);
                for (int s = 0; s < SlotCount; ++s) {
                    const int node = tm[static_cast<std::size_t>(s)];
                    if (node < 0 || sm[static_cast<std::size_t>(s)] < 0) continue;
                    const float a = angleDeg(rotOf(W[static_cast<std::size_t>(node)]), rotOf(R[static_cast<std::size_t>(node)]));
                    sum[static_cast<std::size_t>(s)] += a;
                    worst[static_cast<std::size_t>(s)] = std::max(worst[static_cast<std::size_t>(s)], a);
                }
                const int hip = tm[Hips];
                worstHip = std::max(worstHip, glm::length(glm::vec3(W[static_cast<std::size_t>(hip)][3]) -
                                                          glm::vec3(R[static_cast<std::size_t>(hip)][3])));
                ++n;
            }
            for (int s = 0; s < SlotCount; ++s) {
                if (worst[static_cast<std::size_t>(s)] <= 0.0f) continue;
                const bool limb = s >= LShoulder && s <= RToes;
                if (limb) worstLimb = std::max(worstLimb, worst[static_cast<std::size_t>(s)]);
                if (worst[static_cast<std::size_t>(s)] > 0.5f)
                    std::printf("    %-10s %-12s mean %5.2f  worst %5.2f deg\n", groupName(slotInfo(s).group),
                                slotInfo(s).label, sum[static_cast<std::size_t>(s)] / n, worst[static_cast<std::size_t>(s)]);
            }
            std::printf("  %d frames compared; arms and legs worst %.2f deg; hips worst %.1f cm\n", n, worstLimb,
                        worstHip * 100.0f);
            check(worstLimb < 3.0f, "arms and legs within 3 degrees of the reference");
            check(worstHip < 0.03f, "hips within 3 cm of the reference");
        }
    }

    std::printf("\n== bake\n");
    std::error_code ec;
    fs::create_directories(outDir, ec);
    const ModelInfo before = inspect(target);
    std::printf("  before: %d extra skins, %zu loose meshes\n", before.extraSkins, before.looseMeshes.size());
    for (const std::string& name : before.looseMeshes) std::printf("    loose: %s\n", name.c_str());
    Options o;
    const Clip clip = bake(srcRig, srcMotion, tx, tgtRig, o, "retargetcheck_walk");
    std::printf("  clip: %d frames, %.3f s, %zu rotation tracks\n", clip.frames(), clip.duration(), clip.nodes.size());
    const std::string out = outDir + "/retargetcheck.glb";
    std::string err;
    std::vector<std::string> notes;
    const bool wrote = writeModel(target, out, {clip}, Repair{}, tm, err, &notes);
    for (const std::string& note : notes) std::printf("    %s\n", note.c_str());
    check(wrote, wrote ? "written" : err.c_str());
    const ModelInfo after = inspect(out);
    check(after.extraSkins == 0 && after.looseMeshes.empty(), "repairs: no extra skin, nothing loose");
    const fitzel::ModelData md = fitzel::loadGltf(out);
    int found = -1;
    for (std::size_t i = 0; i < md.animations.size(); ++i)
        if (md.animations[i].name == "retargetcheck_walk") found = static_cast<int>(i);
    std::printf("  engine loads %zu primitives, %zu joints, %zu clips\n", md.primitives.size(), md.skeleton.size(),
                md.animations.size());
    check(found >= 0 && std::abs(md.animations[static_cast<std::size_t>(found)].duration - clip.duration()) < 1e-3f,
          "the engine plays the new clip at its length");
    int skinnedPrims = 0;
    for (const fitzel::ModelPrimitive& p : md.primitives) skinnedPrims += p.skin.empty() ? 0 : 1;
    std::printf("  %d of %zu primitives skinned\n", skinnedPrims, md.primitives.size());
    check(skinnedPrims == static_cast<int>(md.primitives.size()), "every primitive follows the skeleton");

    // The recipe round: original kept once, every bake from it, an outside
    // change taken as the new original.
    std::printf("\n== recipe and bakes\n");
    {
        const std::string model = outDir + "/baked.glb";
        fs::copy_file(target, model, fs::copy_options::overwrite_existing, ec);
        fs::remove(fs::path(originalPath(model)), ec);
        fs::remove(fs::path(recipePath(model)), ec);
        Recipe r;
        ClipEntry e;
        e.name = "walk_a";
        e.source = src;
        e.inPlace = true;
        r.clips.push_back(e);
        const auto source = [&](const ClipEntry&) -> const Rig* { return &srcRig; };
        std::string msg;
        const bool ok1 = bakeAll(model, r, source, msg);
        std::printf("  1st: %s\n", msg.c_str());
        check(ok1 && fs::exists(originalPath(model)) && !r.bakedHash.empty(), "first bake keeps the original");
        const auto size1 = fs::file_size(model, ec);
        Recipe again;
        check(loadRecipe(model, again) && again.clips.size() == 1 && again.clips[0].inPlace &&
                  again.bakedHash == r.bakedHash, "the recipe is saved and reads back");
        const bool ok2 = bakeAll(model, again, source, msg);
        std::printf("  2nd: %s\n", msg.c_str());
        check(ok2 && fs::file_size(model, ec) == size1, "a second bake starts from the original (same size)");
        again.clips.push_back(e);
        again.clips.back().name = "walk_b";
        again.clips.back().inPlace = false;
        const bool ok3 = bakeAll(model, again, source, msg);
        const ModelInfo info3 = inspect(model);
        std::printf("  3rd: %s -> %zu clips\n", msg.c_str(), info3.clips.size());
        check(ok3 && info3.clips.size() == before.clips.size() + 2, "two clips plus the original ones");
        // In place: the hips end where they started (horizontally).
        const Rig baked = loadRig(model);
        for (const char* name : {"walk_a", "walk_b"}) {
            const int mi = motionIndex(baked, name);
            if (mi < 0 || baked.motions[static_cast<std::size_t>(mi)].name != name) continue;
            std::vector<glm::mat4> w0, w1;
            baked.sample(mi, 0.0f, w0);
            baked.sample(mi, baked.motions[static_cast<std::size_t>(mi)].duration, w1);
            const glm::vec3 d = glm::vec3(w1[static_cast<std::size_t>(tm[Hips])][3]) - glm::vec3(w0[static_cast<std::size_t>(tm[Hips])][3]);
            std::printf("  %s: hips travel %.2f m\n", name, std::sqrt(d.x * d.x + d.z * d.z));
        }
        // Somebody else saves the model: that becomes the new original.
        fs::copy_file(target, model, fs::copy_options::overwrite_existing, ec);
        const bool ok4 = bakeAll(model, again, source, msg);
        std::printf("  4th: %s\n", msg.c_str());
        check(ok4 && msg.find(".orig") != std::string::npos, "an outside change becomes the new original");
    }

    std::printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}