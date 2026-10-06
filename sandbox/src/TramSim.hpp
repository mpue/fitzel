#pragma once

#include <vector>

#include <glm/glm.hpp>

// The trams that run on the scene's tram lines -- the tracks laid with a Tram
// track spline (SplineGen, TramGen.cpp). Pure simulation: lines in, the cars'
// poses out, no GPU and no scene; TramSystem draws them and hands the traffic
// their boxes, tramcheck drives this on its own.
//
// A line is ridden as HALVES. On an open line with two tracks, half 0 is the
// right-hand track out and half 1 the other one back; the two meet on one
// track at each end (TramTrack's stub), and a tram that has stopped there with
// its whole length in the stub changes ends: it leaves on the other half from
// where its tail stood, without moving sideways. A one-track open line is the
// same track both ways and runs one tram. A loop is one half per track, run
// round and round, the second one the other way.
//
// Driving: up to the line's top speed, slower through curves (a comfortable
// sideways pull), stopping with its front at each stop sign for the dwell time,
// keeping its distance to the tram ahead and braking for whatever stands on
// the track in front of it -- cars, people, a tram of another line.
namespace tramsim {

constexpr int   kSections   = 3;      // cars of one tram, articulated
constexpr float kSectionLen = 9.6f;   // metres each
constexpr float kGap        = 0.6f;   // between two cars (the bellows)
constexpr float kLength     = kSections * kSectionLen + (kSections - 1) * kGap;
constexpr float kHalfWidth  = 1.2f;   // half of 2.4 m

// --- Inside ----------------------------------------------------------------------
// A car in its own frame (carFrame): x across, y up from the rail head, z along
// it. The cab cars -- the first and the last -- carry the driver's cab at +Z;
// the middle car has a gangway at both ends. TramSystem builds the car (what is
// drawn and what is walked on) and TramRiders seats nobody, but stands them:
// all three read where things are from here.
constexpr float kFloor    = 0.35f;   // the low floor, above the rail head
constexpr float kCeiling  = 2.62f;
constexpr float kWallIn   = 1.12f;   // the side walls' inner face, |x|
constexpr float kDoorHalf = 0.65f;   // half a doorway's clear width
constexpr float kDoorTop  = 2.30f;
constexpr float kCabWall  = 3.45f;   // the cab's back wall (cab cars), z
constexpr float kDoorTime = 1.5f;    // seconds the doors take to open or to close
// The doorways of a car, as z: two to a car, on both sides.
inline void doorsOf(bool cab, float z[2]) {
    z[0] = -2.4f;
    z[1] = cab ? 1.6f : 2.4f;
}
inline bool isCab(int car) { return car == 0 || car == kSections - 1; }

struct Line {
    std::vector<glm::vec3> center;   // the draped centreline (SplineSystem::line)
    std::vector<char>      onRoad;   // parallel; empty = nowhere in a road
    bool  closed = false;
    int   tracks = 1;
    float spacing = 3.2f;            // between the two tracks
    float railTopRoad = 0.032f;      // rail head above the line, in a road...
    float railTopOff  = 0.20f;       // ...and on its own track
    int   trams = 0;
    float vmax = 11.1f;              // m/s
    float stopEvery = 350.0f;
    float dwell = 15.0f;             // seconds
};

// Something a tram must not run into: a point and how far around it reaches.
struct Blocker {
    glm::vec3 p{0.0f};
    float     r = 0.5f;
};

// One car of a tram: the middle of its floor at rail-head height, which way it
// faces (the way the tram drives), and that as yaw (about +Y, 0 = +Z) and pitch
// (nose up positive).
struct Section {
    glm::vec3 center{0.0f};
    glm::vec3 dir{0.0f, 0.0f, 1.0f};
    float     yaw = 0.0f;
    float     pitch = 0.0f;
};

// A car as a box for the street traffic to brake for (traffic::Obstacle's shape).
struct Box {
    glm::vec3 center{0.0f};
    glm::vec3 axes[3]{};
    glm::vec3 vel{0.0f};
    // Not a car of the tram but the track ahead of it: its right of way
    // (traffic::Obstacle::giveWay).
    bool      giveWay = false;
};

class Sim {
public:
    // Start over on these lines (every tram placed afresh).
    void setLines(const std::vector<Line>& lines);
    // Drive every tram on by `dt` seconds.
    void step(float dt, const std::vector<Blocker>& blockers);

