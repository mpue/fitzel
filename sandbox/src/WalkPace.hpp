#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Model.hpp>

// How fast a walk clip walks. A foot (or a hoof) on the ground moves backwards
// under the body at exactly the speed the body has to move forwards, or it
// slides -- so a clip played in step with the ground has to be played at that
// speed. Shared by the herd (Herd.cpp) and the towns' people (TownTraffic.cpp).
namespace walkpace {

// Find the feet (the vertices that come lowest in the cycle, one per corner of
// the body), follow each through `clip`, and average its backward speed along
// `fwd` (model space) over the samples where it is down. Model units per
// second; negative = the model walks the other way round than it was told; 0 =
// no contact found (or the clip carries its own root motion).
float stanceSpeed(const fitzel::ModelData& m, int clip, glm::vec3 fwd);

// How many times the walk repeats within [start, end) of `clip`. A mocap walk
// often holds several stride pairs (f_casual_walk1.glb: three in 3.8 s), and
// whoever pre-skins its poses wants them all in ONE of them -- spread over the
// whole clip, a person steps through a handful of poses per stride and the walk
// looks like dropped frames. Compares the whole skeleton's pose a candidate
// period apart; 1 when nothing shorter repeats.
int cycles(const fitzel::ModelData& m, int clip, float start, float end);

// A de-indexed primitive (as loadGltf gives them: every triangle its own three
// vertices) with the vertices it repeats merged, and the triangle list over
// them in `indices`. Same skin, about a fifth of the vertices to skin and
// upload. Only the geometry and the skin come along, not the maps.
fitzel::ModelPrimitive weld(const fitzel::ModelPrimitive& p, std::vector<std::uint32_t>& indices);

} // namespace walkpace
