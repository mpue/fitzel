#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/world/Model.hpp>

#include "LimbIK.hpp"
#include "Retarget.hpp"

// Hold poses: first-person arms holding a weapon, set up by hand.
//
// A shooter's arms are one skinned model with a still clip per weapon
// ("hold_<weapon>": the hands on that weapon's grips) and, per weapon, where the
// arms' model sits in the weapon's own frame -- so wherever the game puts the
// gun (bob, kick, a reload, a look at a new one), the hands go with it. That
// placement is a small Lua table the game reads (DARKNESS: scripts/doom/arm_holds.lua,
// one `name = { r = {9 numbers, rows}, t = {x, y, z} }` per weapon, plus the arms'
// `scale` and `centre`, the box centre their model is drawn about).
//
// Everything here is in the game's view space: the eye at the origin, looking
// down -Z, +Y up, +X right. The weapon's frame is where the game holds it in
// front of the eye (`off`: right, up, ahead; `aim`: pitch, yaw, roll on top);
// the arms hang off that frame exactly as the game draws them (see armsMatrix).
//
// An edit is the hands' wrists in the weapon's frame, how far each elbow is
// swung out, how each finger is curled on top of the clip, and the arms'
// placement. The solve takes the clip's pose and reaches each arm (two bones,
// shoulder and elbow) to its wrist. Written back, the solved pose becomes the
// clip and the placement the Lua entry.
//
// Plain data and maths -- no GL, no ImGui -- so the Hold Pose Studio window and
// its check harness run the very same code. Editor only.
namespace holdpose {

// --- The setup ---------------------------------------------------------------------

// How the game holds one weapon in front of the eye.
struct Weapon {
    std::string model;                   // its .glb, relative to the recipe's folder
    glm::vec3   scale{1.0f};             // the game's size against the model's own
    glm::vec3   off{0.2f, -0.2f, 0.5f};  // metres right, up, ahead of the eye
    glm::vec3   aim{0.0f};               // degrees: pitch, yaw, roll (engine Euler)
};

// What a set of arms is held with, kept beside its model as "<arms>.holdposes"
// (JSON).
struct Recipe {
    std::string holds;                   // the Lua table, relative to the recipe's folder
    float       fov = 60.0f;             // the game's vertical field of view (degrees)
    std::map<std::string, Weapon> poses; // by name: the clip is "hold_<name>"
};

std::string recipePath(const std::string& arms);   // <arms>.holdposes
bool loadRecipe(const std::string& arms, Recipe& r);
bool saveRecipe(const std::string& arms, const Recipe& r);
// A path of the recipe (relative to its folder) on disk, and back.
std::string resolve(const std::string& arms, const std::string& rel);
std::string relative(const std::string& arms, const std::string& file);

// --- The Lua table ---------------------------------------------------------------------

// Where the arms' model sits in one weapon's frame: its origin, and its turn.
struct Placement {
    glm::vec3 t{0.0f};
    glm::mat3 r{1.0f};
};
// The table's shared numbers: the arms' size in the game and the box centre the
// game draws them about (model units).
struct HoldsInfo {
    float     scale = 1.0f;
    glm::vec3 centre{0.0f};
    bool      haveCentre = false;
};
bool readHoldsInfo(const std::string& file, HoldsInfo& info, std::string& err);
// One weapon's entry; false (no error) when the table has none for `name`.
bool readPlacement(const std::string& file, const std::string& name, Placement& p, std::string& err);
// Put `p` into the table as `name` -- replacing the numbers of an entry that is
// there (its layout stays), or adding one before the table's closing brace.
bool writePlacement(const std::string& file, const std::string& name, const Placement& p, std::string& err);

// --- Frames ----------------------------------------------------------------------------------

// The engine's Euler (degrees): Rz(roll) * Ry(yaw) * Rx(pitch).
glm::mat3 euler(const glm::vec3& pitchYawRoll);
// view <- weapon frame.
glm::mat4 weaponFrame(const Weapon& w);
// The middle of a model's box (all its primitives' vertices, bind pose) -- what
// the engine draws a model about -- and the box's size.
void bounds(const fitzel::ModelData& md, glm::vec3& centre, glm::vec3& size);
// view <- weapon model, as the game draws it.
glm::mat4 weaponMatrix(const Weapon& w, const glm::vec3& modelCentre);
// view <- arms model, as the game draws them (scripts/doom/body.lua): the
// entity in the middle of where the model's origin goes at `p.t` plus the table's
// centre, turned by the weapon's turn times `p.r`, scaled by `info.scale` about
// the box centre the engine finds in the model (`engineCentre`).
glm::mat4 armsMatrix(const glm::mat4& weapon, const Placement& p, const HoldsInfo& info,
                     const glm::vec3& engineCentre);

// --- The edit ----------------------------------------------------------------------------------

enum Finger : int { Thumb, Index, Middle, Ring, Little, FingerCount };
const char* fingerName(int f);

struct Hand {
    bool      on = true;                 // reached to its wrist (off: as the clip has it)
    glm::vec3 pos{0.0f};                 // the wrist joint, weapon frame (metres)
    glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f}; // its turn, weapon frame
    float     swivel = 0.0f;             // degrees the elbow is swung round the shoulder-wrist line
    std::array<float, FingerCount> curl{}; // degrees each finger closes further (- opens)
};

