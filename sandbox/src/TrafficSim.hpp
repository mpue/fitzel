#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "CityPlan.hpp"

// The life on a town's streets: vehicles driving its grid and people walking
// round its blocks. Pure simulation -- positions, speeds, who waits for whom --
// with no GPU and no scene; TownTraffic draws what this computes, trafficcheck
// measures it.
//
// Deliberately simple, and simple on purpose in these ways:
//   - Vehicles drive on the right along lanes between the grid's nodes, and at
//     a node pick the next street at random (a U-turn only at a dead end). The
//     turn is a curve through the crossing, not a lane of its own.
//   - They keep their distance with the Intelligent Driver Model: one formula
//     that brakes for the vehicle ahead, for a red light's stop line and for a
//     bus stop the same way, so a queue forms and dissolves by itself.
//   - Traffic lights are the towns' own (civic::signalPhase): green goes,
//     amber stops whoever still can, red and red-amber stop.
//   - Buses call at the stops on their side of the street for a few seconds.
//   - Cross traffic is NOT resolved at crossings without lights: two cars can
//     meet in the middle of a village crossing and drive through each other.
//   - People walk round their block on the middle of the pavement and never
//     cross a street.
//   - A wreck, the player's car or a person on foot who is not one of the town's
//     walkers (setObstacles) blocks the lanes it stands in,
//     and in a crossing the lanes whose straight line runs through it; the
//     curve of a turn is not checked.
namespace traffic {

enum class Kind : std::uint8_t { Car, Bus, Truck, Count };

// One lane: a straight run from the end of one crossing to the start of the
// next, on the right-hand side of its street, with the road's height sampled
// along it.
struct Lane {
    int       from = -1, to = -1;     // node indices
    int       axis = 0;               // signal axis (0 = the town's X streets)
    glm::vec2 p0{0.0f}, p1{0.0f};     // start and end (stop line), world XZ
    glm::vec2 dir{1.0f, 0.0f};
    float     len = 0.0f;
    float     half = 3.5f;            // the street's half-width
    std::vector<float> y;             // surface height every kStep metres
    std::vector<float> stops;         // bus stops along it (metres from p0)
    float heightAt(float s) const;
};

struct Node {
    glm::vec2        pos{0.0f};
    bool             signal = false;
    std::vector<int> out;             // lanes leaving it
};

struct Vehicle {
    std::uint32_t uid = 0;  // who it is, for as long as it lives (its index shifts)
    Kind  kind   = Kind::Car;
    int   lane   = 0;       // the lane it is on -- or leaving, while turning
    int   next   = -1;      // the lane it takes at the end of this one
    int   prev   = -1;      // the lane it turned in from: its rear is still in that turn
    float s      = 0.0f;    // FRONT bumper: metres along the lane, or along the turn
    bool  turning = false;
    float v      = 0.0f;    // m/s
    float dwell  = 0.0f;    // seconds left at a bus stop
    int   servedLane = -1;  // the last stop called at, so it is not called at twice
    float servedS    = 0.0f;
    std::uint32_t rng = 1;
    int   look   = 0;       // paint variant, for the renderer
    float length = 4.3f;
    float vmax   = 13.0f;
    int   town   = 0;       // which town's rule it was spawned from
    int   prefab = -1;      // a vehicle prefab dressing it (TownTraffic), -1 = placeholder
    int   entity = -1;      // a scene object it drives (TrafficDriverComponent), -1 = none
    // For the wheels: metres driven (their spin), and how fast the heading
    // turns (their steering), radians/s, smoothed; positive turns the heading
    // angle atan2(x, z) up.
    float odo     = 0.0f;
    float yawRate = 0.0f;
    float lastH   = 0.0f;
    bool  haveH   = false;
};

struct Walker {
    int   walk  = 0;        // which block's walk
    float s     = 0.0f;     // metres along it
    int   dir   = 1;        // +1 = the way the walk runs, -1 = against it
    float speed = 1.3f;
    float phase = 0.0f;     // stride clock, for the bob
    int   look  = 0;        // coat variant
    int   town  = 0;        // which town's rule it was spawned from
    int   prefab = -1;      // a person prefab dressing it (TownTraffic), -1 = placeholder
    std::uint32_t rng = 1;  // its own dice, for whoever dresses it
    // Led off its walk by someone else (TramRiders: to a tram stop, into a tram,
    // riding it, out again): it stands at `at` facing `face`, and its walk does
    // not move it; its stride clock runs while it `strides`.
    bool      led = false;
    bool      strides = false;
    glm::vec3 at{0.0f};
    glm::vec2 face{1.0f, 0.0f};
};

// Something in the street that is not traffic -- a wreck, the player's car --
// as a box: its centre, and its three axes in world space, each as long as the
// box is half wide along it (so any tilt a tumbled wreck has is covered). The
// traffic brakes for it like for the vehicle ahead, at the speed it moves.
struct Obstacle {
    glm::vec3 center{0.0f};
    glm::vec3 axes[3]{};
    glm::vec2 vel{0.0f};    // XZ, m/s
    // Not a thing but a right of way: the track a tram is about to run over.
    // Nobody drives INTO it -- a car waits at the stop line rather than turn in
    // front of the tram -- but whoever is already in it, or already turning,
    // drives on out of it. Waiting there would be waiting for the tram, which
    // waits for them.
    bool      giveWay = false;
};

// How hard a hit shook a driven body: the change in its velocity that its
// driver did not ask for (`asked`, `askedSpin`), a spin counted at `reach`
// metres from its middle. That is delta-v, the measure crash severity is given
// in -- and it weighs the masses by itself: a car shunting a bus barely moves it.
float jolt(glm::vec3 vel, glm::vec3 asked, glm::vec3 spin, glm::vec3 askedSpin, float reach);

// A vehicle's or a walker's pose for drawing: where it stands (centre on the
// ground), its heading in XZ, and its pitch (radians, nose up positive).
struct Pose {
    glm::vec3 pos{0.0f};
    glm::vec2 heading{1.0f, 0.0f};
    float     pitch = 0.0f;
    float     bob   = 0.0f;
};

class Sim {
public:
    constexpr static float kStep = 2.0f;   // height samples along a lane

