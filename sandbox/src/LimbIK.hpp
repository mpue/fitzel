#pragma once

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/world/Model.hpp>   // SkeletonJoint, ModelData

// --- Inverse kinematics: feet on the ground, hands where they are sent ----------
// An animation is made on a flat floor. Put the figure on a slope, a kerb or a
// staircase and one foot hangs in the air while the other sinks into the step;
// send it to open a door and the hand swings where the clip says, not to the
// handle. This bends the limbs after the animation and before the skinning:
//
//   - Feet (the IK component): under each foot the ground is looked up (roads,
//     bridges, steps, the terrain as it is drawn); the body comes down by what
//     the lower foot needs, each leg reaches its foot onto its own ground --
//     keeping the height the animation lifts it by, so a stride is still a
//     stride -- and a foot that stands tilts with the slope.
//   - Hands (game.reach): a point in the world for the left or the right hand,
//     this frame, with a weight to blend in and out by.
//
// Each limb is a two-bone chain -- hip, knee, ankle; shoulder, elbow, wrist --
// solved in the skeleton's own space, so bone lengths never change. The knee
// keeps bending the way the animation bends it, the foot and the hand keep the
// orientation the animation gives them. The chains are found on the skeleton by
// structure, not by one rig's names: the foot (or hand), then its parent and
// its parent's parent, stepping over twist bones -- which is the same on a
// Character Creator, Mixamo, Unreal, Blender or Daz rig. Names in the
// component override it for a rig that it gets wrong.
namespace ik {

// Three joints of one limb (-1: not found).
struct Chain {
    int root = -1, mid = -1, end = -1;
    bool ok() const { return root >= 0 && mid >= 0 && end >= 0; }
};

// The limbs of a skeleton: [0] left, [1] right.
struct Rig {
    std::array<Chain, 2> leg, arm;
};

// The limbs found on `skel` by structure (see above).
Rig findRig(const std::vector<fitzel::SkeletonJoint>& skel);
// A chain named "root, mid, end" (exact joint names); not ok if any is missing.
Chain chainNamed(const std::vector<fitzel::SkeletonJoint>& skel, const std::string& names);

// Solve one two-bone chain on model-space joint transforms `G` (joint, not
// palette -- see palette()/joints()): the end reaches `target`, or as near as
// the bones allow, straight towards it. The middle joint bends towards where it
// bends now, or towards `poleHint` when the chain is straight. With `keepEnd`
// the end joint keeps its orientation. Every joint below a turned one turns
// with it. False when the chain is degenerate (a zero-length bone).
bool solveTwoBone(std::vector<glm::mat4>& G, const std::vector<fitzel::SkeletonJoint>& skel,
                  const Chain& c, const glm::vec3& target, const glm::vec3& poleHint, bool keepEnd);
// Turn joint `root` and everything below it by `q` round `pivot` (model space).
void rotateSubtree(std::vector<glm::mat4>& G, const std::vector<fitzel::SkeletonJoint>& skel,
                   int root, const glm::vec3& pivot, const glm::quat& q);
// Palette (joint * inverseBind, what sampleSkeleton gives) <-> joint transforms.
std::vector<glm::mat4> joints(const std::vector<glm::mat4>& palette,
                              const std::vector<fitzel::SkeletonJoint>& skel);
std::vector<glm::mat4> palette(const std::vector<glm::mat4>& joints,
                               const std::vector<fitzel::SkeletonJoint>& skel);

// How the feet are placed (the IK component's settings).
struct FeetOptions {
    bool  feet     = true;   // put the feet on the ground at all
    bool  align    = true;   // a standing foot tilts with the slope
    float maxStep  = 0.5f;   // metres a foot may be raised or the body lowered;
                             // ground higher than this is a wall, not a step
    float response = 12.0f;  // 1/s: how fast the legs follow the ground
    std::string leftLeg, rightLeg, leftArm, rightArm;   // "root, mid, end" overrides
};

// The ground under a point: the first surface straight down from `from`, up to
// `maxDist` below it -- its height and its normal. False when there is none.
using GroundFn = std::function<bool(const glm::vec3& from, float maxDist, float& y, glm::vec3& normal)>;

// The per-figure part: what the legs were doing last frame (they follow the
// ground smoothly, not in jumps), and the hands sent somewhere this frame.
class System {
public:
    // Hand `side` (0 left, 1 right) of figure `id` to `target` (world) this
    // frame, `weight` 0..1 of the way from where the animation has it.
    void reach(int id, int side, const glm::vec3& target, float weight);
    bool reaching(int id) const { return m_reach.count(id) != 0; }

    // Bend figure `id`'s limbs in `palette` (sampleSkeleton's, in place).
    // `toWorld` is how the model is drawn (model space -> world); `baseY` the
    // height its feet stand at when the animation is on flat ground (the
    // bottom of its box). `feet` null: hands only.
    void apply(int id, const FeetOptions* feet, const fitzel::ModelData& model, const glm::mat4& toWorld,
               float baseY, std::vector<glm::mat4>& palette, const GroundFn& ground, float dt);

    // After the frame's figures: the reaches were for this frame only.
    void endFrame() { m_reach.clear(); }
    void clear() { m_reach.clear(); m_state.clear(); m_rigs.clear(); }

private:
    struct Reach {
        glm::vec3 target{0.0f};
        float     weight = 0.0f;
        bool      on     = false;
    };
    struct State {
        float     pelvis = 0.0f;               // metres the body is lowered by (<= 0)
        float     lift[2] = {0.0f, 0.0f};      // metres each foot's ground is above the base
        glm::vec3 normal[2] = {glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)};
        bool      fresh = true;
    };
    const Rig& rigOf(const fitzel::ModelData& model, const FeetOptions* o);
    std::unordered_map<int, std::array<Reach, 2>> m_reach;
    std::unordered_map<int, State> m_state;
    // Found once per skeleton (and set of overrides).
    std::unordered_map<std::string, Rig> m_rigs;
};

} // namespace ik
