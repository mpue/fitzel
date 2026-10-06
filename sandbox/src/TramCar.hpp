#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "SplineGenDetail.hpp"   // Slot
#include "TramSim.hpp"           // the layout inside a car

// One car of a tram, built from boxes in its own frame (TramSim's "Inside"):
// what is drawn, one Slot per material, and what a figure walks on and
// against, as boxes. Pure geometry -- TramSystem uploads and draws it and makes
// the physics bodies of it; tramcheck walks a figure into it.
namespace tramcar {

enum Mat { Body, Accent, Glass, Roof, Dark, Floor, Seat, Pole, Light, Lining, MatCount };

// A cab car (cab at +Z) or the middle car: the shell, the floor, seats, poles,
// light strips -- everything but the door leaves. `hitC`/`hitH` get the
// collision boxes (centre, half extents).
void build(bool cab, splinegen::detail::Slot slot[MatCount], std::vector<glm::vec3>& hitC,
           std::vector<glm::vec3>& hitH);

// The door leaves: eight to a car, two to each doorway on each side. Leaf `n`
// is doorway n/4, side by bit 1 (+x when set), end by bit 0 (+z when set).
void leafOf(int n, int& doorway, float& sx, float& k);
// Where a leaf's middle is in the car's frame, `open` 0..1 -- out of the wall a
// hand's breadth, then aside over the outer skin.
glm::vec3 leafAt(bool cab, int doorway, float sx, float k, float open);
extern const glm::vec3 kLeafHalf;   // a leaf as a box
// Every leaf of a car, those on `side` (+-1) opened by `open`: pane and frame.
void buildLeaves(bool cab, int side, float open, splinegen::detail::Slot& glass,
                 splinegen::detail::Slot& frame);

} // namespace tramcar
