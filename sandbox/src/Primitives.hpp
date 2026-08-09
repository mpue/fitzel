#pragma once

#include <vector>

#include <fitzel/graphics/Mesh.hpp> // fitzel::Vertex, fitzel::MeshData

// Procedural primitive geometry builders used by the sandbox.

// Flower geometry constants shared with assets/shaders/flower.vert (which keeps
// its own copies -- change both together): the petal ring is built at the max
// count and the shader hides/re-spaces petals per bloom around the stem top.
constexpr int   kFlowerMaxPetals = 8;
constexpr float kFlowerStemTop   = 0.50f;

// A small flower (stem + petals + centre disc) as interleaved floats:
// pos3, normal3, tint1 (0 stem, 1 petal, 2 centre), petal1 (petal index, -1 else).
std::vector<float> makeFlowerMesh();

// A capped cylinder with its axle along local X (for vehicle wheels).
fitzel::MeshData makeCylinderX(float r, float ht, int seg);

// A unit ramp in [-0.5,0.5]^3 rising along +Z. Double-sided.
std::vector<fitzel::Vertex> makeRampVerts();

// The unit cube as CPU data, vertex-for-vertex what fitzel::Mesh::cube() uploads.
// Exists so geometry can be MERGED on the CPU (see CityGen's batching) and come
// out identical to the same box drawn as an entity.
std::vector<fitzel::Vertex> makeCubeVerts();

// A unit cylinder (radius 0.5, y in [-0.5,0.5], axle Y). Double-sided.
std::vector<fitzel::Vertex> makeCylinderYVerts(int seg = 20);

// A UV sphere of radius 0.5 centred at the origin. CCW outward winding.
std::vector<fitzel::Vertex> makeSphereVerts(int stacks = 24, int slices = 32);
