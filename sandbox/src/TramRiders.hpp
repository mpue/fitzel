#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace tramsim { class Sim; }
namespace traffic { class Sim; }

// The town's people taking the trams. Pure simulation, like the two it joins:
// TramSim drives the trams, traffic::Sim walks the people round their blocks,
// and this leads some of those people off their walks (traffic::Walker::led)
// -- to a stop, into a tram that stands there with its doors open, on through
// a few stops standing in a car, and out again at one with a pavement near,
// back onto the walks. Who rides is drawn wherever TownTraffic draws them
// anyway, so the people in a tram are the town's own, in its own clothes.
//
// The trams wait for them: whoever is on their way through a door holds it
// open (tramsim::Sim::holdDoors). A stop with no pavement within reach has
// nobody waiting at it, and nobody gets out there.
class TramRiders {
public:
    // Once a frame, after the trams and the people have moved.
    void step(float dt, tramsim::Sim& trams, traffic::Sim& people);
    // Everybody back on the walks, at once (the trams start over).
    void clear(traffic::Sim& people);

    // Those of them in the street this frame -- crossing between the pavement
    // and a tram -- for the traffic to stop for. Feet.
    const std::vector<glm::vec3>& inStreet() const { return m_inStreet; }

    // --- For checks ------------------------------------------------------------
    int waiting() const;              // at a stop, or on their way to one
    int riding() const;               // in a tram, standing
    int ridingOn(int tram) const;
    int boarded() const  { return m_boarded; }    // so far, all told
    int alighted() const { return m_alighted; }
    int stopsServed() const;          // stops with a pavement within reach
    // Where one rider is, and the frame of the car they ride in.
    struct Seen { int walker = -1, tram = -1, car = -1; glm::vec3 local{0.0f}; };
    std::vector<Seen> riders() const;

private:
    struct Pt { glm::vec3 p{0.0f}; bool inCar = false; };
    enum class St { ToStop, Waiting, Boarding, Riding, Alighting, Leaving };
    struct Person {
        int   walker = -1;
        St    st = St::ToStop;
        int   stop = -1;              // where they wait
        int   tram = -1, car = -1, place = -1;
        glm::vec3 spot{0.0f};         // their place on the platform
        glm::vec3 pos{0.0f};          // where they are (world)
        glm::vec2 face{1.0f, 0.0f};
        std::vector<Pt> path;         // still to walk, in order; inCar = in the car's frame
        int   ridesLeft = 1;
        float waited = 0.0f;
        float speed = 1.3f;
        int   joinWalk = -1, joinDir = 1;   // where Alighting/Leaving end
        float joinS = 0.0f;
    };
    struct Stop {
        int   line = 0, half = 0, index = 0;
        float x = 0.0f;               // metres along its half
        glm::vec3 front{0.0f};        // where a tram's front stops (rail head)
        glm::vec3 dir{0.0f, 0.0f, 1.0f}, right{1.0f, 0.0f, 0.0f};
        glm::vec3 entry{0.0f};        // the nearest point of the walks
        bool  served = false;         // ...near enough
        bool  terminus = false;
    };
    struct Arrival { int line = -1, half = -1, next = -1; bool on = false; };

    void rebuild(const tramsim::Sim& trams, const traffic::Sim& people);
    int  stopOf(const tramsim::Sim& trams, int tram) const;   // the stop it stands at, -1
    bool walk(Person& p, float dt, const tramsim::Sim& trams);
    void board(Person& p, int tram, const tramsim::Sim& trams);
    void alight(Person& p, const tramsim::Sim& trams, const traffic::Sim& people);
    void leave(Person& p, const traffic::Sim& people);
    glm::vec3 world(const Pt& q, const Person& p, const tramsim::Sim& trams) const;
    float rand01();

    std::vector<Person>  m_people;
    std::vector<Stop>    m_stops;
    std::vector<std::vector<int>> m_places;   // per tram: car*kPlaces + place -> person+1
    std::vector<Arrival> m_arrived;           // per tram: the stop it was last seen at
    std::vector<float>   m_lastAsked;         // per walker: when they last passed a stop
    std::vector<char>    m_busy;              // per walker: led by us
    std::vector<glm::vec3> m_inStreet;
    int   m_tramGen = -1, m_peopleGen = -1;
    float m_clock = 0.0f, m_nextLook = 0.0f;
    int   m_boarded = 0, m_alighted = 0;
    std::uint32_t m_rng = 0x9E3779B9u;
};
