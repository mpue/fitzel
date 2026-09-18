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

// Squeeze repeated corners out of every loop, drop faces left with fewer than
// three, then the unused corners. Returns old -> new FACE index (-1 = gone).
std::vector<int> dropCollapsedFaces(EditMesh& m, std::vector<int>* vertRemap = nullptr);

// Where edge (a, b) sits in face `f`'s loop, or -1.
int edgeIndexIn(const EditMesh& m, int f, int a, int b);

// The unique, in-range corners of a selection, sorted.
std::vector<int> uniqueVerts(const EditMesh& m, const std::vector<int>& idx);

} // namespace editmesh::detail
