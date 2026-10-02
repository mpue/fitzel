#pragma once

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Retargeting: a motion recorded on one skeleton, played by another.
//
// The source is any rigged glTF -- or an FBX/BVH/.blend that Blender has turned
// into one (Blender.hpp). Both skeletons are first put into the same words: a
// humanoid template of named slots (hips, spine, left upper arm ...), filled in
// automatically from bone names and the shape of the hierarchy, correctable by
// hand. Then every frame, per slot:
//
//     target world rotation = D_src(t) * O * bind_target
//
// where D_src(t) is how far the source bone has turned from its own bind pose
// and O lines the target bone up with the source bone's direction at rest (the
// shortest arc between the two). So an A-pose character plays a T-pose
// recording without its arms ending up 40 degrees off. The hips also carry the
// source's travel, scaled by the ratio of the two hip heights.
//
// Everything here is plain data -- no GL, no ImGui -- so the panel, the bake and
// the check harness (retargetcheck) all run the very same code. Editor only.
namespace retarget {

// --- A skeleton and its motions, as one glTF file has them ------------------------

struct Node {
    std::string name;
    int         parent = -1;
    glm::vec3   t{0.0f};                     // rest pose, local
    glm::quat   r{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3   s{1.0f};
    glm::mat4   bind{1.0f};                  // world matrix in the bind pose
    bool        joint = false;               // part of the skeleton (skin joint, or under the armature)
};

// One animation of the file, kept as its channels and sampled on demand: a
// library of fifty clips would cost hundreds of megabytes as baked frames.
struct Motion {
    struct Channel {
        int node = -1;
        int path = 0;                        // 0 translation, 1 rotation, 2 scale
        int interp = 0;                      // 0 linear, 1 step, 2 cubic spline
        std::vector<float>     times;
        std::vector<glm::vec4> values;       // xyz(w); cubic: in, value, out per key
    };
    std::string          name;
    float                fps = 30.0f;        // the rate the keys were recorded at
    float                duration = 0.0f;    // seconds
    std::vector<Channel> channels;
    int frames() const;                      // whole frames at `fps`, both ends included
};

struct Rig {
    std::string              path;
    std::vector<Node>        nodes;
    std::vector<int>         order;          // parents before children
    std::vector<int>         skinJoints;     // the first skin's joints (empty: no skin)
    std::vector<Motion>      motions;
    std::string              error;          // why it could not be read ("" = fine)

    bool ok() const { return error.empty() && !nodes.empty(); }
    int  find(const std::string& name) const; // -1 when absent
    int  jointCount() const;
    // Every node's world matrix at time `t` of motion `m` (rest where a node
    // has no channel; the rest pose itself for m < 0).
    void sample(int m, float t, std::vector<glm::mat4>& world) const;
};

// Read the skeleton (and, with `motions`, every animation) of a .glb/.gltf.
Rig loadRig(const std::string& path, bool motions = true);

// --- The humanoid template ---------------------------------------------------------

enum Slot : int {
    Hips, Pelvis, Spine, Chest, UpperChest, Neck, UpperNeck, Head,
    LShoulder, LUpperArm, LLowerArm, LHand,
    RShoulder, RUpperArm, RLowerArm, RHand,
    LUpperLeg, LLowerLeg, LFoot, LToes,
    RUpperLeg, RLowerLeg, RFoot, RToes,
    LThumb1, LThumb2, LThumb3, LIndex1, LIndex2, LIndex3, LMiddle1, LMiddle2, LMiddle3,
    LRing1, LRing2, LRing3, LLittle1, LLittle2, LLittle3,
    RThumb1, RThumb2, RThumb3, RIndex1, RIndex2, RIndex3, RMiddle1, RMiddle2, RMiddle3,
    RRing1, RRing2, RRing3, RLittle1, RLittle2, RLittle3,
    SlotCount
};

enum Group : int { GBody, GLeftArm, GRightArm, GLeftLeg, GRightLeg, GLeftHand, GRightHand, GroupCount };

struct SlotInfo {
    const char* key;     // stable id, saved in recipes ("leftUpperArm")
    const char* label;   // what the panel calls it ("Upper arm")
    int         group;
    int         parent;  // the slot above it (-1 for the hips), for drawing
};
const SlotInfo& slotInfo(int slot);
const char*     groupName(int group);
int             slotByKey(const std::string& key); // -1 if unknown

using BoneMap = std::array<int, SlotCount>; // node per slot, -1 = not mapped
BoneMap emptyMap();

// Fill the template from the rig's bone names and hierarchy: names find the
// head, hands, upper arms, thighs and feet; the hierarchy finds the rest (hips
// are where the legs and the spine meet, the spine bones lie between hips and
// head, the forearm is the bone between upper arm and hand ...). Copes with
// Character Creator / ActorCore, Mixamo, Daz Genesis, Unreal, Rigify, VRM and
// BVH naming.
BoneMap autoMap(const Rig& rig);
int     mappedCount(const BoneMap& m);
// Which way the skeleton faces at rest (horizontal unit vector): its left side
// crossed with up, from the thighs (else the arms).
glm::vec3 facing(const Rig& rig, const BoneMap& m);

// The map as bone names, and back. Names that are not in `rig` are skipped.
std::map<std::string, std::string> mapToNames(const Rig& rig, const BoneMap& m);
void applyNames(const Rig& rig, const std::map<std::string, std::string>& names, BoneMap& m);

// --- Transfer ----------------------------------------------------------------------

struct Options {
    float start = 0.0f, end = 0.0f;  // source seconds; end <= start = to the end
    bool  inPlace = false;           // take the travel out (sway and bob stay)
};

// The source and target lined up once; then pose() turns any source frame into
// a target pose. Cheap to build -- the panel rebuilds it on every mapping edit.
class Transfer {
public:
    Transfer() = default;
    Transfer(const Rig& src, const BoneMap& srcMap, const Rig& tgt, const BoneMap& tgtMap);

