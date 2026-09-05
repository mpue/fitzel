#pragma once

#include <vector>

#include "WeatherPreset.hpp"

// The sky as a stack of layers.
//
// It used to be two things nailed in place: a marched cumulus slab, and one
// plane above it that drew cirrus and contrails together out of the same
// sample. That arrangement can say two sentences about a sky and no more. It
// cannot put a stratus deck UNDER the cumulus, it cannot have contrails at
// their own height on their own heading, it has no altocumulus at all, and
// because the ice and the traffic shared a plane, moving one moved the other.
//
// So the sky is an ORDER now. Every cloud type is a layer with its own height,
// its own coverage, its own wind and its own direction; the cumulus is one
// entry in that order rather than a step after it; and compositing is a walk
// down the list from the top of the atmosphere to the bottom. A deck draws over
// what is above it and under what is below it, which is the one rule the old
// fixed arrangement could not express and the reason all of this exists.
//
// Exactly ONE layer is marched. That is a budget decision, not a physical one:
// the cumulus raymarch is 64 steps with a five-tap light march at each, easily
// the most expensive thing in the frame, and a second one would double it to
// draw a deck whose whole visual character is that it has no depth. Every other
// type is one plane intersection -- which is also what they physically are.
//
// This module owns the ordering and the upload; the shader owns what each kind
// LOOKS like. Both callers of it -- the editor and the skycheck harness -- go
// through order() so a picture from the harness is a picture of what ships.
namespace skylayers {

// The kind numbers the shader switches on. They are a wire format between this
// file and sky.frag: change one and change the other.
enum Kind {
    kCumulus       = 0,   // the marched slab -- not a sheet
    kStratus       = 1,
    kStratocumulus = 2,
    kAltocumulus   = 3,
    kCirrus        = 4,
    kCirrocumulus  = 5,
    kContrails     = 6,
};

// The shader's array size. More slots than there are cloud types, because the
// cumulus takes one of them.
constexpr int kMaxLayers = 8;

// One layer, packed the way the shader wants it.
struct Packed {
    int   kind   = kCumulus;
    float amount = 0.0f;
    float height = 0.0f;
    float scale  = 1.0f;
    float wind   = 0.0f;
    float dirX   = 1.0f;   // unit heading in XZ
    float dirZ   = 0.0f;
};

// Every enabled layer, HIGHEST FIRST, with the cumulus dropped in at its own
// altitude. Highest first because that is painter's order for something you are
// standing under: the top of the atmosphere is the furthest away.
//
// The cumulus arrives separately rather than out of `sky` because by the time
// it reaches the shader the storm dial has already had its way with it -- a
// front lowers the base and thickens the deck, and it is the ADJUSTED numbers
// that decide where the slab sits in the order. `cumulusTop` is what it sorts
// on: a sheet above the tops draws behind the deck, one below them in front.
std::vector<Packed> order(const weather::Sky& sky, float cumulusBase,
                          float cumulusTop);

// The Sky panel's cloud-layer sections: one collapsing header per type, each
// with its own controls. Draws into the current ImGui window.
void drawPanel(weather::Sky& sky);

} // namespace skylayers
