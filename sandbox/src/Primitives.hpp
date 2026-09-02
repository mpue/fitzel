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

// A flat unit quad in the XZ plane: x and z in [-0.5,0.5], y = 0, facing +Y,
// UV 0..1 across it. Double-sided, like the ramp and the cylinder -- a floor
// that vanishes when you walk under it is a shape you have to think about, and
// nothing else in this palette asks that.
//
// Subdivided into `cells` x `cells` quads rather than left as two triangles:
// the lit shader shades per fragment but the WATER level, the fog and the
// vertex-interpolated terms still read better with a few vertices across a
// large plane, and a 200 m floor made of two triangles shows it.
std::vector<fitzel::Vertex> makePlaneVerts(int cells = 8);