    bool               valid() const { return m_problem.empty() && !m_plan.empty(); }
    const std::string& problem() const { return m_problem; }
    int                bones() const { return static_cast<int>(m_plan.size()); }
    float              hipScale() const { return m_scale; }
    // The source turned to face the way the target faces (applied to every
    // source world matrix before anything else).
    const glm::mat4&   facing() const { return m_yaw; }

    // Target world matrices (every node) for source world matrices `srcWorld`
    // (raw, as Rig::sample gives them). `shift` is taken off the source hips'
    // travel first (source units, after facing) -- see drift().
    void pose(const std::vector<glm::mat4>& srcWorld, const glm::vec3& shift,
              std::vector<glm::mat4>& tgtWorld) const;
    // The same, plus the local rotation of every animatedNodes() entry (in
    // that order) and the hips' local translation -- what a clip stores.
    void poseLocal(const std::vector<glm::mat4>& srcWorld, const glm::vec3& shift,
                   std::vector<glm::mat4>& tgtWorld, std::vector<glm::quat>& localRot,
                   glm::vec3& hipT) const;

    // What "in place" takes off at source time `t`: the hips' horizontal
    // offset at the start plus the straight-line travel since.
    glm::vec3 drift(const Rig& src, int motion, const Options& o, float t) const;

