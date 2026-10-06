#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "SplineGen.hpp"

// The geometry helpers SplineGen.cpp builds fences, walls and track from, shared
// with BridgeGen.cpp. Not for callers outside those two: everything here works
// in the generator's own conventions (upright frames, CCW profiles, world-space
// batches) and assumes a sane()'d style.
namespace splinegen::detail {

// A deterministic 0..1 from an integer (per-piece jitter keyed by WHICH piece).
float unitHash(std::uint32_t h);

// One drawable slot: geometry plus the AABB that grows with it.
struct Slot {
    fitzel::MeshData data;
    glm::vec3        lo{1e30f}, hi{-1e30f};
    bool empty() const { return data.vertices.empty(); }
};

// A box rotated about +Y (`yaw` in radians, 0 = local +Z on world +Z), merged in
// world space, mapped planar at `tile` metres per texture repeat.
void appendBox(Slot& sl, const glm::vec3& center, const glm::vec3& half, float yaw,
               float tile);

// One sample of the path with the axes a cross-section is expressed in.
//
// The frame is deliberately UPRIGHT: the tangent is taken in plan only and `up`
// is always world up. A wall on a hillside stands plumb and a ballast bed stays
// level across the track, which is what both actually do -- rolling the section
// with the gradient would lean every post downhill.
struct Frame {
    glm::vec3 p;        // world, ground + lift
    glm::vec3 t;        // unit tangent in plan (y = 0)
    glm::vec3 r;        // unit up x t -- the traveller's LEFT (see SplineGen.hpp)
    float     station;  // metres along the path (plan length)
    float     yaw;      // radians about +Y, for boxes placed on this frame
};

std::vector<Frame> makeFrames(const std::vector<glm::vec3>& path, bool closed);

// A closed cross-section in (lateral, up) metres relative to the frame origin,
// wound COUNTER-CLOCKWISE in that plane so the outward normal of edge a->b is
// (dy, -dx).
using Profile = std::vector<glm::vec2>;

Profile rectProfile(float halfWidth, float bottom, float top);
// Vignoles rail in section (foot, web, head), standing on y = 0.
Profile railProfile(float width, float height);

// Sweep `prof` along frames [i0, i1] at a lateral offset, one quad strip per
// profile edge plus end caps. `uvTile` is world metres per texture repeat.
void sweep(Slot& sl, const std::vector<Frame>& f, std::size_t i0, std::size_t i1,
           const Profile& prof, float lateral, float uvTile, bool capStart, bool capEnd);

// Where a repeated piece stands, interpolated between frames.
struct Stop {
    glm::vec3 p;
    float     yaw;
    int       index;  // which piece this is along the WHOLE path (jitter seed)
};

// Stations in [f[i0], f[i1]) every `spacing` metres, phased from the path's
// start; decrements `budget` per piece and stops at 0.
std::vector<Stop> stopsIn(const std::vector<Frame>& f, std::size_t i0, std::size_t i1,
                          float spacing, int& budget, float phase = 0.0f);

// Find-or-create one material by name, re-applying its look either way.
fitzel::AssetId ensureMaterial(std::vector<MaterialDef>& mats, const std::string& name,
                               glm::vec3 albedo, float refl, float rough);

// The bridge presets' numbers and colours, filled into a style that already
// carries the shared defaults. Implemented in BridgeGen.cpp, next to the rule.
void bridgePreset(Preset p, Style& s);

// Track that is a tram line (Style::embed, two tracks or an overhead wire) for
// frames [i0, i1]: rails flush in paving where `onRoad` says the path is on a
// road, on sleepers elsewhere, the overhead line and the stop signs. Slots are
// steel, sleeper/groove, ballast/paving, sign. `len` is the whole path's length.
// Implemented in TramGen.cpp.
void tramChunk(Slot slot[4], const std::vector<Frame>& f, std::size_t i0, std::size_t i1,
               const Style& s, const std::vector<char>& onRoad, bool closed, int& budget,
               int& pieces, bool capStart, bool capEnd);

} // namespace splinegen::detail
