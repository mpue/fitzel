#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Model.hpp>   // SkeletonJoint, ModelData

// --- Spring bones: what hangs on a figure swings after it ------------------------
// An animation moves the skeleton it was made for and nothing else: a ponytail, an
// earring, a belly, a chest that no channel drives stays fixed to the bone it hangs
// from, and a figure that turns sharply turns all of it with her like a statue.
// This gives such bones a mass on a damped spring, after the animation and the IK
// and before the skinning:
//
//   - Each named bone has a point it swings by: the next bone down when it has one
//     (a chain of hair), else the centre of the vertices it moves (a chest, a
//     belly) -- found on the model itself, so nothing has to be placed by hand.
//   - That point is a mass in the world, pulled towards where the animation puts
//     it by a spring of `frequency` Hz and slowed by `damping` (the ratio: 0 swings
//     on for ever, 1 comes back without overshooting). Turn the figure and the mass
//     lags, overshoots and settles; walk and it bounces with every step.
//   - The bone turns about its own origin to point at the mass -- `amount` of the
//     way, never more than `maxAngle` off its pose -- and takes everything below it
//     along. Bones are taken parent first, so a chain swings as a chain.
//
// Simulated in the world, so a figure turned or carried by a script swings as well
// as one the clip turns; a figure that jumps a long way at once (a teleport, a clip
// wrapping round) starts its masses over where they are rather than whipping them
// across the room.
namespace springbones {

// What the Spring Bones component asks for.
struct Settings {
    std::string bones;          // joint names, comma-separated ("lPectoral, rPectoral")
    float frequency = 2.5f;     // Hz: how fast a bone swings
    float damping   = 0.2f;     // damping ratio: 0 swings on, 1 settles at once
    float amount    = 1.0f;     // share of the lag that shows (0 = off, 2 = twice)
    float maxAngle  = 25.0f;    // degrees a bone may swing off its pose
};

class System {
public:
    // Swing the bones `s` names on figure `id`. `palette` is the pose as
    // sampleSkeleton gave it (and the IK bent it), changed in place; `toWorld`
    // takes the model's space to the world's. dt <= 0 holds the masses where
    // they are.
    void apply(int id, const Settings& s, const fitzel::ModelData& model, const glm::mat4& toWorld,
               std::vector<glm::mat4>& palette, float dt);
    // Forget one figure, or all (Play started or stopped).
    void forget(int id) { m_figures.erase(id); }
    void clear() { m_figures.clear(); }

    // How far bone `joint` of figure `id` is turned off its pose now, degrees
    // (-1: not swinging). For the checks.
    float angleOf(int id, int joint) const;

private:
    struct Bone {
        int       joint = -1;
        int       depth = 0;
        glm::vec3 tail{0.0f};      // the swinging point, in the joint's bind space
        glm::vec3 x{0.0f}, v{0.0f};  // the mass in the world, and its velocity
        bool      live  = false;
        float     angle = 0.0f;    // degrees, as last applied
    };
    struct Figure {
        std::string                 bones;
        const fitzel::ModelData*    model = nullptr;
        std::vector<Bone>           list;
    };
    void build(Figure& f, const Settings& s, const fitzel::ModelData& model) const;

    std::unordered_map<int, Figure> m_figures;
};

} // namespace springbones