    // The target nodes the transfer writes (a rotation each; the hips also move).
    std::vector<int> animatedNodes() const;
    // Bones outside the template carried over by name (see the constructor):
    // between two characters of one rig family, the rest of the skeleton.
    int              copiedBones() const { return m_copied; }
    int              hipNode() const { return m_tgtHip; }
    int              srcHipNode() const { return m_srcHip; }

private:
    struct Step { int src = -1, tgt = -1; glm::mat3 srcBindInv{1.0f}, base{1.0f}; };
    void run(const std::vector<glm::mat4>& srcWorld, const glm::vec3& shift,
             std::vector<glm::mat4>& tgtWorld, std::vector<glm::quat>* localRot,
             glm::vec3* hipT) const;
    std::vector<int>  m_animated;      // target nodes with a step or a copy, in node order
    struct Copy { int src = -1, srcParent = -1; glm::quat srcRestInv{1.0f, 0.0f, 0.0f, 0.0f}; };
    std::vector<Copy> m_copyOf;        // target node -> the same-named source bone (src -1: none)
    int               m_copied = 0;
    const Rig*        m_tgt = nullptr;
    std::vector<Step> m_plan;          // per mapped slot, in slot order
    std::vector<int>  m_stepOf;        // target node -> plan index (-1)
    glm::mat4         m_yaw{1.0f};
    float             m_scale = 1.0f;
    int               m_srcHip = -1, m_tgtHip = -1;
    glm::vec3         m_srcHipBind{0.0f}, m_tgtHipBind{0.0f};
    std::string       m_problem;
};

// A finished clip in the target's own terms: local rotations per animated
// node per frame, plus the hips' local translation.
struct Clip {
    std::string                          name;
    float                                fps = 30.0f;
    std::vector<int>                     nodes;   // target nodes with a rotation track
    std::vector<std::vector<glm::quat>>  rot;     // [node][frame]
    int                                  hipNode = -1;
    std::vector<glm::vec3>               hipT;    // [frame]
    int   frames() const { return hipT.empty() ? (rot.empty() ? 0 : static_cast<int>(rot[0].size()))
                                               : static_cast<int>(hipT.size()); }
    float duration() const { return frames() > 1 ? static_cast<float>(frames() - 1) / fps : 0.0f; }
};

// The source range [start, end] in seconds, clamped to the motion.
void clipRange(const Rig& src, int motion, const Options& o, float& start, float& end);

// Retarget motion `motion` of `src` into a clip called `name`, frame by frame
// at the source's own rate.
Clip bake(const Rig& src, int motion, const Transfer& tx, const Rig& tgt,
          const Options& o, const std::string& name);

// --- Writing the target --------------------------------------------------------------

// Fixes a rig needs before it can play anything -- found on Daz exports:
//  * more than one skin: the engine binds every mesh to the FIRST skin, so a
//    garment skinned to its own copy of the skeleton would hang off joint 0.
//    Its joints are pointed at the body's joints of the same name.
//  * meshes without a skin (hair, eyelashes): drawn where they stood, while
//    the body walks away. Each is bound whole to the nearest mapped bone.
struct Repair {
    bool mergeSkins = true;
    bool bindLoose  = true;
};
// What a .glb holds, read from its header alone (no meshes, no images).
struct ModelInfo {
    bool                     ok = false;         // a readable glTF
    bool                     binary = false;     // .glb -- the only kind a bake can write
    bool                     skinned = false;    // has a skin: a character
    int                      extraSkins = 0;     // skins beyond the first with joints of their own
    std::vector<std::string> looseMeshes;        // mesh nodes without a skin (in a skinned model)
    std::vector<std::string> clips;              // the animations in the file
    bool needsRepair() const { return extraSkins > 0 || !looseMeshes.empty(); }
};
ModelInfo inspect(const std::string& path);

// Write `base` + `clips` (+ repairs) to `out`. Clips already in the file under
// one of the new names are replaced; every other clip stays. `tgtMap` picks the
// bones loose meshes are bound to. Written to a temporary file first and moved
// over `out`, so a failure never leaves half a model behind.
bool writeModel(const std::string& base, const std::string& out, const std::vector<Clip>& clips,
                const Repair& repair, const BoneMap& tgtMap, std::string& err,
                std::vector<std::string>* notes = nullptr);

// --- The recipe -------------------------------------------------------------------------

// What a character's animations are made of, kept beside the model as
// "<model>.retarget" (JSON): its untouched original lives in "<model>.orig", and
// every bake writes original + all clips afresh -- so a clip can be redone or
// taken out any time, and nothing piles up in the file.
struct ClipEntry {
    std::string name;                         // the clip's name in the model
    std::string source;                       // the motion file (absolute, or relative to the model)
    std::string take;                         // which animation of it ("" = the first)
    float       start = 0.0f, end = 0.0f;     // seconds; 0/0 = all of it
    bool        inPlace = false;
    std::map<std::string, std::string> srcMap; // slot key -> source bone (hand-made choices)
};
struct Recipe {
    std::vector<ClipEntry>             clips;
    std::map<std::string, std::string> tgtMap; // slot key -> target bone (hand-made choices)
    Repair                             repair;
    std::string                        bakedHash; // the model as the last bake left it
};

std::string recipePath(const std::string& model);  // <model>.retarget
std::string originalPath(const std::string& model); // <model>.orig
bool loadRecipe(const std::string& model, Recipe& r);
// The file a bake starts from: the saved original -- unless there is none yet,
// or the model was changed by something else since the last bake; then the
// model itself (and the bake makes it the new original).
std::string baseFor(const std::string& model, const Recipe& r);
bool saveRecipe(const std::string& model, const Recipe& r);
// Where `source` is on disk (relative paths are taken from the model's folder).
std::string sourceFile(const std::string& model, const std::string& source);
// How a source file is written into the recipe: relative to the model's folder
// when it lies inside `projectDir`, absolute otherwise.
std::string sourceRef(const std::string& model, const std::string& projectDir, const std::string& file);
// A content hash of a file (FNV-1a 64, hex) -- "did somebody else change it?".
std::string fileHash(const std::string& path);

// A clip name made of a file name: lower case, letters, digits and _.
std::string clipNameFor(const std::string& file);
// ...and of an animation's name in its file ("Armature|walk_slow" -> "walk_slow").
std::string clipNameForTake(const std::string& take);

// Bake every clip of `r` into `model`. `source` hands over the loaded source
// rig of a clip (nullptr = not available). The original is saved first if it
// is not there yet -- and taken anew when the model changed outside this tool
// since the last bake. One line for the status bar in `message`.
bool bakeAll(const std::string& model, Recipe& r,
             const std::function<const Rig*(const ClipEntry&)>& source,
             std::string& message);

// Which animation of `rig` a clip means: `take` by name, else the one that
// moves the most bones (the longest of those); -1 if there is none.
int motionIndex(const Rig& rig, const std::string& take);

} // namespace retarget