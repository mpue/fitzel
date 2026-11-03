#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "ProcGraph.hpp"

// What a building's inside is to the procedural graphs: rooms as closed lines
// on the floor, each tagged with its kind, beside the lines for what a room has
// to keep clear of (see Curve::tag). Two nodes make them -- Floor plan (a
// walkable house from a room programme) and Rooms (a Facade block made hollow)
// -- and Furnish reads them, so it furnishes a hand-drawn Rectangle just as well.
//
// The nodes themselves are in ProcNodesInterior.cpp, like every kind only known
// by its registrar. This is the one thing the outside wants to ask of them:
// what Furnish would place and where, without looking at faces.
namespace proc {

// The room kinds a tag can name, in the order of Furnish's menu.
struct RoomKind { const char* id; const char* label; };
const std::vector<RoomKind>& roomKinds();

// One furnished group in the graph's space: its footprint on the floor (a box,
// turned), how tall it stands, and the floor kept free in front of it.
struct FurnishedPiece {
    int         room = 0;
    std::string kind;          // the room's
    std::string what;          // "bed", "wardrobe"
    glm::vec2   center{0.0f};  // x, z
    glm::vec2   half{0.0f};    // along its width, along its depth
    float       turn   = 0.0f; // degrees: its front is (sin, cos)
    float       floor  = 0.0f; // y it stands on
    float       height = 0.0f;
    float       clear  = 0.0f; // depth of free floor in front
};

// Every room in `in`, furnished the way a Furnish node with these settings does
// it: the pieces it places and, when `made` is given, their faces added there.
struct FurnishSettings {
    int   seed        = 1;
    int   defaultKind = 0;     // for a closed line without a tag
    float ceiling     = 2.5f;  // for a room that does not say
    float share       = 1.0f;  // how many of the rooms get furnished at all
    bool  small       = true;  // books and the like
};
std::vector<FurnishedPiece> furnishPlan(const Geo& in, const FurnishSettings& s);

} // namespace proc