struct Edit {
    Placement            place;
    std::array<Hand, 2>  hands;          // [0] left, [1] right
};

// The arms' skeleton, and a hold solved on it.
class Poser {
public:
    // Finds the arms (two-bone chains) and the fingers. False: no arms found.
    bool init(const fitzel::ModelData& arms);
    bool ok() const { return m_ok; }
    const std::string& problem() const { return m_problem; }
    bool hasArm(int side) const { return m_rig.arm[static_cast<std::size_t>(side & 1)].ok(); }

    // The joints (model space) of clip `clip` at its start; the rest pose for -1.
    std::vector<glm::mat4> basePose(const fitzel::ModelData& arms, int clip) const;

    // The hands as `base` has them: their wrists in the weapon's frame.
    // `arms` view <- arms model, `weapon` view <- weapon frame.
    Hand handOf(const std::vector<glm::mat4>& base, int side, const glm::mat4& arms,
                const glm::mat4& weapon) const;

    // `base` with the edit applied: each arm reached to its wrist, swung, its
    // fingers curled. `shortBy` (optional): metres each wrist stayed short of
    // its target (out of reach).
    void solve(const std::vector<glm::mat4>& base, const Edit& e, const glm::mat4& arms,
               const glm::mat4& weapon, std::vector<glm::mat4>& G, float* shortBy = nullptr) const;

    // The middle of the palm and the way it faces (model space), as `G` has them.
    bool palm(const std::vector<glm::mat4>& G, int side, glm::vec3& centre, glm::vec3& facing) const;
    int wrist(int side) const { return m_rig.arm[static_cast<std::size_t>(side & 1)].end; }

private:
    void curl(std::vector<glm::mat4>& G, int side, const std::array<float, FingerCount>& deg) const;
    const std::vector<fitzel::SkeletonJoint>* m_skel = nullptr;
    ik::Rig     m_rig;
    // [side][finger]: up to three joints, root first (-1: none).
    std::array<std::array<std::array<int, 3>, FingerCount>, 2> m_fingers{};
    bool        m_ok = false;
    std::string m_problem;
};

// --- Writing ----------------------------------------------------------------------------------

// The pose `G` as a still clip called `name` (two equal frames): a rotation per
// joint, and a translation where a joint stands off its rest.
retarget::Clip clipOf(const std::string& name, const fitzel::ModelData& arms, const retarget::Rig& rig,
                      const std::vector<glm::mat4>& G);

// --- Picking ------------------------------------------------------------------------------------

// The nearest hit of a ray (view space) on a model drawn with `place`: distance,
// point and face normal (facing the ray). False if it misses.
bool pick(const fitzel::ModelData& md, const glm::mat4& place, const glm::vec3& origin, const glm::vec3& dir,
          float& dist, glm::vec3& point, glm::vec3& normal);

} // namespace holdpose