    // The road surface under a point (false = no road there). The streets are
    // only drivable where a road was laid -- a street the water broke has a gap.
    std::function<bool(glm::vec2, float&)> surfaceAt;

    // Build the network and populate it for every town (rules parallel to the
    // derived towns). `seed` picks who is where; the same inputs give the same
    // start.
    // `dress` sees every vehicle as it is spawned -- its kind and town known,
    // not yet placed -- and may set its length and prefab (TownTraffic's
    // vehicle prefabs), so it is spaced by the size it will really have.
    // `dressWalker` does the same for every person (TownTraffic's person
    // prefabs); it changes nothing about how they walk.
    void build(const std::vector<cityplan::Rule>& rules,
               const std::vector<const cityplan::Town*>& towns, std::uint32_t seed = 1,
               const std::function<void(Vehicle&)>& dress = {},
               const std::function<void(Walker&)>& dressWalker = {});
    void clear();

    // A scene object joining the traffic (TrafficDriverComponent): put on the
    // lane nearest `pos` that runs roughly along `heading`, driven by the same
    // rules as everyone else. False when no lane is near enough (60 m).
    bool addDriver(int entity, glm::vec2 pos, glm::vec2 heading, Kind kind, float length,
                   float vmax);
    void removeDriver(int entity);   // it crashed: out of the traffic, a wreck now
    void removeDrivers();
    // A town vehicle crashed: out of the traffic. False if it is not there.
    bool removeVehicle(std::uint32_t uid);

