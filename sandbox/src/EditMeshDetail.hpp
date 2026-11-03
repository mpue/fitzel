#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "EditMesh.hpp"

// The handful of helpers every EditMesh operation is made of, shared between
// EditMesh.cpp and EditMeshTools.cpp. Not for callers outside those two: each
// of these leaves the parallel arrays (paint, faceMat, faceUV) in step only as
// part of an operation that finishes the job.
namespace editmesh::detail {

// Is `f` a usable index into a mesh with at least a triangle there?
bool okFace(const EditMesh& m, int f);

// Add a corner and its paint weights in one step.
int addVert(EditMesh& m, const glm::vec3& p, const glm::vec4& w);

// Add a face that wears what `src` wears (material and texture placement). An
// out-of-range `src` gives the object's own material and the default placement.
int addFaceLike(EditMesh& m, std::vector<int> loop, int src);

// Both ends of an edge in one key, order-independent.
std::uint64_t edgeKey(int a, int b);

// Faces meeting at each edge (second is -1 on a border).
using EdgeMap = std::unordered_map<std::uint64_t, std::pair<int, int>>;
EdgeMap buildEdges(const EditMesh& m);

// Drop the corners no face uses any more; old -> new index (-1 = gone).
std::vector<int> dropUnusedVerts(EditMesh& m);
// Seams onto renumbered corners (old -> new, -1 gone); dropUnusedVerts does it.
void remapSeams(EditMesh& m, const std::vector<int>& remap);

// Squeeze repeated corners out of every loop, drop faces left with fewer than
// three, then the unused corners. Returns old -> new FACE index (-1 = gone).
std::vector<int> dropCollapsedFaces(EditMesh& m, std::vector<int>* vertRemap = nullptr);

// A wall an extrusion grew: face `face`, the quad {a, b, cap[b], cap[a]} along
// rim edge a -> b of `from` (now the cap), whose corners `ka` and `kb` are
// cap[a] and cap[b].
struct Wall {
    int face = -1, from = -1, ka = -1, kb = -1;
};
// The walls of an extrusion of `caps`, unwrapped where the cap is: each wall's
// top keeps the cap's coordinates, and it unfolds outward from that edge in UV
// by its height in metres at the edge's own scale -- the texture folds over the
// edge instead of starting again. Walls an earlier extrusion grew under that
// edge (an island hanging off that one edge only) move out by the new height,
// so a stack of extrusions reads as one continuous wall.
void unfoldWalls(EditMesh& m, const std::vector<int>& caps, const std::vector<Wall>& walls);

// Where edge (a, b) sits in face `f`'s loop, or -1.
int edgeIndexIn(const EditMesh& m, int f, int a, int b);

// The unique, in-range corners of a selection, sorted.
std::vector<int> uniqueVerts(const EditMesh& m, const std::vector<int>& idx);

} // namespace editmesh::detail
