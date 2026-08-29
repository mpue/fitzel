#pragma once

#include <glm/glm.hpp>

#include "EditMesh.hpp"

// Painting texture layers onto a modelled mesh -- the terrain paint brush, aimed
// at an object instead of the ground.
//
// The terrain can be painted cheaply because its paint lives in a world-space
// grid over a heightfield: one vec4 per cell, sampled by (x, z). None of that
// carries to an object, which has overhangs, a transform of its own and no
// privileged axis, so the weights live on the mesh's own CORNERS instead (see
// EditMesh::paint) and travel with it.
//
// That leaves resolution as the real problem: a box has eight corners, and a
// brush stroke across eight corners is one flat blob. So the brush SPLITS the
// faces it passes over until they are fine enough to hold the stroke -- refine()
// below -- which is why painting a wall gives you a wall with more faces than it
// had. The alternative is a texture per object and a UV unwrap to go with it,
// which is a different (and much larger) tool.
//
// Everything here is world-space: the caller hands over the entity's model
// matrix and a brush in metres, so a scaled object paints with the brush the
// user sees rather than a brush stretched by its scale.
namespace meshpaint {

// The paintable layers, matching the terrain's: one vertex vec4 of weights.
inline constexpr int kPaintSlots = 4;

// Where a ray meets the mesh.
struct Hit {
    int       face  = -1;         // -1 when the ray missed
    float     t     = 0.0f;       // distance along the ray
    glm::vec3 world{0.0f};        // the point itself
};

// Ray against the mesh's triangles, fanned exactly the way build() fans them so
// what is painted is what is drawn. False when the ray misses.
bool pick(const EditMesh& m, const glm::mat4& model,
          const glm::vec3& ro, const glm::vec3& rd, Hit& out);

// Split the quads under the brush until their edges are shorter than `target`
// metres, so the stroke has corners to land on. Bounded twice over -- at most
// `maxFaces` faces in the mesh afterwards, and only faces the brush actually
// covers -- because an unbounded subdivide under a held mouse button is how a
// wall becomes a million triangles. Returns the number of faces split.
int refine(EditMesh& m, const glm::mat4& model, const glm::vec3& center,
           float radius, float target, int maxFaces);

// One dab: raise `layer` toward 1 over the brush and fade the other three, or
// (erase) fade all four toward 0, which puts the object's own material back.
// Same convergence as the terrain brush, so overlapping dabs settle instead of
// overshooting. True if any corner changed.
bool dab(EditMesh& m, const glm::mat4& model, const glm::vec3& center,
         float radius, int layer, float amount, bool erase);

// Drop every weight (the panel's "Clear paint"). True if there was any.
bool clear(EditMesh& m);

} // namespace meshpaint