    // What stands in the street this frame (wrecks, the player's car). Every
    // lane it reaches into is blocked from its near end to its far end: who
    // comes up behind stops short of it, or follows it while it moves on. Kept
    // until the next call; cleared by a rebuild.
    void setObstacles(const std::vector<Obstacle>& obstacles);

    // Advance by `dt` seconds; `clock` is the signals' time (civic::signalPhase).
    void step(float dt, double clock);

    Pose pose(const Vehicle& v) const;
    Pose pose(const Walker& w) const;

    // A person led off the walks (Walker::led): where they stand, which way
    // they look, and whether they are walking (the stride clock runs).
    void lead(int walker, glm::vec3 at, glm::vec2 face, bool striding);
    // Where a person at `p` looking along `face` joins the walks: the nearest
    // point of the nearest walk, walked the way they look -- and `at`, where
    // they then stand (walkers keep to the right of a walk's line). False when
    // there is no walk.
    bool joinWalk(glm::vec3 p, glm::vec2 face, int& walk, float& s, int& dir,
                  glm::vec3& at) const;
    // ...and back on it, from there: no longer led.
    void release(int walker, int walk, float s, int dir);
    // Counts every clear and build: walker (and vehicle) indices from before
    // one are void.
    int generation() const { return m_generation; }

    const std::vector<Lane>&    lanes() const { return m_lanes; }
    const std::vector<Node>&    nodes() const { return m_nodes; }
    const std::vector<Vehicle>& vehicles() const { return m_vehicles; }
    const std::vector<Walker>&  walkers() const { return m_walkers; }
    const std::vector<std::vector<glm::vec3>>& walks() const { return m_walks; }

    // The turn from lane a into lane b: its length, and the point/tangent at
    // `s` metres into it.
    float turnLength(int a, int b) const;
    void  turnPoint(int a, int b, float s, glm::vec2& p, glm::vec2& t) const;

    // Whether a vehicle on lane `l`, `dist` metres short of its stop line at
    // speed `v`, has to stop for the signal at time `clock`.
    bool mustStop(int l, float dist, float v, double clock) const;

private:
    // A point of a vehicle's path: where, which way, and the road's height.
    // The path is the lane it turned in from, that turn, its lane, the turn it
    // is taking and the lane it turns into, joined end to end -- so a point
    // sliding along it never jumps, whichever of them it is on.
    struct PathPoint { glm::vec2 p{0.0f}, t{1.0f, 0.0f}; float y = 0.0f; };
    PathPoint at(const Vehicle& v, float d) const;             // d as its front bumper's s
    PathPoint onLane(int lane, int prev, float d) const;       // d from the lane's start
    PathPoint inTurn(int a, int b, float d) const;             // d along the turn

    float turnSpeed(int a, int b, float vmax) const;   // how fast the turn a -> b is taken

    int  pickNext(Vehicle& v) const;
    void addTown(const cityplan::Rule& r, const cityplan::Town& t, int town,
                 const std::function<void(Vehicle&)>& dress,
                 const std::function<void(Walker&)>& dressWalker);

    // An obstacle's stretch of one lane (metres from its start) and its speed along it.
    struct Block { int lane = 0; float back = 0.0f, front = 0.0f, v = 0.0f; bool giveWay = false; };

    std::vector<Node>    m_nodes;
    std::vector<Lane>    m_lanes;
    std::vector<Vehicle> m_vehicles;
    std::vector<Block>   m_blocks;
    std::uint32_t        m_nextUid = 1;   // never reset: a rebuild must not hand out an old one
    int                  m_generation = 0;
    std::vector<Walker>  m_walkers;
    std::vector<std::vector<glm::vec3>> m_walks;
    std::vector<std::vector<float>>     m_walkLen;   // cumulative length per walk
};

// The vehicle kinds' dimensions and top speeds.
float kindLength(Kind k);
float kindSpeed(Kind k);

} // namespace traffic
