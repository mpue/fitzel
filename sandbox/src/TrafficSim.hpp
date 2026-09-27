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
    Kind  kind   = Kind::Car;
    int   lane   = 0;       // the lane it is on -- or leaving, while turning
    int   next   = -1;      // the lane it takes at the end of this one
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
};

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
    void build(const std::vector<cityplan::Rule>& rules,
               const std::vector<const cityplan::Town*>& towns, std::uint32_t seed = 1,
               const std::function<void(Vehicle&)>& dress = {});
    void clear();

    // A scene object joining the traffic (TrafficDriverComponent): put on the
    // lane nearest `pos` that runs roughly along `heading`, driven by the same
    // rules as everyone else. False when no lane is near enough (60 m).
    bool addDriver(int entity, glm::vec2 pos, glm::vec2 heading, Kind kind, float length,
                   float vmax);
    void removeDrivers();

    // Advance by `dt` seconds; `clock` is the signals' time (civic::signalPhase).
    void step(float dt, double clock);

    Pose pose(const Vehicle& v) const;
    Pose pose(const Walker& w) const;

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
    int  pickNext(Vehicle& v) const;
    void addTown(const cityplan::Rule& r, const cityplan::Town& t, int town,
                 const std::function<void(Vehicle&)>& dress);

    std::vector<Node>    m_nodes;
    std::vector<Lane>    m_lanes;
    std::vector<Vehicle> m_vehicles;
    std::vector<Walker>  m_walkers;
    std::vector<std::vector<glm::vec3>> m_walks;
    std::vector<std::vector<float>>     m_walkLen;   // cumulative length per walk
};

// The vehicle kinds' dimensions and top speeds.
float kindLength(Kind k);
float kindSpeed(Kind k);

} // namespace traffic
