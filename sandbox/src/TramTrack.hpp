#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// A tram line's layout along its path, shared by the two things that must agree
// on it to the centimetre: the track drawn in the street (SplineGen, TramGen.cpp)
// and the trams that run on it (TramSim). Neither may work this out on its own --
// a tram a hand's breadth off its rails is the whole of what would go wrong.
//
// Stations are metres along the path's centreline, in plan; `len` is its length.
// "Right" is the right-hand side of someone walking the path from its start.
namespace tramtrack {

// --- In a street ---------------------------------------------------------------
// Laid into a road the rails do not stand on sleepers: the head lies flush with
// a band of paving set into the asphalt, a groove beside it for the flange.
// Heights are above the road's surface (RoadSystem::surfaceHeightAt).
constexpr float kBandTop = 0.020f;   // the paving band: above the asphalt's road decals' 0.015
constexpr float kRailTop = 0.032f;   // the polished head, a hair proud of the band
constexpr float kBandMargin = 0.45f; // paving beyond each rail

// Rail head above the path where the track is NOT in a road (sleepers + rail).
inline float railTopOffRoad(float sleeperHeight, float railHeight) {
    return sleeperHeight + railHeight;
}

// --- Double track ----------------------------------------------------------------
// Two tracks, one per direction, each on its side of the path. At an open end
// they come together on ONE track for the last kStub metres -- a stub terminus,
// where a two-way tram stops, its driver changes ends and it leaves on the other
// track. The stub is longer than a tram, so the turn needs no sideways move at
// all; ahead of it the two tracks ease apart over kTaper.
constexpr float kStub  = 36.0f;
constexpr float kTaper = 28.0f;

// How far the TRAVELLED tracks sit from the centreline at station `s`: track 0
// is on the right (the way the path runs), track 1 on the left. Positive = right.
inline float offset(float s, float len, int tracks, float spacing, bool closed, int track) {
    if (tracks < 2) return 0.0f;
    float half = std::max(spacing, 0.0f) * 0.5f;
    if (!closed) {
        const float e = std::min(s, len - s);   // distance to the nearer end
        float u = (e - kStub) / kTaper;
        u = std::clamp(u, 0.0f, 1.0f);
        half *= u * u * (3.0f - 2.0f * u);
    }
    return track == 0 ? half : -half;
}

// --- Stops -----------------------------------------------------------------------
// Where the line stops, as stations: on an open line both termini (the stub's
// far end, kEndStop short of the buffer) and every `every` metres between, the
// last one dropped if it would crowd the terminus; on a loop every `every`
// metres from the start. 0 = only the termini.
constexpr float kEndStop = 3.0f;
inline std::vector<float> stops(float len, float every, bool closed) {
    std::vector<float> out;
    if (len <= 1.0f) return out;
    if (closed) {
        if (every <= 1.0f) return out;
        for (float s = 0.0f; s < len - every * 0.5f; s += every) out.push_back(s);
        return out;
    }
    out.push_back(kEndStop);
    if (every > 1.0f)
        for (float s = every; s < len - every * 0.5f; s += every) out.push_back(s);
    if (len > 2.0f * kEndStop + 1.0f) out.push_back(len - kEndStop);
    return out;
}

} // namespace tramtrack
