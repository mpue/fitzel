#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

// The exhaust of rocket engines as a body of gas: flame, the clouds it throws
// out of a flame trench, the column it leaves in the sky, the vapour that boils
// off a fuelled tank.
//
// What moves is a cloud of PUFFS -- soft spheres that are born small and fast
// where the jet hits something and then slow down, swell and rise. That is the
// whole physics, and it is enough because it is the physics of a real launch
// cloud: a turbulent puff entrains the still air around it, so it grows as it
// travels and loses speed as it grows; a hot one rises; a cold one (vapour off
// liquid oxygen) sinks and spreads along the ground. Nothing here draws. The
// renderer (ExhaustFx) splats the puffs into a density grid and marches it, so
// the cloud that is seen is a continuous volume, not a pile of sprites.
//
// Pure arithmetic and deterministic for a given seed and step sequence, so a
// check can drive it without a window.
namespace exhaust {

struct Puff {
    glm::vec3 pos{0.0f}, vel{0.0f};
    float r     = 1.0f;   // radius, m
    float mass  = 1.0f;   // how much it adds to the density field at full strength
    float heat  = 0.0f;   // 0 = cold steam .. 1 = glowing exhaust, fades fast
    float opacity = 1.0f; // how thick its stuff is: 1 exhaust, a tenth for boil-off vapour
    // 0: a billow of its own, its dome standing out of the heap (the cauliflower
    // of a launch cloud); 1: melts into its neighbours (a trail's smooth column).
    float merge = 0.0f;
    float buoy  = 0.0f;   // upward acceleration, m/s^2 (negative sinks)
    float grow  = 0.15f;  // radius gained per metre travelled through the air
    float swell = 0.6f;   // radius gained per second on its own (turbulence)
    float age   = 0.0f, life = 60.0f;
    std::uint32_t seed = 0;
    float fade() const;   // 0..1: eased in at birth, out at the end of its life
};

// One engine burning this frame, as RocketEngineComponent describes it in world
// space. `dir` is where the exhaust goes (out of the nozzle), unit length.
struct Engine {
    int       id = 0;
    glm::vec3 nozzle{0.0f};
    glm::vec3 dir{0.0f, -1.0f, 0.0f};
    glm::vec3 vel{0.0f};          // the vehicle's own velocity, m/s
    float     radius   = 1.0f;    // nozzle exit radius, m
    float     thrust   = 0.0f;    // 0..1
    float     length   = 30.0f;   // visible flame at full thrust, m
    float     spread   = 0.14f;   // half-angle of the plume, radians
    float     reach    = 4.0f;    // the jet still blasts the ground this many flame lengths out
    float     smoke    = 1.0f;    // how much cloud it makes
    float     heat     = 1.0f;    // how hot its fresh cloud glows
};

// A vent: puffs out of a point, no flame. Liquid-oxygen boil-off (cold, sinks),
// the deluge's steam, a burst of fire. Emits along `dir`.
struct Vent {
    int       id = 0;
    glm::vec3 pos{0.0f}, dir{0.0f, 1.0f, 0.0f};
    float     rate = 6.0f, speed = 3.0f, spread = 0.35f;  // puffs/s, m/s, radians
    float     size = 1.5f, growth = 0.4f, buoyancy = -0.5f;
    float     heat = 0.0f, life = 8.0f, density = 0.6f;
};

// A launch deck with a flame trench under it (FlameTrenchComponent). The box is
// the deck: its top face is where exhaust that misses the hole spreads out, the
// hole in its middle swallows the jet, and the trench below throws it out at
// both ends of the box's local X axis (or the +X end only).
struct Trench {
    glm::mat4 model{1.0f};   // unit cube -> the deck's box
    float hole      = 8.0f;  // radius of the opening, m
    float reach     = 60.0f; // centre -> each exit along local X, m
    float drop      = 8.0f;  // the exits sit this far below the deck top
    bool  twoSided  = true;
    float exitSpeed = 70.0f; // m/s the cloud leaves the trench at
    float steam     = 1.0f;  // the deluge: extra white steam per unit of exhaust
};

// What an engine's flame is this frame -- where it starts, where it ends and how
// wide it is -- worked out once here so the cloud the sim emits and the flame
// the renderer draws agree about where the jet stops.
struct FlameShape {
    glm::vec3 a{0.0f}, dir{0.0f, -1.0f, 0.0f};
    float     len = 0.0f;       // visible length after clipping, m
    float     r0 = 1.0f, r1 = 2.0f;
    float     hitT = -1.0f;     // distance along dir to what the jet hits (-1: nothing in reach)
    int       hitKind = 0;      // 0 nothing, 1 ground, 2 deck, 3 trench hole
    int       trench = -1;
};

struct World {
    std::function<float(float, float)> groundAt;   // terrain height; null = 0
    glm::vec3 wind{2.0f, 0.0f, 0.5f};
};

class Sim {
public:
    // Every puff the sim may hold. Past it the oldest give way first.
    static constexpr int kMaxPuffs = 24000;

    void clear();
    void step(float dt, const std::vector<Engine>& engines, const std::vector<Vent>& vents,
              const std::vector<Trench>& trenches, const World& world);

    const std::vector<Puff>& puffs() const { return m_puffs; }
    float time() const { return m_time; }

    // The flame of `e` against the ground and the decks. Static so the renderer
    // can ask the same question about the same frame.
    static FlameShape flameOf(const Engine& e, const std::vector<Trench>& trenches,
                              const World& world);

private:
    float     rnd();                    // 0..1
    glm::vec3 rndUnit();                // on the unit sphere
    glm::vec2 rndDisc();                // in the unit disc
    float     groundAt(const World& w, float x, float z) const;
    // The highest deck under (x, z), or -inf: puffs land on a deck as on ground.
    float     floorAt(const World& w, const std::vector<Trench>& trenches, float x, float z,
                      int skip = -1) const;
    void      emitEngine(float dt, const Engine& e, const std::vector<Trench>& trenches,
                         const World& world);
    void      emitVent(float dt, const Vent& v);
    void      add(const Puff& p);

    std::vector<Puff> m_puffs;
    std::unordered_map<int, float> m_owed;   // fractional puffs owed per emitter
    // Where each engine's flame tip was last step, so the trail is laid along
    // the whole path the tip swept rather than at one point per frame.
    std::unordered_map<int, glm::vec3> m_lastTip;
    std::unordered_map<int, bool>      m_haveTip;
    std::uint32_t m_rng   = 0x9E3779B9u;
    std::uint32_t m_seeds = 1;
    float m_time = 0.0f;
};

} // namespace exhaust
