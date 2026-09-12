#pragma once

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Shader.hpp>

#include "FrameRender.hpp"
#include "Wind.hpp"

// --- What lives in the landscape -------------------------------------------------
//
// A valley with grass that moves and clouds that drift is still a picture until
// something in it has somewhere to be. This is the fauna, all of it simulated --
// not animated on rails -- because it is the small decisions (a flock turning
// together, a buzzard tilting into its circle, a butterfly settling on a flower
// and taking off again) that read as life:
//
//  - starlings: flocks of a few dozen, boids (separation, alignment, cohesion)
//    chasing a wandering goal over the valley, holding a height band above the
//    ground, flapping to climb and gliding down;
//  - swallows: a handful hunting insects low over the meadow around you, fast
//    and darting, swooping down to the grass and back up;
//  - raptors: two or three soaring in thermals high over the hills, circling
//    for minutes with hardly a wingbeat, banked into the turn, the circle
//    drifting with the wind;
//  - butterflies: around the flowers near you in sunshine, erratic flight from
//    bloom to bloom, landing with their wings slowly opening and closing;
//  - fish: rising in the lake and the river pools, now and then one leaping
//    clear -- a flash of silver, a splash, rings spreading on the water.
//
// Everything is spawned around the eye and quietly moved on when it falls far
// behind, so the valley always has its share without a world-sized population.
// One instanced draw per body type through creature.vert/frag, lit and fogged
// like everything else, in the sun's and the clouds' shadow.
class Wildlife {
public:
    bool enabled     = true;
    int  starlings   = 90;   // across all flocks (30 a flock)
    int  swallows    = 10;
    int  raptors     = 3;
    int  butterflies = 40;

    bool init();

    struct World {
        glm::vec3 eye{0.0f};
        std::function<float(float, float)> ground;   // terrain height
        float waterLevel = -1000.0f;
        float daylight   = 1.0f;       // 0 night .. 1 day
        const wind::State* wind = nullptr;
        // Flower positions (x, y, z per flower) to visit.
        const std::vector<glm::vec3>* flowers = nullptr;
        glm::vec3 forward{0.0f, 0.0f, -1.0f};   // where the eye looks (fish jump there)
        // Is there water at (x, z) for a fish, and where is its surface and
        // how deep is it? The lake, a river's pool -- the host knows which.
        // Unset: the lake at `waterLevel` only.
        std::function<bool(float, float, float&, float&)> water;
    };
    void update(float dt, const World& w);
    void draw(const FrameContext& ctx);
    // Creatures in the last update's draw lists (birds, butterflies).
    int birdsDrawn() const { return static_cast<int>(m_birdData.size() / 13); }
    int fliesDrawn() const { return static_cast<int>(m_flyData.size() / 13); }
    bool ready() const { return m_shader.isValid(); }
    float debugSpeed() const { return m_birds.empty() ? 0.0f : glm::length(m_birds[0].vel); }
    float debugGoal() const {
        return (m_birds.empty() || m_flocks.empty()) ? 0.0f
             : glm::distance(m_birds[0].pos, m_flocks[m_birds[0].flock].goal);
    }
    int   debugUpdates = 0;

    // The lake: rings spreading where a fish rose or fell back in (xz, age
    // in seconds, strength) for water.frag, and the splashes of this update
    // for the host's spray pool -- taken, and so emptied, once a frame.
    static constexpr int kMaxRipples = 6;
    const std::vector<glm::vec4>& ripples() const { return m_ripples; }
    // fishrings.glsl's uniforms (none when `on` is false).
    void applyRipples(const fitzel::Shader& s, bool on) const;
    struct Splash { glm::vec3 pos; float strength; };
    std::vector<Splash> takeSplashes() { std::vector<Splash> s; s.swap(m_splashes); return s; }
    int fishJumps() const { return m_fishJumps; }
    glm::vec3 lastRipple() const { return m_lastRipple; }
    float fishRate = 1.0f;             // jumps per usual interval (shots turn it up)
    // A fish in the air right now (for a shot to look at), or false.
    bool fishInAir(glm::vec3& at) const {
        if (!m_fish.flying) return false;
        at = m_fish.pos;
        return true;
    }
    // Instance `i`'s position as last uploaded (debugging).
    glm::vec3 debugPos(int i) const {
        return (i * 13 + 2 < static_cast<int>(m_birdData.size()))
                   ? glm::vec3(m_birdData[i * 13], m_birdData[i * 13 + 1], m_birdData[i * 13 + 2])
                   : glm::vec3(0.0f);
    }

private:
    struct Bird {
        glm::vec3 pos{0.0f}, vel{0.0f};
        float phase = 0.0f, flap = 0.0f, bank = 0.0f;
        float scale = 1.0f;
        int   flock = 0;
    };
    struct Flock {
        glm::vec3 goal{0.0f};
        float goalTimer = 0.0f;
        float height = 40.0f;          // metres above ground it likes
    };
    struct Raptor {
        glm::vec2 centre{0.0f};
        float radius = 50.0f, angle = 0.0f, height = 150.0f, dir = 1.0f;
        float phase = 0.0f, flapTimer = 10.0f, flapLeft = 0.0f;
        glm::vec3 pos{0.0f}, vel{0.0f};
    };
    struct Fly {                       // butterfly
        glm::vec3 pos{0.0f}, vel{0.0f}, target{0.0f};
        float phase = 0.0f, rest = 0.0f, wander = 0.0f;
        float heading = 0.0f;
        glm::vec3 colour{1.0f};
        bool landed = false;
        bool toFlower = false;         // the target is a bloom (land there)
    };

    struct Fish {                      // the one in the air, if any
        glm::vec3 pos{0.0f}, vel{0.0f};
        float heading = 0.0f, scale = 0.3f, phase = 0.0f;
        bool  flying = false;
        float nextJump = 3.0f, nextRise = 1.5f;
        float surface = 0.0f;          // the water it came out of
    };
    void updateFish(float dt, const World& w);
    // Somewhere on the lake at least `depth` deep, mostly where the eye looks.
    bool findWater(const World& w, float minDist, float maxDist, float depth, glm::vec2& at,
                   float* surface = nullptr);
    void addRipple(glm::vec2 xz, float strength, float y);

    void respawnFlock(int f, const World& w);
    void placeRaptor(Raptor& r, const World& w, bool initial);
    void placeFly(Fly& b, const World& w);
    float groundAt(const World& w, float x, float z) const;

    std::vector<Bird>   m_birds;       // starlings then swallows
    std::vector<Flock>  m_flocks;
    std::vector<Raptor> m_raptors;
    std::vector<Fly>    m_flies;
    Fish                m_fish;
    std::vector<glm::vec4> m_ripples;
    glm::vec3           m_lastRipple{0.0f};
    std::vector<Splash> m_splashes;
    int                 m_fishJumps = 0;
    std::mt19937        m_rng{2718u};
    float uni() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng); }
    float sym() { return uni() * 2.0f - 1.0f; }
    bool  m_spawned = false;
    float m_daylight = 1.0f;

    // GPU: two body meshes (bird, butterfly) and one instance buffer each.
    fitzel::Shader m_shader;
    std::uint32_t m_birdVao = 0, m_birdVbo = 0, m_birdInst = 0;
    std::uint32_t m_flyVao = 0, m_flyVbo = 0, m_flyInst = 0;
    std::uint32_t m_fishVao = 0, m_fishVbo = 0, m_fishInst = 0;
    int m_birdVerts = 0, m_flyVerts = 0, m_fishVerts = 0;
    std::vector<float> m_birdData, m_flyData, m_fishData;
};
