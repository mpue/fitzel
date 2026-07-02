#pragma once

#include <vector>

#include <fitzel/graphics/Mesh.hpp> // fitzel::Vertex, fitzel::MeshData

// Procedural primitive geometry builders used by the sandbox.

// A small flower (stem + petals + centre disc) as interleaved floats:
// pos3, normal3, tint1 (0 stem, 1 petal, 2 centre).
std::vector<float> makeFlowerMesh();

// A capped cylinder with its axle along local X (for vehicle wheels).
fitzel::MeshData makeCylinderX(float r, float ht, int seg);

// A unit ramp in [-0.5,0.5]^3 rising along +Z. Double-sided.
std::vector<fitzel::Vertex> makeRampVerts();

// A unit cylinder (radius 0.5, y in [-0.5,0.5], axle Y). Double-sided.
std::vector<fitzel::Vertex> makeCylinderYVerts(int seg = 20);

// A UV sphere of radius 0.5 centred at the origin. CCW outward winding.
std::vector<fitzel::Vertex> makeSphereVerts(int stacks = 24, int slices = 32);