    int  tramCount() const { return static_cast<int>(m_trams.size()); }
    // Counts every setLines: tram indices from before one are void.
    int  generation() const { return m_generation; }
    // The cars of tram `i`, front first.
    void sections(int i, Section out[kSections]) const;
    float speed(int i) const;
    // Car `car` of tram `i` as a frame (see "Inside"): which car is which stays
    // the same when the tram changes ends -- the car that was last is first
    // then, and its frame does not jump -- so whatever is in a car (the bodies
    // a figure walks on, the people riding) can be kept in it by this.
    glm::mat4 carFrame(int i, int car) const;
    // Which side of the car its doors open on: +1 = its +x side, -1 = its -x.
    // Always the right of the way the tram drives, where the stop is.
    int doorSide(int i, int car) const;
    // How far its doors are open, 0..1. They open once it has stopped at a
    // stop and close for the last kDoorTime of its time there.
    float doors(int i) const;
    // Someone is in a doorway or on their way through one: the doors stay
    // open (and the tram at its stop) a little longer. No-op unless it is
    // standing at a stop.
    void holdDoors(int i);
    // Every car of every tram -- and, as giveWay boxes, the track each is about
    // to run over: a few seconds' worth at its speed, at least a car's length.
    std::vector<Box> boxes() const;

    // --- For checks ----------------------------------------------------------
    struct Info {
        int   line = 0, half = 0;
        float x = 0.0f, v = 0.0f, dwell = 0.0f;
        int   served = 0, flips = 0;
        int   next = 0;      // the stop it heads for -- or stands at, while dwell > 0
        float door = 0.0f;   // doors(i)
    };
    Info      info(int i) const;
    int       lineCount() const { return static_cast<int>(m_lines.size()); }
    // An open line turns its trams at both ends: their last stop on a half is
    // the terminus, where everyone gets out.
    bool      lineOpen(int line) const;
    int       halfCount(int line) const;
    float     halfLength(int line, int half) const;
    glm::vec3 pointOn(int line, int half, float x) const;
    const std::vector<float>& stopsOn(int line, int half) const;

private:
    struct Half {
        std::vector<glm::vec3> pts;     // rail-head level, in the order it is driven
        std::vector<float>     st;      // metres along it (plan)
        std::vector<float>     vcurve;  // the speed a curve allows, per sample
        std::vector<float>     stops;   // where a front stops, ascending
        bool  loop = false;
        float len  = 0.0f;
        glm::vec3 at(float x) const;
        float     wrap(float x) const;
        float     project(const glm::vec3& p) const;
    };
    struct LineRt {
        Line              def;
        std::vector<Half> halves;
        bool              open = true;
    };
    struct Tram {
        int   line = 0, half = 0;
        float x = 0.0f;       // its FRONT, metres along its half
        float v = 0.0f;
        float dwell = 0.0f;   // seconds left at a stop
        float hold = 0.0f;    // seconds left before leaving, after changing ends
        int   next = 0;       // the stop it is heading for (index into stops)
        int   served = 0, flips = 0;
        float door = 0.0f;    // 0 shut .. 1 open
    };

    static bool backwards(const Tram& t, int car);
    void finishStop(Tram& t);
    void changeEnds(Tram& t);
    int  firstStopAfter(const Half& h, float x) const;

    std::vector<LineRt> m_lines;
    std::vector<Tram>   m_trams;
    int                 m_generation = 0;
};

} // namespace tramsim
